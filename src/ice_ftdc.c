// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2018-2026 Intel Corporation */

/* ice_ftdc.c - Full Time Diagnostic Data Capture
 *
 * Collects firmware debug dump data and driver state into a single
 * dev_coredump blob that is exposed through sysfs at
 * /sys/class/devcoredump/devcdN/data
 */

#include "ice.h"
#include "ice_ftdc.h"
#include "ice_lib.h"
#include <linux/timekeeping.h>

#if IS_ENABLED(CONFIG_DEV_COREDUMP)
#include <linux/devcoredump.h>
#endif

/* FW dump constants (matching the AQ protocol for internal data dump) */
#define ICE_FTDC_FW_DATA_SIZE	4096
#define ICE_FTDC_FW_LAST_IDX	0xFFFFFFFF
#define ICE_FTDC_FW_LAST_ID2	0xFFFF

/* Cluster IDs to dump from FW - the most useful ones for diagnostics */
static const u16 ice_ftdc_cluster_ids[] = {
	ICE_AQC_DBG_DUMP_CLUSTER_ID_SW,
	ICE_AQC_DBG_DUMP_CLUSTER_ID_TXSCHED,
	ICE_AQC_DBG_DUMP_CLUSTER_ID_PROFILES,
	ICE_AQC_DBG_DUMP_CLUSTER_ID_LINK,
	ICE_AQC_DBG_DUMP_CLUSTER_ID_DCB,
	ICE_AQC_DBG_DUMP_CLUSTER_ID_L2P,
	ICE_AQC_DBG_DUMP_CLUSTER_ID_QUEUE_MNG,
};

/**
 * ice_ftdc_reset_type_str - return string name for a reset type
 * @reset_type: the reset type value
 */
static const char *ice_ftdc_reset_type_str(enum ice_reset_req reset_type)
{
	switch (reset_type) {
	case ICE_RESET_PFR:
		return "PFR";
	case ICE_RESET_CORER:
		return "CORER";
	case ICE_RESET_GLOBR:
		return "GLOBR";
	case ICE_RESET_EMPR:
		return "EMPR";
	default:
		return "UNKNOWN";
	}
}

/**
 * ice_ftdc_dump_header - write FTDC header section
 * @buf: output buffer
 * @remain: remaining space in buffer
 * @pf: PF device structure
 * @reset_type: type of reset that triggered the dump
 *
 * Returns number of bytes written.
 */
static int ice_ftdc_dump_header(char *buf, int remain, struct ice_pf *pf,
				enum ice_reset_req reset_type)
{
	struct ice_hw *hw = &pf->hw;
	struct timespec64 ts;
	int written = 0;
	int n;

	ktime_get_real_ts64(&ts);

	n = scnprintf(buf + written, remain - written,
		      "[ftdc-header]\n"
		      "magic: 0x%08x\n"
		      "version: %u\n"
		      "timestamp: %lld.%09ld\n"
		      "reset-type: %s\n"
		      "pci-device: %04x:%02x:%02x.%x\n"
		      "device-id: 0x%04x\n"
		      "vendor-id: 0x%04x\n"
		      "subsystem-device-id: 0x%04x\n"
		      "subsystem-vendor-id: 0x%04x\n"
		      "revision-id: 0x%02x\n"
		      "pf-id: %u\n"
		      "\n",
		      ICE_FTDC_MAGIC,
		      ICE_FTDC_VERSION,
		      (long long)ts.tv_sec, ts.tv_nsec,
		      ice_ftdc_reset_type_str(reset_type),
		      pci_domain_nr(pf->pdev->bus),
		      pf->pdev->bus->number,
		      PCI_SLOT(pf->pdev->devfn),
		      PCI_FUNC(pf->pdev->devfn),
		      hw->device_id,
		      hw->vendor_id,
		      hw->subsystem_device_id,
		      hw->subsystem_vendor_id,
		      hw->revision_id,
		      hw->pf_id);
	written += n;

	return written;
}

/**
 * ice_ftdc_dump_fw_info - write firmware version information section
 * @buf: output buffer
 * @remain: remaining space in buffer
 * @pf: PF device structure
 *
 * Returns number of bytes written.
 */
static int ice_ftdc_dump_fw_info(char *buf, int remain, struct ice_pf *pf)
{
	struct ice_hw *hw = &pf->hw;
	int written = 0;
	int n;

	n = scnprintf(buf + written, remain - written,
		      "[fw-info]\n"
		      "fw-version: %u.%u.%u.%u\n"
		      "fw-build: %u\n"
		      "api-version: %u.%u.%u.%u\n"
		      "\n",
		      hw->fw_branch, hw->fw_maj_ver,
		      hw->fw_min_ver, hw->fw_patch,
		      hw->fw_build,
		      hw->api_branch, hw->api_maj_ver,
		      hw->api_min_ver, hw->api_patch);
	written += n;

	return written;
}

/**
 * ice_ftdc_dump_driver_state - write driver state section
 * @buf: output buffer
 * @remain: remaining space in buffer
 * @pf: PF device structure
 *
 * Returns number of bytes written.
 */
static int ice_ftdc_dump_driver_state(char *buf, int remain,
				      struct ice_pf *pf)
{
	int written = 0;
	int n;

	n = scnprintf(buf + written, remain - written,
		      "[driver-state]\n"
		      "state-bits: 0x%0*lx\n"
		      "flags-bits: 0x%0*lx\n"
		      "max-pf-txqs: %u\n"
		      "max-pf-rxqs: %u\n"
		      "num-alloc-vsi: %u\n"
		      "num-lan-tx: %u\n"
		      "num-lan-rx: %u\n"
		      "corer-count: %u\n"
		      "globr-count: %u\n"
		      "empr-count: %u\n"
		      "pfr-count: %u\n"
		      "tx-timeout-count: %u\n"
		      "oicr-err-reg: 0x%08x\n"
		      "\n",
		      (int)DIV_ROUND_UP(ICE_STATE_NBITS, 4),
		      pf->state[0],
		      (int)DIV_ROUND_UP(ICE_PF_FLAGS_NBITS, 4),
		      pf->flags[0],
		      pf->max_pf_txqs,
		      pf->max_pf_rxqs,
		      pf->num_alloc_vsi,
		      pf->num_lan_tx,
		      pf->num_lan_rx,
		      pf->corer_count,
		      pf->globr_count,
		      pf->empr_count,
		      pf->pfr_count,
		      pf->tx_timeout_count,
		      pf->oicr_err_reg);
	written += n;

	return written;
}

/**
 * ice_ftdc_dump_port_stats - write port statistics section
 * @buf: output buffer
 * @remain: remaining space in buffer
 * @pf: PF device structure
 *
 * Returns number of bytes written.
 */
static int ice_ftdc_dump_port_stats(char *buf, int remain,
				    struct ice_pf *pf)
{
	struct ice_hw_port_stats *s = &pf->stats;
	int written = 0;
	int n;

	n = scnprintf(buf + written, remain - written,
		      "[port-stats]\n"
		      "rx-bytes: %llu\n"
		      "rx-unicast: %llu\n"
		      "rx-multicast: %llu\n"
		      "rx-broadcast: %llu\n"
		      "rx-discards: %llu\n"
		      "rx-errors: %llu\n"
		      "tx-bytes: %llu\n"
		      "tx-unicast: %llu\n"
		      "tx-multicast: %llu\n"
		      "tx-broadcast: %llu\n"
		      "tx-discards: %llu\n"
		      "tx-errors: %llu\n"
		      "tx-dropped-link-down: %llu\n"
		      "crc-errors: %llu\n"
		      "illegal-bytes: %llu\n"
		      "mac-local-faults: %llu\n"
		      "mac-remote-faults: %llu\n"
		      "rx-undersize: %llu\n"
		      "rx-oversize: %llu\n"
		      "rx-fragments: %llu\n"
		      "rx-jabber: %llu\n"
		      "link-xon-rx: %llu\n"
		      "link-xoff-rx: %llu\n"
		      "link-xon-tx: %llu\n"
		      "link-xoff-tx: %llu\n"
		      "\n",
		      s->eth.rx_bytes,
		      s->eth.rx_unicast,
		      s->eth.rx_multicast,
		      s->eth.rx_broadcast,
		      s->eth.rx_discards,
		      s->eth.rx_errors,
		      s->eth.tx_bytes,
		      s->eth.tx_unicast,
		      s->eth.tx_multicast,
		      s->eth.tx_broadcast,
		      s->eth.tx_discards,
		      s->eth.tx_errors,
		      s->tx_dropped_link_down,
		      s->crc_errors,
		      s->illegal_bytes,
		      s->mac_local_faults,
		      s->mac_remote_faults,
		      s->rx_undersize,
		      s->rx_oversize,
		      s->rx_fragments,
		      s->rx_jabber,
		      s->link_xon_rx,
		      s->link_xoff_rx,
		      s->link_xon_tx,
		      s->link_xoff_tx);
	written += n;

	return written;
}

/**
 * ice_ftdc_dump_vsi_info - write VSI information section
 * @buf: output buffer
 * @remain: remaining space in buffer
 * @pf: PF device structure
 *
 * Returns number of bytes written.
 */
static int ice_ftdc_dump_vsi_info(char *buf, int remain, struct ice_pf *pf)
{
	int written = 0;
	int n;
	u16 i;

	n = scnprintf(buf + written, remain - written, "[vsi-info]\n");
	written += n;

	ice_for_each_vsi(pf, i) {
		struct ice_vsi *vsi = pf->vsi[i];

		if (!vsi)
			continue;

		n = scnprintf(buf + written, remain - written,
			      "vsi-%u: type=%d vsi_num=%u "
			      "txq=%u rxq=%u alloc_txq=%u alloc_rxq=%u "
			      "tx_restart=%u tx_busy=%u "
			      "rx_buf_failed=%u rx_page_failed=%u "
			      "state=0x%lx\n",
			      i, vsi->type, vsi->vsi_num,
			      vsi->num_txq, vsi->num_rxq,
			      vsi->alloc_txq, vsi->alloc_rxq,
			      vsi->tx_restart, vsi->tx_busy,
			      vsi->rx_buf_failed, vsi->rx_page_failed,
			      vsi->state[0]);
		written += n;

		if (remain - written < 256)
			break;
	}

	n = scnprintf(buf + written, remain - written, "\n");
	written += n;

	return written;
}

/**
 * ice_ftdc_dump_fw_clusters - dump FW internal data clusters as hex
 * @buf: output buffer
 * @remain: remaining space in buffer
 * @pf: PF device structure
 *
 * Iterates over key FW debug clusters and dumps their raw data as
 * hex-encoded lines. Each cluster is prefixed with its cluster/table ID.
 *
 * Returns number of bytes written.
 */
static int ice_ftdc_dump_fw_clusters(char *buf, int remain,
				     struct ice_pf *pf)
{
	u8 *data_buf;
	int written = 0;
	int n;
	int i;

	n = scnprintf(buf + written, remain - written, "[fw-dump]\n");
	written += n;

	data_buf = kmalloc(ICE_FTDC_FW_DATA_SIZE, GFP_KERNEL);
	if (!data_buf) {
		n = scnprintf(buf + written, remain - written,
			      "error: failed to allocate FW dump buffer\n\n");
		written += n;
		return written;
	}

	for (i = 0; i < ARRAY_SIZE(ice_ftdc_cluster_ids); i++) {
		u16 cluster_id = ice_ftdc_cluster_ids[i];
		u16 tbl_id = 0;
		u32 blk_idx = 0;
		int blk_count = 0;

		n = scnprintf(buf + written, remain - written,
			      "cluster-%u:\n", cluster_id);
		written += n;

		while (blk_count < 64) {
			u16 next_cluster_id, next_tbl_id, buf_len;
			u32 next_blk_idx;
			int res, j;

			res = ice_aq_get_internal_data(&pf->hw, cluster_id,
						       tbl_id, blk_idx,
						       data_buf,
						       ICE_FTDC_FW_DATA_SIZE,
						       &buf_len,
						       &next_cluster_id,
						       &next_tbl_id,
						       &next_blk_idx, NULL);
			if (res) {
				n = scnprintf(buf + written,
					      remain - written,
					      "error: AQ cmd failed %d\n",
					      res);
				written += n;
				break;
			}

			/* Write table/block header */
			n = scnprintf(buf + written, remain - written,
				      "  tbl=%u blk=%u len=%u: ",
				      tbl_id, blk_idx, buf_len);
			written += n;

			/* Write hex data (limit to keep output reasonable) */
			for (j = 0; j < buf_len && (remain - written) > 4; j++) {
				n = scnprintf(buf + written,
					      remain - written,
					      "%02x", data_buf[j]);
				written += n;
			}
			n = scnprintf(buf + written, remain - written, "\n");
			written += n;

			blk_count++;

			/* Check for end of blocks in this table */
			if (blk_idx == next_blk_idx)
				blk_idx = ICE_FTDC_FW_LAST_IDX;
			else
				blk_idx = next_blk_idx;

			if (blk_idx != ICE_FTDC_FW_LAST_IDX)
				continue;

			/* Move to next table or cluster */
			blk_idx = 0;

			if (next_cluster_id == ICE_FTDC_FW_LAST_ID2)
				break;

			if (cluster_id != next_cluster_id)
				break;

			tbl_id = next_tbl_id;

			/* Safety check for buffer space */
			if (remain - written < 512)
				goto out;
		}
	}

out:
	n = scnprintf(buf + written, remain - written, "\n");
	written += n;

	kfree(data_buf);
	return written;
}

/**
 * ice_ftdc_trigger - capture FTDC data and submit via dev_coredump
 * @pf: PF device structure
 * @reset_type: the type of reset that triggered this capture
 *
 * Allocates a buffer, fills it with both driver state and FW debug
 * data, and hands it to dev_coredumpv() which makes it available at
 * /sys/class/devcoredump/devcdN/data.
 *
 * The buffer is freed automatically by the dev_coredump framework
 * after it has been read or after a timeout.
 */
void ice_ftdc_trigger(struct ice_pf *pf, enum ice_reset_req reset_type)
{
#if IS_ENABLED(CONFIG_DEV_COREDUMP)
	struct device *dev = ice_pf_to_dev(pf);
	size_t buf_size;
	int written = 0;
	char *buf;
	int n;

	buf_size = ICE_FTDC_DRV_BUF_SIZE + ICE_FTDC_FW_BUF_SIZE;
	buf = vzalloc(buf_size);
	if (!buf) {
		dev_warn(dev, "FTDC: failed to allocate dump buffer\n");
		return;
	}

	dev_info(dev, "FTDC: capturing diagnostic data for %s reset\n",
		 ice_ftdc_reset_type_str(reset_type));

	/* Collect driver debug information */
	n = ice_ftdc_dump_header(buf + written, buf_size - written,
				 pf, reset_type);
	written += n;

	n = ice_ftdc_dump_fw_info(buf + written, buf_size - written, pf);
	written += n;

	n = ice_ftdc_dump_driver_state(buf + written, buf_size - written, pf);
	written += n;

	n = ice_ftdc_dump_port_stats(buf + written, buf_size - written, pf);
	written += n;

	n = ice_ftdc_dump_vsi_info(buf + written, buf_size - written, pf);
	written += n;

	/* Collect firmware debug dump data */
	n = ice_ftdc_dump_fw_clusters(buf + written, buf_size - written, pf);
	written += n;

	dev_info(dev, "FTDC: captured %d bytes of diagnostic data\n", written);

	/*
	 * dev_coredumpv takes ownership of the buffer and frees it via vfree.
	 * The data will be accessible at /sys/class/devcoredump/devcdN/data
	 * for 5 minutes (300 seconds) by default.
	 */
	dev_coredumpv(dev, buf, written, GFP_KERNEL);
#endif /* CONFIG_DEV_COREDUMP */
}
