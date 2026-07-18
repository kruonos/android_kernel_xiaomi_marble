/*
 * Copyright (c) 2019-2021 The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef _CFR_DEFS_I_H_
#define _CFR_DEFS_I_H_

#include <wlan_objmgr_cmn.h>
#include <wlan_objmgr_global_obj.h>
#include <wlan_objmgr_psoc_obj.h>
#include <wlan_objmgr_pdev_obj.h>
#include <wlan_objmgr_vdev_obj.h>
#include <wlan_objmgr_peer_obj.h>
#include <qdf_list.h>
#include <qdf_timer.h>
#include <qdf_util.h>
#include <qdf_types.h>
#include <wlan_cfr_utils_api.h>
#include <linux/types.h>

/**
 * wlan_cfr_psoc_obj_create_handler() - psoc object create handler for cfr
 * @psoc - pointer to psoc object
 * @args - void pointer in case it needs arguments
 *
 * Return: status of object creation
 */
QDF_STATUS
wlan_cfr_psoc_obj_create_handler(struct wlan_objmgr_psoc *psoc, void *arg);

/**
 * wlan_cfr_psoc_obj_destroy_handler() - psoc object destroy handler for cfr
 * @psoc - pointer to psoc object
 * @args - void pointer in case it needs arguments
 *
 * Return: status of destroy object
 */
QDF_STATUS
wlan_cfr_psoc_obj_destroy_handler(struct wlan_objmgr_psoc *psoc, void *arg);

/**
 * wlan_cfr_pdev_obj_create_handler() - pdev object create handler for cfr
 * @pdev - pointer to pdev object
 * @args - void pointer in case it needs arguments
 *
 * Return: status of object creation
 */
QDF_STATUS
wlan_cfr_pdev_obj_create_handler(struct wlan_objmgr_pdev *pdev, void *arg);

/**
 * wlan_cfr_pdev_obj_destroy_handler() - pdev object destroy handler for cfr
 * @pdev - pointer to pdev object
 * @args - void pointer in case it needs arguments
 *
 * Return: status of destroy object
 */
QDF_STATUS
wlan_cfr_pdev_obj_destroy_handler(struct wlan_objmgr_pdev *pdev, void *arg);

/**
 * wlan_cfr_peer_obj_create_handler() - peer object create handler for cfr
 * @peer - pointer to peer object
 * @args - void pointer in case it needs arguments
 *
 * Return: status of object creation
 */
QDF_STATUS
wlan_cfr_peer_obj_create_handler(struct wlan_objmgr_peer *peer, void *arg);

/**
 * wlan_cfr_peer_obj_destroy_handler() - peer object destroy handler for cfr
 * @peer - pointer to peer object
 * @args - void pointer in case it needs arguments
 *
 * Return: status ofi destry object
 */
QDF_STATUS
wlan_cfr_peer_obj_destroy_handler(struct wlan_objmgr_peer *peer, void *arg);

/**
 * cfr_streamfs_init() - stream filesystem init
 * @pdev - pointer to pdev object
 *
 * Return: status of fs init
 */
QDF_STATUS
cfr_streamfs_init(struct wlan_objmgr_pdev *pdev);

/**
 * cfr_streamfs_remove() - stream filesystem remove
 * @pdev - pointer to pdev object
 *
 * Return: status of fs remove
 */
QDF_STATUS
cfr_streamfs_remove(struct wlan_objmgr_pdev *pdev);

/**
 * cfr_streamfs_reset() - discard buffered relay data and restart sequencing
 * @pdev: pointer to pdev object
 *
 * Return: status of relay reset
 */
QDF_STATUS
cfr_streamfs_reset(struct wlan_objmgr_pdev *pdev);

/**
 * cfr_streamfs_write() - write to stream filesystem
 * @pa - pointer to pdev_cfr object
 * @write_data - Pointer to data
 * @write_len - data len
 *
 * Return: status of fs write
 */
/*
 * CFRR framed relay ABI.
 *
 * The frame header and lifecycle records use explicit little-endian fields.
 * DBR metadata and RX PPDU evidence retain the validated main-kernel
 * fixed-width layout emitted by the little-endian Marble target. A frame is
 * committed in full or dropped in full, so readers can resynchronize on this
 * magic and account for loss through sequence gaps. Keep existing structure
 * prefixes and fixed sizes stable when extending the research ABI.
 */
#define CFR_STREAMFS_RECORD_MAGIC 0x52524643U /* "CFRR" little-endian */
#define CFR_STREAMFS_RECORD_VERSION 2
#define CFR_STREAMFS_RECORD_V1_HDR_LEN 40U
#define CFR_STREAMFS_MAX_RECORD_SIZE (32U * 1024U)

/* Bounded watchdog defaults and accepted operator-control limits. */
#define CFR_CONTINUOUS_DEFAULT_POLL_MS 50U
#define CFR_CONTINUOUS_DEFAULT_STALL_MS 250U
#define CFR_CONTINUOUS_DEFAULT_DRAIN_MS 75U
#define CFR_CONTINUOUS_DEFAULT_BACKOFF_MS 500U
#define CFR_CONTINUOUS_MIN_POLL_MS 20U
#define CFR_CONTINUOUS_MAX_POLL_MS 1000U
#define CFR_CONTINUOUS_MIN_STALL_MS 100U
#define CFR_CONTINUOUS_MAX_STALL_MS 10000U
#define CFR_CONTINUOUS_MIN_DRAIN_MS 10U
#define CFR_CONTINUOUS_MAX_DRAIN_MS 100U

enum cfr_streamfs_record_type {
	CFR_STREAMFS_RECORD_FINAL = 0,
	CFR_STREAMFS_RECORD_RAW_DBR = 0x80000000U,
	CFR_STREAMFS_RECORD_RX_PPDU = 0x80000001U,
	CFR_STREAMFS_RECORD_DBR_META = 0x80000002U,
	CFR_STREAMFS_RECORD_SESSION_START = 0x80000003U,
	CFR_STREAMFS_RECORD_SESSION_END = 0x80000004U,
	CFR_STREAMFS_RECORD_REARM = 0x80000005U,
};

enum cfr_continuous_rearm_stage {
	CFR_CONTINUOUS_REARM_NONE = 0,
	CFR_CONTINUOUS_REARM_SOFT = 1,
	CFR_CONTINUOUS_REARM_HARD = 2,
	CFR_CONTINUOUS_REARM_BLIND = 3,
};

enum cfr_streamfs_session_state {
	CFR_STREAMFS_SESSION_DISABLED = 0,
	CFR_STREAMFS_SESSION_STARTING,
	CFR_STREAMFS_SESSION_ACTIVE,
	CFR_STREAMFS_SESSION_STOPPING,
	CFR_STREAMFS_SESSION_STOPPED,
	CFR_STREAMFS_SESSION_ERROR,
};

enum cfr_streamfs_stop_reason {
	CFR_STREAMFS_STOP_CAPTURE = 1,
	CFR_STREAMFS_STOP_RELAY_DISABLED,
	CFR_STREAMFS_STOP_RELAY_RESET,
	CFR_STREAMFS_STOP_RESTART,
	CFR_STREAMFS_STOP_ERROR,
	CFR_STREAMFS_STOP_SHUTDOWN,
};

#define CFR_STREAMFS_DBR_META_MAGIC 0x4d524244U /* "DBRM" little-endian */
#define CFR_STREAMFS_DBR_META_VERSION 1

#define CFR_STREAMFS_DBR_META_F_RAW_HDR_VALID 0x00000001U
#define CFR_STREAMFS_DBR_META_F_FREEZE_PRESENT 0x00000002U
#define CFR_STREAMFS_DBR_META_F_MU_PRESENT 0x00000004U
#define CFR_STREAMFS_DBR_META_F_SAMPLE_PRESENT 0x00000008U
#define CFR_STREAMFS_DBR_META_F_FREEZE_FIELDS 0x00000010U
#define CFR_STREAMFS_DBR_META_F_RAW_HEADER_BYTES 0x00000020U

struct cfr_streamfs_dbr_meta_v1 {
	uint32_t magic;
	uint16_t version;
	uint16_t meta_len;
	uint32_t flags;
	uint32_t cookie;
	uint32_t phy_ppdu_id;
	uint32_t paddr_low32;
	uint32_t paddr_high32;
	uint32_t dbr_len;
	uint32_t parsed_len;
	uint16_t dma_hdr_bytes;
	uint16_t dma_hdr_words;
	uint16_t freeze_tlv_len;
	uint16_t mu_rx_user_size;
	uint16_t mu_rx_num_users;
	uint16_t sample_offset;
	uint32_t sample_len;
	uint8_t tag;
	uint8_t upload_done;
	uint8_t capture_type;
	uint8_t preamble_type;
	uint8_t nss;
	uint8_t num_chains;
	uint8_t upload_pkt_bw;
	uint8_t sw_peer_id_valid;
	uint16_t sw_peer_id;
	uint16_t total_bytes;
	uint8_t header_version;
	uint8_t target_id;
	uint8_t cfr_fmt;
	uint8_t mu_rx_data_incl;
	uint8_t freeze_data_incl;
	uint8_t freeze_tlv_version;
	uint8_t decimation_factor;
	uint8_t reserved0;
	uint8_t freeze;
	uint8_t freeze_capture_reason;
	uint8_t freeze_packet_type;
	uint8_t freeze_packet_subtype;
	uint8_t freeze_directed;
	uint8_t freeze_sw_peer_id_valid;
	uint16_t freeze_sw_peer_id;
	uint16_t freeze_phy_ppdu_id;
	uint16_t packet_ta_lower_16;
	uint16_t packet_ta_mid_16;
	uint16_t packet_ta_upper_16;
	uint16_t packet_ra_lower_16;
	uint16_t packet_ra_mid_16;
	uint16_t packet_ra_upper_16;
	uint16_t tsf_word0;
	uint16_t tsf_word1;
	uint16_t tsf_word2;
	uint16_t tsf_word3_or_user_mask_36_32;
	uint16_t user_mask_word0;
	uint16_t user_mask_word1;
	uint16_t user_mask_word2;
	uint32_t raw_header_len;
} __attribute__((__packed__));

struct cfr_streamfs_record_hdr {
	__le32 magic;
	__le16 version;
	__le16 hdr_len;
	__le32 type;
	__le32 flags;
	__le32 seq;
	__le32 payload_len;
	__le32 meta0;
	__le32 meta1;
	__le64 timestamp_ns;
	__le64 session_id;
	__le32 pdev_id;
	__le32 reserved0;
	__le64 reserved1;
} __attribute__((__packed__));

struct cfr_streamfs_session_start_v1 {
	__le16 version;
	__le16 payload_len;
	__le32 chip_type;
	__le32 pdev_id;
	__le32 capture_mode;
	__le32 filter_group_bitmap;
	__le32 capture_duration;
	__le32 capture_interval;
	__le32 relay_subbuf_size;
	__le32 relay_num_subbufs;
	__le32 max_record_size;
	__le32 framing_version;
	__le64 start_timestamp_ns;
	__le32 reserved0;
	__le32 reserved1;
	__le32 reserved2;
} __attribute__((__packed__));

struct cfr_streamfs_session_start_v2 {
	struct cfr_streamfs_session_start_v1 v1;
	__le32 capture_count;
	__le32 capture_interval_mode;
	__le32 continuous_enabled;
	__le32 watchdog_stall_ms;
} __attribute__((__packed__));

struct cfr_streamfs_session_end_v1 {
	__le16 version;
	__le16 payload_len;
	__le32 stop_reason;
	__le32 last_sequence;
	__le32 reserved0;
	__le64 records_attempted;
	__le64 records_committed;
	__le64 records_dropped;
	__le64 bytes_committed;
	__le64 bytes_dropped;
	__le64 correlation_successes;
	__le64 correlation_misses;
	__le64 end_timestamp_ns;
} __attribute__((__packed__));

struct cfr_streamfs_rearm_v1 {
	__le16 version;
	__le16 payload_len;
	__le32 stage;
	__le32 status;
	__le64 epoch;
	__le64 last_ppdu_timestamp_ns;
	__le64 last_dbr_timestamp_ns;
	__le64 rearm_timestamp_ns;
	__le32 stall_threshold_ms;
	__le32 poll_interval_ms;
	__le32 drain_interval_ms;
	__le32 reserved0;
} __attribute__((__packed__));

/**
 * cfr_streamfs_write_record() - write a framed CFR record to streamfs
 * @pa: pointer to pdev_cfr object
 * @type: CFR streamfs record type
 * @meta0: type-specific metadata
 * @meta1: type-specific metadata
 * @head: optional first payload segment
 * @hlen: first payload segment length
 * @data: optional second payload segment
 * @dlen: second payload segment length
 * @tail: optional third payload segment
 * @tlen: third payload segment length
 * @flush: flush the global relay channel before releasing the record lock
 *
 * Return: status of framed streamfs write
 */
QDF_STATUS
cfr_streamfs_write_record(struct pdev_cfr *pa, uint32_t type,
			  uint32_t meta0, uint32_t meta1,
			  const void *head, size_t hlen,
			  const void *data, size_t dlen,
			  const void *tail, size_t tlen,
			  bool flush);

QDF_STATUS cfr_streamfs_begin_session(struct wlan_objmgr_pdev *pdev);

QDF_STATUS cfr_streamfs_set_capture_active(struct wlan_objmgr_pdev *pdev,
					   bool active);

QDF_STATUS
cfr_streamfs_end_session(struct wlan_objmgr_pdev *pdev,
			 enum cfr_streamfs_stop_reason reason,
			 bool disable_relay);

QDF_STATUS
cfr_streamfs_report_reader_stats(struct wlan_objmgr_pdev *pdev,
				 uint64_t sequence_gaps,
				 uint64_t resync_bytes,
				 uint64_t invalid_frames);

QDF_STATUS cfr_streamfs_clear_counters(struct wlan_objmgr_pdev *pdev);

#ifdef WLAN_ENH_CFR_ENABLE
QDF_STATUS cfr_continuous_init(struct pdev_cfr *pa);
void cfr_continuous_deinit(struct pdev_cfr *pa);
QDF_STATUS cfr_continuous_configure(struct wlan_objmgr_pdev *pdev,
				    bool enabled, uint32_t poll_ms,
				    uint32_t stall_ms, uint32_t drain_ms);
void cfr_continuous_update_snapshot(struct pdev_cfr *pa,
				    const struct cfr_rcc_param *rcc);
QDF_STATUS cfr_continuous_start(struct wlan_objmgr_pdev *pdev);
void cfr_continuous_stop(struct wlan_objmgr_pdev *pdev);

static inline void cfr_continuous_note_ppdu(struct pdev_cfr *pa)
{
	if (qdf_unlikely(!pa || !READ_ONCE(pa->continuous_capture_active) ||
			 !READ_ONCE(pa->continuous_enabled)))
		return;

	WRITE_ONCE(pa->continuous_last_ppdu_ns,
		   qdf_ktime_to_ns(qdf_ktime_get()));
}

static inline void cfr_continuous_note_dbr(struct pdev_cfr *pa)
{
	if (qdf_unlikely(!pa || !READ_ONCE(pa->continuous_capture_active) ||
			 !READ_ONCE(pa->continuous_enabled)))
		return;

	WRITE_ONCE(pa->continuous_last_dbr_ns,
		   qdf_ktime_to_ns(qdf_ktime_get()));
}
#else
static inline void cfr_continuous_note_ppdu(struct pdev_cfr *pa) { }
static inline void cfr_continuous_note_dbr(struct pdev_cfr *pa) { }
#endif

/**
 * cfr_streamfs_flush() - flush the write to streamfs
 * @pa - pointer to pdev_cfr object
 *
 * Return: status of fs flush
 */
QDF_STATUS
cfr_streamfs_flush(struct pdev_cfr *pa);

/**
 * cfr_stop_indication() - end the current framed CFR transport session
 * @vdev - pointer to vdev object
 *
 * Return: status of framed session-end write
 */
QDF_STATUS cfr_stop_indication(struct wlan_objmgr_vdev *vdev);

#ifdef WLAN_CFR_PM
/**
 * cfr_prevent_suspend() - Acquire wake lock and prevent suspend
 * @pcfr - pointer to pdev_cfr object
 *
 * Return: QDF status
 */
QDF_STATUS cfr_prevent_suspend(struct pdev_cfr *pcfr);

/**
 * cfr_allow_suspend() - Release wake lock and allow suspend
 * @pcfr - pointer to pdev_cfr object
 *
 * Return: QDF status
 */
QDF_STATUS cfr_allow_suspend(struct pdev_cfr *pcfr);
#endif
#endif
