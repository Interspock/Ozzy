/* SPDX-License-Identifier: MIT */
/*
 * Ozzy USB Audio Driver - Logging Macros
 *
 * Centralized logging with consistent prefix, matching the macOS driver's
 * OzzyLog.h pattern. All messages are prefixed with a component tag for
 * easy filtering in kernel logs (dmesg / journalctl).
 *
 * Copyright (C) 2024 Marcel Bierling <marcel@hackerman.art>
 */

#ifndef OZZY_LOG_H
#define OZZY_LOG_H

#include <linux/device.h>

/* Core driver logging (probe, disconnect, reset) */
#define ozzy_log(dev, fmt, ...)     dev_info(dev, "[ozzy] " fmt, ##__VA_ARGS__)
#define ozzy_err(dev, fmt, ...)     dev_err(dev, "[ozzy] " fmt, ##__VA_ARGS__)
#define ozzy_warn(dev, fmt, ...)    dev_warn(dev, "[ozzy] " fmt, ##__VA_ARGS__)
#define ozzy_dbg(dev, fmt, ...)     dev_dbg(dev, "[ozzy] " fmt, ##__VA_ARGS__)
#define ozzy_notice(dev, fmt, ...)  dev_notice(dev, "[ozzy] " fmt, ##__VA_ARGS__)

/* PCM subsystem logging */
#define ozzy_pcm_log(dev, fmt, ...)     dev_info(dev, "[ozzy-pcm] " fmt, ##__VA_ARGS__)
#define ozzy_pcm_err(dev, fmt, ...)     dev_err(dev, "[ozzy-pcm] " fmt, ##__VA_ARGS__)
#define ozzy_pcm_dbg(dev, fmt, ...)     dev_dbg(dev, "[ozzy-pcm] " fmt, ##__VA_ARGS__)

/* MIDI subsystem logging */
#define ozzy_midi_log(dev, fmt, ...)    dev_info(dev, "[ozzy-midi] " fmt, ##__VA_ARGS__)
#define ozzy_midi_err(dev, fmt, ...)    dev_err(dev, "[ozzy-midi] " fmt, ##__VA_ARGS__)
#define ozzy_midi_notice(dev, fmt, ...) dev_notice(dev, "[ozzy-midi] " fmt, ##__VA_ARGS__)

/* Ploytec device logging */
#define ploytec_log(dev, fmt, ...)  dev_info(dev, "[ploytec] " fmt, ##__VA_ARGS__)
#define ploytec_err(dev, fmt, ...)  dev_err(dev, "[ploytec] " fmt, ##__VA_ARGS__)
#define ploytec_dbg(dev, fmt, ...)  dev_dbg(dev, "[ploytec] " fmt, ##__VA_ARGS__)
#define ploytec_notice(dev, fmt, ...) dev_notice(dev, "[ploytec] " fmt, ##__VA_ARGS__)

#endif /* OZZY_LOG_H */
