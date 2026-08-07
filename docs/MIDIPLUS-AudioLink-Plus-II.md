# MIDIPLUS AudioLink Plus II / AudioLink Plus 4x4

Linux support for the MIDIPLUS AudioLink Plus II (`1acc:0103`) was reverse-engineered from real hardware and the legacy Ploytec OEM driver.

## Status

Validated on real hardware:

- 4-channel capture
- 4-channel playback
- `S24_3LE`
- 44.1, 48, 88.2 and 96 kHz
- dynamic sample-rate switching
- USB reset and full vendor re-handshake between rates
- MIDI IN
- MIDI OUT
- simultaneous audio playback + MIDI IN
- physical MIDI loopback through the device DIN ports

The Linux profile now provides complete 4x4 audio plus duplex MIDI support through the same `snd-usb-ozzy` kernel driver.

## USB topology

The device is vendor-specific rather than USB Audio Class compliant.

Relevant endpoints:

- `0x86` bulk IN: PCM capture
- `0x05` bulk OUT: PCM playback plus embedded MIDI OUT
- `0x83` bulk IN: MIDI IN
- `0x02` isochronous OUT: present in the descriptor, not required for the validated bulk PCM path

Both USB interfaces must be configured with alternate setting 1 for the validated streaming path. The PCM bulk packet size is 512 bytes.

## Vendor handshake and sample-rate control

The Linux driver uses the same Ploytec-style vendor control sequence required by the hardware:

- firmware read: vendor request `0x56` (`'V'`)
- status read/write: vendor request `0x49` (`'I'`)
- sample-rate GET_CUR: `bmRequestType=0xA2`, `bRequest=0x81`, `wValue=0x0100`
- sample-rate SET_CUR: `bmRequestType=0x22`, `bRequest=0x01`, `wValue=0x0100`
  - input rate endpoint index: `0x86`
  - output rate endpoint index: `0x05`

Validated rate encodings are little-endian 24-bit values:

| Rate | Raw bytes |
| ---: | :--- |
| 44100 | `44 AC 00` |
| 48000 | `80 BB 00` |
| 88200 | `88 58 01` |
| 96000 | `00 77 01` |

After the handshake the observed status is normally `0x32`.

## Capture format

Capture uses 512-byte bulk packets from endpoint `0x86`.

Each device input frame is 64 bytes. The existing Ploytec decoder reconstructs 24-bit samples from the bit-per-byte representation. For the AudioLink profile only channels 1 through 4 are exposed to ALSA.

Eight input frames fit in one 512-byte USB packet.

## Playback format

Playback uses 512-byte bulk packets to endpoint `0x05`.

The working 4-channel encoder mirrors the legacy Ploytec `dmaEncode04_C` behavior:

- 10 sample instants per USB packet
- 48 wire bytes per sample instant
- 480 bytes of encoded audio
- byte 480: MIDI OUT byte, or idle byte `0xFD`
- byte 481: sync byte `0xFF`
- remaining bytes are zero padding

For each 24-bit sample instant:

- wire bytes `0..23`: channel 1 in bit 0, channel 3 in bit 1
- wire bytes `24..47`: channel 2 in bit 0, channel 4 in bit 1
- sample bits are emitted MSB-first, bit 23 through bit 0

The device provides USB bulk backpressure, so multiple queued OUT URBs do not multiply the effective audio sample rate.

## MIDI protocol

### MIDI IN

MIDI input is received through bulk endpoint `0x83` in 5-byte transfers:

```text
[slot0] [slot1] [slot2] [slot3] EB
```

The first four positions contain either a standard MIDI byte or `0xFD` when idle. The final byte is the framing byte `0xEB`.

The AudioLink-specific MIDI input decoder removes `0xFD` idle bytes and the trailing `0xEB`, then forwards the remaining byte stream unchanged to ALSA rawmidi. Standard MIDI running status is therefore preserved. Real-hardware tests covered Note On/Off, chords, Program Change and Pitch Bend.

### MIDI OUT

MIDI output is embedded in the same 512-byte bulk packet used for PCM playback on endpoint `0x05`:

```text
offset 480 = one MIDI byte, or 0xFD when idle
offset 481 = 0xFF sync
```

The physical mapping was confirmed by a DIN loopback test:

```text
ALSA rawmidi
  -> Ozzy
  -> EP 0x05 offset 480
  -> AudioLink MIDI OUT DIN
  -> MIDI cable
  -> AudioLink MIDI IN DIN
  -> EP 0x83
  -> Ozzy
  -> ALSA rawmidi
```

A repeated `90 3C 40 90 3C 00` sequence was recovered byte-for-byte through the physical loop.

## ALSA examples

Capture four channels at 44.1 kHz:

```bash
arecord -D hw:1,0 -c 4 -r 44100 -f S24_3LE -d 10 capture.wav
```

Playback four channels at 44.1 kHz:

```bash
aplay -D hw:1,0 -c 4 -r 44100 -f S24_3LE playback.wav
```

List raw MIDI ports:

```bash
amidi -l
```

Monitor MIDI IN:

```bash
amidi -d -p hw:1,0,0
```

Send a MIDI Note On through MIDI OUT:

```bash
amidi -p hw:1,0,0 -S "90 3C 40"
```

If PulseAudio owns the device, playback testing can be done with:

```bash
pasuspender -- aplay -D hw:1,0 -c 4 -r 44100 -f S24_3LE playback.wav
```

## Validation

The AudioLink profile was tested on Linux kernel `5.4.0-216-generic`.

Playback and capture were exercised at all four supported sample rates in sequence. Rate changes performed the expected USB reset, device reinitialization, sample-rate verification and status confirmation without new XRUNs, URB failures, kernel BUGs or Oopses in the validation run.

MIDI IN was validated through ALSA rawmidi with notes, chords, running status, Program Change and Pitch Bend. MIDI OUT was validated with a physical DIN OUT-to-IN loopback, and audio playback was also tested concurrently with MIDI input.

The implementation is intentionally device-specific where required: the AudioLink 4-channel playback packing and MIDI input framing differ from the generic 8-channel Ploytec/Xone paths.
