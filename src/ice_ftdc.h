/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2018-2026 Intel Corporation */

#ifndef _ICE_FTDC_H_
#define _ICE_FTDC_H_

/**
 * FTDC - Full Time Diagnostic Data Capture
 *
 * Captures both firmware and driver debug information into a single
 * dev_coredump blob that can be retrieved from
 * /sys/class/devcoredump/devcdN/data
 *
 * The dump is a text-based format with clearly delimited sections:
 *   [ftdc-header]   - Magic, version, timestamp, device IDs
 *   [fw-info]       - Firmware and API version info
 *   [driver-state]  - PF state flags, queue counts, reset counters
 *   [port-stats]    - Port-level hardware statistics
 *   [vsi-info]      - VSI configuration and per-VSI stats
 *   [fw-dump]       - Raw firmware cluster dump (hex-encoded)
 */

#define ICE_FTDC_MAGIC		0x46544443	/* "FTDC" */
#define ICE_FTDC_VERSION	1

/* Maximum size for the text-based FTDC output buffer */
#define ICE_FTDC_DRV_BUF_SIZE	(64 * 1024)

/* Maximum size for the FW cluster dump section */
#define ICE_FTDC_FW_BUF_SIZE	(4 * 1024 * 1024)

struct ice_pf;
enum ice_reset_req;

void ice_ftdc_trigger(struct ice_pf *pf, enum ice_reset_req reset_type);

#endif /* _ICE_FTDC_H_ */
