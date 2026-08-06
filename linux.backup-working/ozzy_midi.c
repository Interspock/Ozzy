// SPDX-License-Identifier: MIT
/*
 * Ozzy USB Audio Driver - MIDI Subsystem
 *
 * Device-agnostic ALSA rawmidi implementation. Handles URB lifecycle,
 * ALSA rawmidi ops, and the MIDI send buffer. Device-specific MIDI
 * embedding in PCM output packets is handled by the device's fill_midi_out
 * callback, which calls ozzy_midi_consume() to read from the send buffer.
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#include <linux/slab.h>
#include <sound/rawmidi.h>

#include "ozzy.h"
#include "ozzy_log.h"
#include "ozzy_midi.h"

/* ========================================================================
 * MIDI Send Buffer (consumed by device's fill_midi_out)
 * ======================================================================== */

/*
 * ozzy_midi_consume - Read pending MIDI output bytes from the send buffer.
 * Fills with idle_byte when no data is pending. Called from device's
 * fill_midi_out in URB handler context (atomic).
 */
void ozzy_midi_consume(struct midi_runtime *rt, u8 *buffer, int count,
		       u8 idle_byte)
{
	int i;

	for (i = 0; i < count; i++) {
		if (rt->send_pending > 0) {
			buffer[i] = rt->send_buffer[rt->send_count];
			rt->send_count++;
			rt->send_pending--;
		} else {
			rt->send_count = 0;
			buffer[i] = idle_byte;
		}
	}
}

/* ========================================================================
 * URB Handlers
 * ======================================================================== */

/*
 * ozzy_midi_in_urb_handler - MIDI input URB completion handler.
 * Forwards received MIDI bytes to the ALSA rawmidi input substream.
 */
static void ozzy_midi_in_urb_handler(struct urb *usb_urb)
{
	struct midi_urb *in_urb = usb_urb->context;
	struct midi_runtime *rt = in_urb->chip->midi;
	unsigned long flags;
	int ret;

	if (unlikely(usb_urb->status == -ENOENT ||
		     usb_urb->status == -ENODEV ||
		     usb_urb->status == -ECONNRESET ||
		     usb_urb->status == -ESHUTDOWN))
		goto in_fail;

	spin_lock_irqsave(&rt->in_lock, flags);
	if (rt->in)
		snd_rawmidi_receive(rt->in, in_urb->buffer,
				    in_urb->instance.actual_length);
	spin_unlock_irqrestore(&rt->in_lock, flags);

	ret = usb_submit_urb(&in_urb->instance, GFP_ATOMIC);
	if (ret < 0)
		goto in_fail;

	return;

in_fail:
	ozzy_midi_err(&in_urb->chip->dev->dev, "input URB failure\n");
}

/* ========================================================================
 * ALSA Rawmidi Operations
 * ======================================================================== */

static int ozzy_midi_in_open(struct snd_rawmidi_substream *alsa_sub)
{
	return 0;
}

static int ozzy_midi_in_close(struct snd_rawmidi_substream *alsa_sub)
{
	return 0;
}

static int ozzy_midi_out_open(struct snd_rawmidi_substream *alsa_sub)
{
	return 0;
}

static int ozzy_midi_out_close(struct snd_rawmidi_substream *alsa_sub)
{
	return 0;
}

/*
 * ozzy_midi_in_trigger - Enable or disable MIDI input.
 */
static void ozzy_midi_in_trigger(struct snd_rawmidi_substream *alsa_sub, int up)
{
	struct midi_runtime *rt = alsa_sub->rmidi->private_data;
	unsigned long flags;

	spin_lock_irqsave(&rt->in_lock, flags);
	if (up)
		rt->in = alsa_sub;
	else
		rt->in = NULL;
	spin_unlock_irqrestore(&rt->in_lock, flags);
}

/*
 * ozzy_midi_out_trigger - Enable MIDI output and drain data into send buffer.
 */
static void ozzy_midi_out_trigger(struct snd_rawmidi_substream *alsa_sub, int up)
{
	struct midi_runtime *rt = alsa_sub->rmidi->private_data;
	int ret;
	unsigned long flags;

	spin_lock_irqsave(&rt->out_lock, flags);
	if (up) {
		if (rt->out) {
			spin_unlock_irqrestore(&rt->out_lock, flags);
			return;
		}

		ret = snd_rawmidi_transmit(alsa_sub, rt->out_buffer, 64);
		if (ret > 0) {
			if (rt->send_pending > (OZZY_MIDI_SEND_BUF_SIZE - ret)) {
				ozzy_midi_notice(&rt->chip->dev->dev,
					    "send buffer overflow\n");
			} else {
				memcpy(rt->send_buffer + rt->send_pending,
				       rt->out_buffer, ret);
				rt->send_pending += ret;
			}
		}
	} else if (rt->out == alsa_sub) {
		rt->out = NULL;
	}
	spin_unlock_irqrestore(&rt->out_lock, flags);
}

static const struct snd_rawmidi_ops ozzy_midi_out_ops = {
	.open    = ozzy_midi_out_open,
	.close   = ozzy_midi_out_close,
	.trigger = ozzy_midi_out_trigger,
};

static const struct snd_rawmidi_ops ozzy_midi_in_ops = {
	.open    = ozzy_midi_in_open,
	.close   = ozzy_midi_in_close,
	.trigger = ozzy_midi_in_trigger,
};

/* ========================================================================
 * URB Initialization
 * ======================================================================== */

/*
 * ozzy_midi_init_bulk_in_urb - Initialize a single MIDI input URB.
 */
static int ozzy_midi_init_bulk_in_urb(struct midi_urb *urb,
				      struct ozzy_chip *chip, unsigned int ep)
{
	urb->chip = chip;
	usb_init_urb(&urb->instance);

	urb->buffer = kzalloc(OZZY_MIDI_PACKET_SIZE, GFP_KERNEL);
	if (!urb->buffer)
		return -ENOMEM;

	usb_fill_bulk_urb(&urb->instance, chip->dev,
			  usb_rcvbulkpipe(chip->dev, ep),
			  urb->buffer, 9,
			  ozzy_midi_in_urb_handler, urb);

	if (usb_urb_ep_type_check(&urb->instance)) {
		ozzy_midi_err(&chip->dev->dev, "MIDI URB endpoint sanity check failed\n");
		return -EINVAL;
	}

	init_usb_anchor(&urb->submitted);
	return 0;
}

/*
 * ozzy_midi_init_urbs - Initialize and submit all MIDI input URBs.
 * Called during initial probe and after USB device reset (post_reset).
 */
int ozzy_midi_init_urbs(struct ozzy_chip *chip)
{
	struct midi_runtime *rt = chip->midi;
	uint8_t i;
	int ret;

	if (!rt)
		return -EINVAL;

	rt->chip = chip;

	for (i = 0; i < OZZY_MIDI_N_URBS; i++) {
		ret = ozzy_midi_init_bulk_in_urb(&rt->midi_in_urbs[i], chip,
						 chip->info->midi_in_ep);
		if (ret < 0)
			goto error;
	}

	for (i = 0; i < OZZY_MIDI_N_URBS; i++) {
		usb_anchor_urb(&rt->midi_in_urbs[i].instance,
			       &rt->midi_in_urbs[i].submitted);
		ret = usb_submit_urb(&rt->midi_in_urbs[i].instance, GFP_ATOMIC);
		if (ret < 0) {
			ozzy_midi_abort(chip);
			goto error;
		}
	}

	return 0;

error:
	for (i = 0; i < OZZY_MIDI_N_URBS; i++)
		kfree(rt->midi_in_urbs[i].buffer);
	return ret;
}

/*
 * ozzy_midi_kill_urbs - Kill all MIDI URBs.
 */
static void ozzy_midi_kill_urbs(struct midi_runtime *rt)
{
	int i, time;

	for (i = 0; i < OZZY_MIDI_N_URBS; i++) {
		time = usb_wait_anchor_empty_timeout(
			&rt->midi_in_urbs[i].submitted, 100);
		if (!time)
			usb_kill_anchored_urbs(&rt->midi_in_urbs[i].submitted);
		usb_kill_urb(&rt->midi_in_urbs[i].instance);
	}
}

/*
 * ozzy_midi_abort - Stop all MIDI URBs immediately.
 */
void ozzy_midi_abort(struct ozzy_chip *chip)
{
	struct midi_runtime *rt = chip->midi;

	if (rt && rt->active)
		ozzy_midi_kill_urbs(rt);
}

/* ========================================================================
 * Init
 * ======================================================================== */

/*
 * ozzy_midi_init - Create and initialize the MIDI subsystem.
 * Allocates the runtime, registers ALSA rawmidi, and sets up URBs.
 */
int ozzy_midi_init(struct ozzy_chip *chip)
{
	struct snd_rawmidi *midi;
	struct midi_runtime *rt;
	int ret;

	rt = kzalloc(sizeof(struct midi_runtime), GFP_KERNEL);
	if (!rt)
		return -ENOMEM;

	rt->out_buffer = kzalloc(64, GFP_KERNEL);
	if (!rt->out_buffer) {
		kfree(rt);
		return -ENOMEM;
	}

	rt->send_buffer = kzalloc(OZZY_MIDI_SEND_BUF_SIZE, GFP_KERNEL);
	if (!rt->send_buffer) {
		kfree(rt->out_buffer);
		kfree(rt);
		return -ENOMEM;
	}

	rt->chip = chip;
	rt->send_count = 0;
	rt->send_pending = 0;

	spin_lock_init(&rt->in_lock);
	spin_lock_init(&rt->out_lock);

	ret = snd_rawmidi_new(chip->card, chip->dev->product, 0, 1, 1, &midi);
	if (ret < 0) {
		ozzy_midi_err(&chip->dev->dev, "Cannot create MIDI instance\n");
		kfree(rt->send_buffer);
		kfree(rt->out_buffer);
		kfree(rt);
		return ret;
	}

	midi->private_data = rt;

	strscpy(midi->name, chip->dev->product, sizeof(midi->name));
	midi->info_flags = SNDRV_RAWMIDI_INFO_OUTPUT |
			   SNDRV_RAWMIDI_INFO_INPUT |
			   SNDRV_RAWMIDI_INFO_DUPLEX;
	snd_rawmidi_set_ops(midi, SNDRV_RAWMIDI_STREAM_OUTPUT, &ozzy_midi_out_ops);
	snd_rawmidi_set_ops(midi, SNDRV_RAWMIDI_STREAM_INPUT, &ozzy_midi_in_ops);

	rt->instance = midi;
	chip->midi = rt;
	rt->active = true;

	/* Set up and submit MIDI URBs */
	if (chip->info->midi_in_ep) {
		ret = ozzy_midi_init_urbs(chip);
		if (ret < 0) {
			ozzy_midi_err(&chip->dev->dev, "MIDI URB setup failed\n");
			kfree(rt->send_buffer);
			kfree(rt->out_buffer);
			kfree(rt);
			return ret;
		}
	}

	return 0;
}
