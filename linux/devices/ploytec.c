// SPDX-License-Identifier: MIT
/*
 * Ploytec Device Family - Allen & Heath Xone Series
 *
 * Device-specific implementation for the Ploytec USB audio chipset used
 * in Allen & Heath Xone:DB4, DB2, DX, 4D, and Wizard 4 mixers.
 *
 * Handles: vendor request handshake, sample rate control, Ploytec
 * bit-interleaved PCM codec, packet framing with padding gaps, and
 * MIDI byte embedding in PCM output packets.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <linux/slab.h>
#include <linux/usb.h>

#include "../ozzy.h"
#include "../ozzy_log.h"
#include "../ozzy_pcm.h"
#include "../ozzy_midi.h"
#include "ploytec.h"
#include "../../common/devices/ploytec/ploytec_defs.h"
#include "../../common/devices/ploytec/ploytec_protocol.h"
#include "../../common/devices/ploytec/ploytec_codec.h"

/* ALSA frame size: 8 channels x 3 bytes (S24_3LE) */
#define ALSA_FRAME_SIZE   (PLOYTEC_CHANNELS * 3)

/* Number of ALSA bytes per output packet */
#define ALSA_OUT_PKT_SIZE (PLOYTEC_FRAMES_PER_PKT * ALSA_FRAME_SIZE)

/* Number of ALSA bytes per input packet */
#define ALSA_IN_PKT_SIZE  (PLOYTEC_FRAMES_PER_PKT * ALSA_FRAME_SIZE)

/*
 * struct ploytec_private - Ploytec device-specific runtime data.
 *
 * All buffers used with usb_control_msg must be heap-allocated (DMA-safe).
 * Stack buffers cause "transfer buffer is on stack" kernel warnings/crashes.
 */
struct ploytec_private {
	unsigned char firmware_ver[15]; /* raw firmware version response */
	unsigned char status[1];        /* hardware status byte */
	unsigned char xfer_buf[16];     /* reusable DMA-safe buffer for vendor requests */
};

/* Supported sample rates */
static const unsigned int ploytec_rates[] = { 44100, 48000, 88200, 96000 };

/* Forward declarations */
static int ploytec_set_rate(struct ozzy_chip *chip, unsigned int rate_index);

/* ========================================================================
 * Vendor Request Helpers
 * ======================================================================== */

/*
 * ploytec_get_firmware - Read the Ploytec firmware version.
 * Stores the 15-byte response in priv->firmware_ver and logs the version.
 */
static int ploytec_get_firmware(struct ozzy_chip *chip)
{
	struct ploytec_private *priv = chip->private_data;
	int ret;

	ret = usb_control_msg(chip->dev, usb_rcvctrlpipe(chip->dev, 0),
			      PLOYTEC_CMD_FIRMWARE, 0xC0, 0x0000, 0,
			      priv->firmware_ver, 15, 2000);
	if (ret < 0)
		return ret;

	{
		struct ploytec_firmware_version fw =
			ploytec_parse_firmware(priv->firmware_ver);
		ploytec_log(&chip->dev->dev,
			    "firmware: v1.%d.%d (chip ID: 0x%02X, raw: %*ph)\n",
			    fw.major, fw.minor, fw.chip_id,
			    15, priv->firmware_ver);
	}

	return 0;
}

/*
 * ploytec_get_status - Read and decode the AJ Input Selector register.
 * See ploytec_aj_input_status for confirmed vs partially understood bits.
 */
static int ploytec_get_status(struct ozzy_chip *chip)
{
	struct ploytec_private *priv = chip->private_data;
	struct ploytec_aj_input_status st;
	int ret;

	ret = usb_control_msg(chip->dev, usb_rcvctrlpipe(chip->dev, 0),
			      PLOYTEC_CMD_STATUS, 0xC0, 0x0000,
			      PLOYTEC_REG_AJ_INPUT_SEL,
			      priv->status, 1, 2000);
	if (ret < 0)
		return ret;

	ploytec_decode_aj_input(priv->status[0], &st);
	ploytec_log(&chip->dev->dev,
		    "status: 0x%02X [InputSel:%d] [DigLock:%d] "
		    "[mode: %d%d%d%d%d%d]\n",
		    st.raw, st.input_select, st.digital_lock,
		    st.mode7, st.mode6, st.mode5,
		    st.mode4, st.mode3, st.mode0);

	return 0;
}

/*
 * ploytec_get_rate - Read the current hardware sample rate and update chip->current_rate.
 * Returns 0 on success, negative errno on failure.
 */
static int ploytec_get_rate(struct ozzy_chip *chip)
{
	struct ploytec_private *priv = chip->private_data;
	uint32_t rate;
	unsigned int i;
	int ret;

	ret = usb_control_msg(chip->dev, usb_rcvctrlpipe(chip->dev, 0),
			      PLOYTEC_CMD_GET_RATE_REQ, PLOYTEC_CMD_GET_RATE_TYPE,
			      0x0100, 0, priv->xfer_buf, 3, 2000);
	if (ret < 0)
		return ret;

	rate = ploytec_decode_rate(priv->xfer_buf);

	ploytec_log(&chip->dev->dev, "hardware sample rate: %u Hz (raw: %02X %02X %02X)\n",
		    rate, priv->xfer_buf[0], priv->xfer_buf[1], priv->xfer_buf[2]);

	for (i = 0; i < chip->info->num_rates; i++) {
		if (chip->info->rates[i] == rate) {
			chip->current_rate = i;
			return 0;
		}
	}

	ploytec_err(&chip->dev->dev, "unsupported sample rate: %u Hz\n", rate);
	return -EINVAL;
}

/*
 * ploytec_confirm_status - Read-modify-write the AJ Input Selector register.
 *
 * Reads the status byte from vendor request 'I' (wIndex=0), then writes
 * it back with bit 5 set using sign-extension to compute the 16-bit wValue.
 * This completes the handshake and arms the device for streaming.
 */
static int ploytec_confirm_status(struct ozzy_chip *chip)
{
	struct ploytec_private *priv = chip->private_data;
	uint16_t wvalue;
	int ret;

	ret = usb_control_msg(chip->dev, usb_rcvctrlpipe(chip->dev, 0),
			      PLOYTEC_CMD_STATUS, 0xC0, 0x0000,
			      PLOYTEC_REG_AJ_INPUT_SEL,
			      priv->status, 1, 2000);
	if (ret < 0)
		return ret;

	wvalue = ploytec_confirm_wvalue(priv->status[0]);

	ret = usb_control_msg(chip->dev, usb_sndctrlpipe(chip->dev, 0),
			      PLOYTEC_CMD_STATUS, 0x40,
			      wvalue, PLOYTEC_REG_AJ_INPUT_SEL,
			      NULL, 0, 2000);
	if (ret >= 0)
		ploytec_log(&chip->dev->dev,
			    "status confirmed (read=0x%02X, wrote wValue=0x%04X)\n",
			    priv->status[0], wvalue);

	return ret;
}

/* ========================================================================
 * Device Ops Implementation
 * ======================================================================== */

/*
 * ploytec_init - Perform the Ploytec vendor handshake after USB probe.
 * Allocates private data, reads firmware, sets sample rate, confirms status.
 */
static int ploytec_init(struct ozzy_chip *chip)
{
	struct ploytec_private *priv;
	int ret;




    priv = chip->private_data;

    if (!priv) {
        priv = kzalloc(sizeof(*priv), GFP_KERNEL);
        if (!priv)
            return -ENOMEM;

        chip->private_data = priv;
    } else {
        memset(priv->firmware_ver, 0, sizeof(priv->firmware_ver));
        memset(priv->status, 0, sizeof(priv->status));
        memset(priv->xfer_buf, 0, sizeof(priv->xfer_buf));
    }



	ploytec_log(&chip->dev->dev, "--- begin handshake sequence ---\n");

	ploytec_log(&chip->dev->dev, "[1/7] reading firmware version...\n");
	ret = ploytec_get_firmware(chip);
	if (ret < 0)
		goto err;

	ploytec_log(&chip->dev->dev, "[2/7] reading hardware status...\n");
	ret = ploytec_get_status(chip);
	if (ret < 0)
		goto err;

	ploytec_log(&chip->dev->dev, "[3/7] reading current sample rate...\n");
	ret = ploytec_get_rate(chip);
	if (ret < 0)
		goto err;

	ploytec_log(&chip->dev->dev, "[4/7] setting sample rate to %u Hz...\n",
		    chip->info->rates[chip->requested_rate]);
	ret = ploytec_set_rate(chip, chip->requested_rate);
	if (ret < 0)
		goto err;

	ploytec_log(&chip->dev->dev, "[5/7] verifying sample rate...\n");
	ret = ploytec_get_rate(chip);
	if (ret < 0)
		goto err;

	ploytec_log(&chip->dev->dev, "[6/7] reading hardware status...\n");
	ret = ploytec_get_status(chip);
	if (ret < 0)
		goto err;

	ploytec_log(&chip->dev->dev, "[7/7] confirming device status...\n");
	ret = ploytec_confirm_status(chip);
	if (ret < 0)
		goto err;

	ploytec_log(&chip->dev->dev, "--- handshake complete, device ready ---\n");

	return 0;

err:
	ploytec_err(&chip->dev->dev, "handshake failed at step (ret=%d)\n", ret);
	kfree(priv);
	chip->private_data = NULL;
	return ret;
}

/*
 * ploytec_free - Release Ploytec private data.
 */
static void ploytec_free(struct ozzy_chip *chip)
{
	kfree(chip->private_data);
	chip->private_data = NULL;
}

/*
 * ploytec_set_rate - Set the hardware sample rate via vendor requests.
 * Converts the rate to 3 little-endian bytes and sends SET_CUR to both
 * the input (0x86) and output (0x05) endpoints.
 */
static int ploytec_set_rate(struct ozzy_chip *chip, unsigned int rate_index)
{
	struct ploytec_private *priv = chip->private_data;
	unsigned int rate;
	int ret;

	if (rate_index >= chip->info->num_rates)
		return -EINVAL;

	rate = chip->info->rates[rate_index];
	ploytec_encode_rate(rate, priv->xfer_buf);

	ret = usb_control_msg(chip->dev, usb_sndctrlpipe(chip->dev, 0),
			      PLOYTEC_CMD_SET_RATE_REQ, PLOYTEC_CMD_SET_RATE_TYPE,
			      0x0100, PLOYTEC_EP_RATE_IN, priv->xfer_buf, 3, 2000);
	if (ret < 0) return ret;

	ret = usb_control_msg(chip->dev, usb_sndctrlpipe(chip->dev, 0),
			      PLOYTEC_CMD_SET_RATE_REQ, PLOYTEC_CMD_SET_RATE_TYPE,
			      0x0100, PLOYTEC_EP_RATE_OUT, priv->xfer_buf, 3, 2000);
	if (ret < 0) return ret;

	chip->current_rate = rate_index;

	ploytec_log(&chip->dev->dev,
		    "sample rate set: %u Hz (raw: %02X %02X %02X)\n",
		    rate, priv->xfer_buf[0], priv->xfer_buf[1], priv->xfer_buf[2]);

	return 0;
}

/*
 * ploytec_reset - Perform a USB device reset for sample rate changes.
 * The core's pre_reset/post_reset callbacks handle URB teardown/reinit.
 */
static int ploytec_reset(struct ozzy_chip *chip)
{
	ploytec_notice(&chip->dev->dev, "resetting device for sample rate change\n");
	return usb_reset_device(chip->dev);
}

/* ========================================================================
 * PCM Packet Processing (zero-copy, directly between URB and DMA buffers)
 * ======================================================================== */

/*
 * ploytec_process_out_bulk - Encode ALSA audio into a Ploytec bulk output packet.
 *
 * The Ploytec bulk format groups 10 frames (480 bytes) followed by a 32-byte
 * gap (1 MIDI + 1 sync + 30 padding), repeated 4 times for 40 frames total.
 * Total packet size: 4 * (480 + 32) = 2048 bytes.
 */
static unsigned int ploytec_process_out_bulk(struct ozzy_chip *chip,
					     uint8_t *urb_buf, uint8_t *dma_area,
					     unsigned int dma_off,
					     unsigned int pcm_buffer_size)
{
	unsigned int g, f, src_off;

	for (g = 0; g < PLOYTEC_BULK_NUM_SUBPACKETS; g++) {
		const struct ploytec_subpacket *sp = &ploytec_bulk_subpackets[g];

		for (f = 0; f < sp->frame_count; f++) {
			src_off = dma_off + (sp->start_frame + f) * ALSA_FRAME_SIZE;
			if (src_off >= pcm_buffer_size)
				src_off -= pcm_buffer_size;

			ploytec_encode_frame(
				urb_buf + sp->byte_offset + f * PLOYTEC_OUT_FRAME_SIZE,
				dma_area + src_off);
		}
	}

	return ALSA_OUT_PKT_SIZE;
}

/*
 * ploytec_process_out_int - Encode ALSA audio into a Ploytec interrupt output packet.
 *
 * The interrupt format has 2-byte MIDI gaps at frames 9, 19, 29, 39 and
 * different overall framing than bulk. Frame offsets accumulate a 2-byte
 * shift after each group of ~10 frames.
 */
static unsigned int ploytec_process_out_int(struct ozzy_chip *chip,
					    uint8_t *urb_buf, uint8_t *dma_area,
					    unsigned int dma_off,
					    unsigned int pcm_buffer_size)
{
	unsigned int g, f, src_off;

	for (g = 0; g < PLOYTEC_INT_NUM_SUBPACKETS; g++) {
		const struct ploytec_subpacket *sp = &ploytec_int_subpackets[g];

		for (f = 0; f < sp->frame_count; f++) {
			src_off = dma_off + (sp->start_frame + f) * ALSA_FRAME_SIZE;
			if (src_off >= pcm_buffer_size)
				src_off -= pcm_buffer_size;

			ploytec_encode_frame(
				urb_buf + sp->byte_offset + f * PLOYTEC_OUT_FRAME_SIZE,
				dma_area + src_off);
		}
	}

	return ALSA_OUT_PKT_SIZE;
}

/*
 * ploytec_process_out_packet - Dispatch to bulk or interrupt output processing.
 */
static unsigned int ploytec_process_out_packet(struct ozzy_chip *chip,
					       uint8_t *urb_buf, uint8_t *dma_area,
					       unsigned int dma_off,
					       unsigned int pcm_buffer_size)
{
	if (chip->is_bulk)
		return ploytec_process_out_bulk(chip, urb_buf, dma_area,
						dma_off, pcm_buffer_size);
	else
		return ploytec_process_out_int(chip, urb_buf, dma_area,
					       dma_off, pcm_buffer_size);
}

/*
 * ploytec_process_in_packet - Decode a Ploytec input packet into ALSA audio.
 * Zero-copy: reads from URB buffer, writes directly to ALSA DMA area.
 * Handles DMA ring buffer wraparound.
 */
static unsigned int ploytec_process_in_packet(struct ozzy_chip *chip,
					      uint8_t *urb_buf, uint8_t *dma_area,
					      unsigned int dma_off,
					      unsigned int pcm_buffer_size)
{
	unsigned int f;
	unsigned int dst_off;

	for (f = 0; f < PLOYTEC_FRAMES_PER_PKT; f++) {
		dst_off = dma_off + f * ALSA_FRAME_SIZE;
		if (dst_off >= pcm_buffer_size)
			dst_off -= pcm_buffer_size;

		ploytec_decode_frame(dma_area + dst_off,
				     urb_buf + f * PLOYTEC_IN_FRAME_SIZE);
	}

	return ALSA_IN_PKT_SIZE;
}

/* ========================================================================
 * URB Initialization and MIDI Embedding
 * ======================================================================== */

/*
 * ploytec_init_out_urb - Fill an output URB with the Ploytec silence pattern.
 * Sets up the proper padding, sync bytes, and MIDI idle bytes.
 */
static void ploytec_init_out_urb(struct ozzy_chip *chip, uint8_t *buffer)
{
	const struct ploytec_midi_slot *slots;
	unsigned int num_slots, i, j;
	unsigned int pkt_size;

	if (chip->is_bulk) {
		slots = ploytec_bulk_midi_slots;
		num_slots = PLOYTEC_BULK_NUM_MIDI_SLOTS;
		pkt_size = PLOYTEC_BULK_OUT_PKT_SIZE;
	} else {
		slots = ploytec_int_midi_slots;
		num_slots = PLOYTEC_INT_NUM_MIDI_SLOTS;
		pkt_size = PLOYTEC_INT_OUT_PKT_SIZE;
	}

	/* Zero entire packet (silence for PCM, zero for padding) */
	memset(buffer, 0, pkt_size);

	/* Fill all MIDI slot positions with idle byte */
	for (i = 0; i < num_slots; i++)
		for (j = 0; j < slots[i].num_bytes; j++)
			buffer[slots[i].offset + j] = PLOYTEC_MIDI_IDLE_BYTE;

	/* Bulk mode: sync byte at offset 481 within each sub-packet */
	if (chip->is_bulk) {
		for (i = 0; i < PLOYTEC_BULK_NUM_SUBPACKETS; i++)
			buffer[ploytec_bulk_subpackets[i].byte_offset + 481] = 0xFF;
	}
}

/*
 * ploytec_fill_midi_out - Embed pending MIDI bytes into a PCM output packet.
 *
 * Rate limiting: MIDI standard baud rate is 31250 bps (~3125 bytes/sec).
 * At 96kHz with 40 frames/packet, we get ~2400 packets/sec. Using all 4
 * MIDI slots per packet would give 9600 bytes/sec -- way over the limit.
 *
 * We only use the first MIDI byte position in the first sub-packet,
 * limiting output to ~2400 bytes/sec at 96kHz (safely below 3125 bps).
 * Remaining slots are filled with the idle byte (0xFD).
 * This matches the macOS driver's approach.
 */
/*
 * ploytec_fill_midi_out - Embed pending MIDI bytes into a PCM output packet.
 *
 * Rate limiting: MIDI standard baud rate is 31250 bps (~3125 bytes/sec).
 * At 96kHz with 40 frames/packet, we get ~2400 packets/sec. Using all
 * MIDI slots per packet would exceed the MIDI baud rate.
 *
 * We only consume 1 MIDI byte from the first slot, filling all other
 * slots with the idle byte (0xFD). This limits output to ~2400 bytes/sec
 * at 96kHz, safely below 3125 bps. Matches the macOS driver's approach.
 */
static void ploytec_fill_midi_out(struct ozzy_chip *chip, uint8_t *urb_buf)
{
	const struct ploytec_midi_slot *slots;
	unsigned int num_slots, i, j;
	struct midi_runtime *rt = chip->midi;

	if (!rt)
		return;

	if (chip->is_bulk) {
		slots = ploytec_bulk_midi_slots;
		num_slots = PLOYTEC_BULK_NUM_MIDI_SLOTS;
	} else {
		slots = ploytec_int_midi_slots;
		num_slots = PLOYTEC_INT_NUM_MIDI_SLOTS;
	}

	/* First slot: consume 1 actual MIDI byte (rate-limited) */
	ozzy_midi_consume(rt, urb_buf + slots[0].offset, 1, PLOYTEC_MIDI_IDLE_BYTE);

	/* Fill remaining bytes in first slot with idle */
	for (j = 1; j < slots[0].num_bytes; j++)
		urb_buf[slots[0].offset + j] = PLOYTEC_MIDI_IDLE_BYTE;

	/* All other slots: idle bytes only */
	for (i = 1; i < num_slots; i++)
		for (j = 0; j < slots[i].num_bytes; j++)
			urb_buf[slots[i].offset + j] = PLOYTEC_MIDI_IDLE_BYTE;
}

/*
 * ploytec_get_out_packet_size - Return output packet size for bulk or interrupt.
 */
static unsigned int ploytec_get_out_packet_size(struct ozzy_chip *chip, bool is_bulk)
{
	return is_bulk ? PLOYTEC_BULK_OUT_PKT_SIZE : PLOYTEC_INT_OUT_PKT_SIZE;
}

/* ========================================================================
 * Exported Device Descriptor and Operations
 * ======================================================================== */

#define AUDIOLINK_CHANNELS          4
#define AUDIOLINK_IN_PACKET_SIZE    512
#define AUDIOLINK_OUT_PACKET_SIZE   512
#define AUDIOLINK_IN_FRAME_SIZE     64
#define AUDIOLINK_FRAMES_PER_PACKET 8
#define AUDIOLINK_ALSA_FRAME_SIZE   (AUDIOLINK_CHANNELS * 3)
#define AUDIOLINK_ALSA_PACKET_SIZE  \
    (AUDIOLINK_FRAMES_PER_PACKET * AUDIOLINK_ALSA_FRAME_SIZE)

#define AUDIOLINK_OUT_FRAMES        10
#define AUDIOLINK_OUT_FRAME_SIZE    48
#define AUDIOLINK_ALSA_OUT_FRAME    (AUDIOLINK_CHANNELS * 3)

/*
 * MIDIPLUS AudioLink Plus II / Ploytec bulk 4-channel encoder.
 *
 * This mirrors the official driver's dmaEncode04_C routine:
 *
 *   10 sample instants per USB bulk packet
 *   48 bytes per sample instant
 *
 *   dst[ 0..23] = CH1 in bit 0, CH3 in bit 1
 *   dst[24..47] = CH2 in bit 0, CH4 in bit 1
 *
 * The 24 significant sample bits are emitted MSB first (bit 23 -> bit 0).
 * ALSA supplies packed S24_3LE, so each channel is reconstructed as an
 * unsigned 24-bit word solely for bit extraction.
 */
static uint32_t audiolink_get_s24le(const uint8_t *p)
{
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16);
}

static void audiolink_encode_frame(uint8_t *dst, const uint8_t *src)
{
    uint32_t ch1 = audiolink_get_s24le(src + 0);
    uint32_t ch2 = audiolink_get_s24le(src + 3);
    uint32_t ch3 = audiolink_get_s24le(src + 6);
    uint32_t ch4 = audiolink_get_s24le(src + 9);
    unsigned int i;

    for (i = 0; i < 24; i++) {
        unsigned int shift = 23 - i;

        dst[i] =
            (uint8_t)(((ch1 >> shift) & 0x01) |
                      (((ch3 >> shift) & 0x01) << 1));

        dst[24 + i] =
            (uint8_t)(((ch2 >> shift) & 0x01) |
                      (((ch4 >> shift) & 0x01) << 1));
    }
}

static unsigned int audiolink_process_out_packet(
    struct ozzy_chip *chip,
    uint8_t *urb_buf,
    uint8_t *dma_area,
    unsigned int dma_off,
    unsigned int pcm_buffer_size)
{
    unsigned int f;
    unsigned int src_off;

    memset(urb_buf, 0, AUDIOLINK_OUT_PACKET_SIZE);

    for (f = 0; f < AUDIOLINK_OUT_FRAMES; f++) {
        uint8_t frame[12];
        uint8_t *src;

        src_off = dma_off + f * AUDIOLINK_ALSA_OUT_FRAME;

        while (src_off >= pcm_buffer_size)
            src_off -= pcm_buffer_size;

        if (src_off + AUDIOLINK_ALSA_OUT_FRAME <= pcm_buffer_size) {
            src = dma_area + src_off;
        } else {
            unsigned int first = pcm_buffer_size - src_off;

            memcpy(frame, dma_area + src_off, first);
            memcpy(frame + first, dma_area,
                   AUDIOLINK_ALSA_OUT_FRAME - first);
            src = frame;
        }

        audiolink_encode_frame(
            urb_buf + f * AUDIOLINK_OUT_FRAME_SIZE,
            src
        );
    }

    /*
     * 10 * 48 = 480 bytes audio.
     * Official Ploytec bulk trailer / idle framing.
     */
    urb_buf[480] = PLOYTEC_MIDI_IDLE_BYTE;
    urb_buf[481] = 0xFF;

    return AUDIOLINK_OUT_FRAMES * AUDIOLINK_ALSA_OUT_FRAME;
}

static unsigned int audiolink_process_in_packet(
    struct ozzy_chip *chip,
    uint8_t *urb_buf,
    uint8_t *dma_area,
    unsigned int dma_off,
    unsigned int pcm_buffer_size)
{
    unsigned int f;
    unsigned int dst_off;
    uint8_t decoded[24];

    for (f = 0; f < AUDIOLINK_FRAMES_PER_PACKET; f++) {
        ploytec_decode_frame(
            decoded,
            urb_buf + f * AUDIOLINK_IN_FRAME_SIZE
        );

        dst_off = dma_off + f * AUDIOLINK_ALSA_FRAME_SIZE;

        if (dst_off >= pcm_buffer_size)
            dst_off -= pcm_buffer_size;

        if (dst_off + AUDIOLINK_ALSA_FRAME_SIZE <= pcm_buffer_size) {
            memcpy(dma_area + dst_off,
                   decoded,
                   AUDIOLINK_ALSA_FRAME_SIZE);
        } else {
            unsigned int first = pcm_buffer_size - dst_off;

            memcpy(dma_area + dst_off, decoded, first);
            memcpy(dma_area,
                   decoded + first,
                   AUDIOLINK_ALSA_FRAME_SIZE - first);
        }
    }

    return AUDIOLINK_ALSA_PACKET_SIZE;
}

static void audiolink_init_out_urb(
    struct ozzy_chip *chip,
    uint8_t *buffer)
{
    memset(buffer, 0, AUDIOLINK_OUT_PACKET_SIZE);
    buffer[480] = PLOYTEC_MIDI_IDLE_BYTE;
    buffer[481] = 0xFF;
}

static unsigned int audiolink_get_out_packet_size(
    struct ozzy_chip *chip,
    bool is_bulk)
{
    return AUDIOLINK_OUT_PACKET_SIZE;
}



/*
 * AudioLink MIDI IN framing: each USB transfer is composed of 5-byte records.
 * The first four bytes are MIDI slots where 0xFD means idle; byte 4 is a
 * fixed 0xEB framing byte. Compact valid MIDI bytes in-place and preserve
 * running status exactly as sent by the hardware.
 */
static unsigned int audiolink_process_midi_in_packet(
    struct ozzy_chip *chip, uint8_t *buffer, unsigned int length)
{
    unsigned int base, slot, out = 0;

    (void)chip;

    for (base = 0; base + 5 <= length; base += 5) {
        for (slot = 0; slot < 4; slot++) {
            uint8_t byte = buffer[base + slot];

            if (byte != PLOYTEC_MIDI_IDLE_BYTE)
                buffer[out++] = byte;
        }
    }

    return out;
}

/*
 * AudioLink MIDI OUT framing: one MIDI byte is embedded at offset 480 of
 * each 512-byte EP05 bulk packet. Offset 481 is the fixed 0xFF sync byte.
 * This layout was confirmed with a physical MIDI OUT -> MIDI IN loopback.
 */
static void audiolink_fill_midi_out(struct ozzy_chip *chip, uint8_t *urb_buf)
{
    struct midi_runtime *rt = chip->midi;

    if (!rt)
        return;

    ozzy_midi_consume(rt, urb_buf + 480, 1, PLOYTEC_MIDI_IDLE_BYTE);
    urb_buf[481] = 0xFF;
}

const struct ozzy_device_info ploytec_info = {
	.name                  = "Ploytec Xone",
	.playback_channels     = PLOYTEC_CHANNELS,
	.capture_channels      = PLOYTEC_CHANNELS,
	.out_packet_size       = PLOYTEC_BULK_OUT_PKT_SIZE,
	.in_packet_size        = PLOYTEC_IN_PKT_SIZE,
	.frames_per_out_packet = PLOYTEC_FRAMES_PER_PKT,
	.frames_per_in_packet  = PLOYTEC_FRAMES_PER_PKT,
	.out_ep                = PLOYTEC_EP_PCM_OUT,
	.in_ep                 = PLOYTEC_EP_PCM_IN,
	.alsa_format           = SNDRV_PCM_FMTBIT_S24_3LE,
	.bytes_per_sample      = 3,
	.midi_in_ep            = PLOYTEC_EP_MIDI_IN,
	.midi_out_embedded     = true,
	.num_interfaces        = PLOYTEC_NUM_INTERFACES,
	.alt_setting           = PLOYTEC_ALT_SETTING,
	.rates                 = ploytec_rates,
	.num_rates             = ARRAY_SIZE(ploytec_rates),
	.rates_mask            = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000,
	.rate_min              = 44100,
	.rate_max              = 96000,
};

const struct ozzy_device_ops ploytec_ops = {
	.init                = ploytec_init,
	.free                = ploytec_free,
	.set_rate            = ploytec_set_rate,
	.reset               = ploytec_reset,
	.process_out_packet  = ploytec_process_out_packet,
	.process_in_packet   = ploytec_process_in_packet,
	.init_out_urb        = ploytec_init_out_urb,
	.fill_midi_out       = ploytec_fill_midi_out,
	.process_midi_in_packet = NULL,
	.get_out_packet_size = ploytec_get_out_packet_size,
};

const struct ozzy_device_info audiolink_info = {
    .name                  = "MIDIPLUS AudioLink Plus II",

    .playback_channels     = 4,
    .capture_channels      = 4,

    .out_packet_size       = AUDIOLINK_OUT_PACKET_SIZE,
    .in_packet_size        = AUDIOLINK_IN_PACKET_SIZE,

    .frames_per_out_packet = 10,
    .frames_per_in_packet  = 8,

    .out_ep                = 0x05,
    .in_ep                 = 0x06,

    .alsa_format           = SNDRV_PCM_FMTBIT_S24_3LE,
    .bytes_per_sample      = 3,

    .midi_in_ep            = 0x03,
    .midi_out_embedded     = true,

    .num_interfaces        = 2,
    .alt_setting           = 1,

    .rates                 = ploytec_rates,
    .num_rates             = ARRAY_SIZE(ploytec_rates),

    .rates_mask            =
        SNDRV_PCM_RATE_44100 |
        SNDRV_PCM_RATE_48000 |
        SNDRV_PCM_RATE_88200 |
        SNDRV_PCM_RATE_96000,

    .rate_min              = 44100,
    .rate_max              = 96000,
};

const struct ozzy_device_ops audiolink_ops = {
    /*
     * Todo el handshake Ploytec ya está confirmado
     * físicamente en nuestra placa.
     */
    .init                = ploytec_init,
    .free                = ploytec_free,
    .set_rate            = ploytec_set_rate,
    .reset               = ploytec_reset,

    .process_out_packet  = audiolink_process_out_packet,

    .process_in_packet   = audiolink_process_in_packet,

    .init_out_urb        = audiolink_init_out_urb,

    .fill_midi_out       = audiolink_fill_midi_out,
    .process_midi_in_packet = audiolink_process_midi_in_packet,

    .get_out_packet_size = audiolink_get_out_packet_size,
};