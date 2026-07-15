/*
 * Copyright (c) 2019-2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2022 Qualcomm Innovation Center, Inc. All rights reserved.
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

#include <cfr_defs_i.h>
#include <linux/build_bug.h>
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/overflow.h>
#include <linux/preempt.h>
#include <linux/seq_file.h>
#include <qdf_types.h>
#include <qdf_time.h>
#include <wlan_objmgr_pdev_obj.h>
#include <wlan_objmgr_vdev_obj.h>
#include <wlan_objmgr_peer_obj.h>
#include <wlan_cfr_tgt_api.h>
#include <wdi_event.h>
#include <qdf_streamfs.h>
#include <target_if.h>
#include <target_if_direct_buf_rx_api.h>
#ifdef WLAN_ENH_CFR_ENABLE
#include <target_if_cfr_enh.h>
#include <target_if_cfr_6490.h>
#endif
#include <wlan_osif_priv.h>
#include <cfg_ucfg_api.h>
#include "cfr_cfg.h"
#ifdef WLAN_CFR_PM
#include "host_diag_core_event.h"
#endif

#ifdef WLAN_ENH_CFR_ENABLE
static uint64_t cfr_continuous_age_ns(uint64_t now_ns, uint64_t timestamp_ns)
{
	if (!timestamp_ns || now_ns < timestamp_ns)
		return 0;

	return now_ns - timestamp_ns;
}
#endif

static const char *cfr_streamfs_session_state_name(uint8_t state)
{
	switch (state) {
	case CFR_STREAMFS_SESSION_DISABLED:
		return "disabled";
	case CFR_STREAMFS_SESSION_STARTING:
		return "starting";
	case CFR_STREAMFS_SESSION_ACTIVE:
		return "active";
	case CFR_STREAMFS_SESSION_STOPPING:
		return "stopping";
	case CFR_STREAMFS_SESSION_STOPPED:
		return "stopped";
	case CFR_STREAMFS_SESSION_ERROR:
		return "error";
	default:
		return "unknown";
	}
}

static int cfr_debugfs_status_show(struct seq_file *s, void *unused)
{
	struct pdev_cfr *pa = s->private;

	seq_printf(s, "pdev_id=%u\n", wlan_objmgr_pdev_get_pdev_id(pa->pdev_obj));
	seq_printf(s, "chip_type=%u\n", READ_ONCE(pa->chip_type));
	seq_printf(s, "cfr_capable=%u\n", READ_ONCE(pa->is_cfr_capable));
	seq_printf(s, "relay_enabled=%u\n",
		   READ_ONCE(pa->streamfs_record_enabled));
	seq_printf(s, "netlink_enabled=%u\n", READ_ONCE(pa->netlink_enabled));
#ifdef WLAN_ENH_CFR_ENABLE
	seq_printf(s, "rcc_capable=%u\n", READ_ONCE(pa->is_cfr_rcc_capable));
	seq_printf(s, "ppdu_subscribed=%u\n", READ_ONCE(pa->ppdu_subscribed));
	seq_printf(s, "dbr_registered=%u\n", READ_ONCE(pa->dbr_registered));
	seq_printf(s, "capture_mode_direct_ftm=%u\n",
		   pa->rcc_param.m_directed_ftm);
	seq_printf(s, "capture_mode_all_ftm_ack=%u\n",
		   pa->rcc_param.m_all_ftm_ack);
	seq_printf(s, "capture_mode_direct_ndpa=%u\n",
		   pa->rcc_param.m_ndpa_ndp_directed);
	seq_printf(s, "capture_mode_all_ndpa=%u\n",
		   pa->rcc_param.m_ndpa_ndp_all);
	seq_printf(s, "capture_mode_ta_ra=%u\n",
		   pa->rcc_param.m_ta_ra_filter);
	seq_printf(s, "capture_mode_all_packet=%u\n",
		   pa->rcc_param.m_all_packet);
	seq_printf(s, "filter_group_bitmap=0x%x\n",
		   READ_ONCE(pa->rcc_param.filter_group_bitmap));
	seq_printf(s, "capture_duration=%u\n",
		   READ_ONCE(pa->rcc_param.capture_duration));
	seq_printf(s, "capture_interval=%u\n",
		   READ_ONCE(pa->rcc_param.capture_interval));
	seq_printf(s, "capture_count_supported=%u\n",
		   READ_ONCE(pa->is_cap_interval_mode_sel_support));
	seq_printf(s, "capture_count=%u\n",
		   pa->rcc_param.capture_count + 1U);
	seq_printf(s, "capture_interval_mode=%u\n",
		   pa->rcc_param.capture_intval_mode_sel);
#endif

	return 0;
}

static int cfr_debugfs_relay_stats_show(struct seq_file *s, void *unused)
{
	struct pdev_cfr *pa = s->private;
	struct {
		uint32_t subbuf_size, num_subbufs, last_sequence;
		uint32_t last_type, last_length;
		int32_t last_error;
		uint64_t attempted, committed, bytes, dropped, dropped_bytes;
		uint64_t reserve_fail, invalid_len, gaps, disabled;
		uint64_t drop_final, drop_raw, drop_rx, drop_meta, drop_session;
		uint64_t rearm_records, drop_rearm;
		uint64_t reader_gaps, reader_resync, reader_invalid;
	} snapshot;

	qdf_spin_lock_bh(&pa->streamfs_record_lock);
	snapshot.subbuf_size = pa->subbuf_size;
	snapshot.num_subbufs = pa->num_subbufs;
	snapshot.last_sequence = pa->streamfs_record_seq;
	snapshot.last_type = pa->streamfs_record_last_type;
	snapshot.last_length = pa->streamfs_record_last_len;
	snapshot.last_error = pa->streamfs_record_last_status;
	snapshot.attempted = pa->streamfs_record_attempt_cnt;
	snapshot.committed = pa->streamfs_record_write_cnt;
	snapshot.bytes = pa->streamfs_record_bytes;
	snapshot.dropped = pa->streamfs_record_drop_cnt;
	snapshot.dropped_bytes = pa->streamfs_record_drop_bytes;
	snapshot.reserve_fail = pa->streamfs_record_reserve_fail_cnt;
	snapshot.invalid_len = pa->streamfs_record_invalid_len_cnt;
	snapshot.gaps = pa->streamfs_sequence_gap_cnt;
	snapshot.disabled = pa->streamfs_record_disabled_cnt;
	snapshot.drop_final = pa->streamfs_drop_final_cnt;
	snapshot.drop_raw = pa->streamfs_drop_raw_dbr_cnt;
	snapshot.drop_rx = pa->streamfs_drop_rx_ppdu_cnt;
	snapshot.drop_meta = pa->streamfs_drop_dbr_meta_cnt;
	snapshot.drop_session = pa->streamfs_drop_session_cnt;
	snapshot.rearm_records = pa->streamfs_record_rearm_cnt;
	snapshot.drop_rearm = pa->streamfs_drop_rearm_cnt;
	snapshot.reader_gaps = pa->streamfs_reader_sequence_gaps;
	snapshot.reader_resync = pa->streamfs_reader_resync_bytes;
	snapshot.reader_invalid = pa->streamfs_reader_invalid_frames;
	qdf_spin_unlock_bh(&pa->streamfs_record_lock);

	seq_printf(s, "framing_version=%u\n", CFR_STREAMFS_RECORD_VERSION);
	seq_printf(s, "max_record_size=%u\n", CFR_STREAMFS_MAX_RECORD_SIZE);
	seq_printf(s, "subbuf_size=%u\n", snapshot.subbuf_size);
	seq_printf(s, "num_subbufs=%u\n", snapshot.num_subbufs);
	seq_printf(s, "relay_memory_bytes=%llu\n",
		   (unsigned long long)snapshot.subbuf_size * snapshot.num_subbufs);
	seq_printf(s, "relay_staging_bytes=%u\n",
		   CFR_STREAMFS_MAX_RECORD_SIZE);
	seq_printf(s, "netlink_staging_bytes=%u\n",
		   CFR_STREAMFS_MAX_RECORD_SIZE);
	seq_printf(s, "memory_bytes=%llu\n",
		   (unsigned long long)snapshot.subbuf_size * snapshot.num_subbufs +
		   (2U * CFR_STREAMFS_MAX_RECORD_SIZE));
	seq_printf(s, "frames_attempted=%llu\n", snapshot.attempted);
	seq_printf(s, "frames_committed=%llu\n", snapshot.committed);
	seq_printf(s, "bytes_committed=%llu\n", snapshot.bytes);
	seq_printf(s, "frames_dropped=%llu\n", snapshot.dropped);
	seq_printf(s, "bytes_dropped=%llu\n", snapshot.dropped_bytes);
	seq_printf(s, "reservation_failures=%llu\n", snapshot.reserve_fail);
	seq_printf(s, "invalid_internal_lengths=%llu\n", snapshot.invalid_len);
	seq_printf(s, "sequence_gaps_expected=%llu\n", snapshot.gaps);
	seq_printf(s, "drops_final=%llu\n", snapshot.drop_final);
	seq_printf(s, "drops_raw_dbr=%llu\n", snapshot.drop_raw);
	seq_printf(s, "drops_rx_ppdu=%llu\n", snapshot.drop_rx);
	seq_printf(s, "drops_dbr_meta=%llu\n", snapshot.drop_meta);
	seq_printf(s, "drops_session=%llu\n", snapshot.drop_session);
	seq_printf(s, "rearm_records=%llu\n", snapshot.rearm_records);
	seq_printf(s, "drops_rearm=%llu\n", snapshot.drop_rearm);
	seq_printf(s, "disabled_writes=%llu\n", snapshot.disabled);
	seq_printf(s, "reader_sequence_gaps=%llu\n", snapshot.reader_gaps);
	seq_printf(s, "reader_resync_bytes=%llu\n", snapshot.reader_resync);
	seq_printf(s, "reader_invalid_frames=%llu\n", snapshot.reader_invalid);
	seq_printf(s, "last_sequence=%u\n", snapshot.last_sequence);
	seq_printf(s, "last_type=0x%x\n", snapshot.last_type);
	seq_printf(s, "last_length=%u\n", snapshot.last_length);
	seq_printf(s, "last_error=%d\n", snapshot.last_error);

	return 0;
}

static int cfr_debugfs_session_show(struct seq_file *s, void *unused)
{
	struct pdev_cfr *pa = s->private;
	struct {
		uint8_t state, reason;
		uint64_t id, count, resets, start_ns, end_ns;
		uint64_t attempted, committed, dropped, bytes, dropped_bytes;
	} snapshot;

	qdf_spin_lock_bh(&pa->streamfs_record_lock);
	snapshot.state = pa->streamfs_session_state;
	snapshot.reason = pa->streamfs_last_stop_reason;
	snapshot.id = pa->streamfs_session_id;
	snapshot.count = pa->streamfs_session_count;
	snapshot.resets = pa->streamfs_session_reset_cnt;
	snapshot.start_ns = pa->streamfs_session_start_ns;
	snapshot.end_ns = pa->streamfs_session_end_ns;
	snapshot.attempted = pa->streamfs_session_records_attempted;
	snapshot.committed = pa->streamfs_session_records_committed;
	snapshot.dropped = pa->streamfs_session_records_dropped;
	snapshot.bytes = pa->streamfs_session_bytes_committed;
	snapshot.dropped_bytes = pa->streamfs_session_bytes_dropped;
	qdf_spin_unlock_bh(&pa->streamfs_record_lock);

	seq_printf(s, "state=%s\n",
		   cfr_streamfs_session_state_name(snapshot.state));
	seq_printf(s, "session_id=%llu\n", snapshot.id);
	seq_printf(s, "session_count=%llu\n", snapshot.count);
	seq_printf(s, "session_resets=%llu\n", snapshot.resets);
	seq_printf(s, "start_timestamp_ns=%llu\n", snapshot.start_ns);
	seq_printf(s, "end_timestamp_ns=%llu\n", snapshot.end_ns);
	seq_printf(s, "last_stop_reason=%u\n", snapshot.reason);
	seq_printf(s, "records_attempted=%llu\n", snapshot.attempted);
	seq_printf(s, "records_committed=%llu\n", snapshot.committed);
	seq_printf(s, "records_dropped=%llu\n", snapshot.dropped);
	seq_printf(s, "bytes_committed=%llu\n", snapshot.bytes);
	seq_printf(s, "bytes_dropped=%llu\n", snapshot.dropped_bytes);

	return 0;
}

static int cfr_debugfs_correlation_show(struct seq_file *s, void *unused)
{
	struct pdev_cfr *pa = s->private;

#ifdef WLAN_ENH_CFR_ENABLE
	uint32_t idx, pending_dbr = 0, pending_rx = 0;

	qdf_spin_lock_bh(&pa->lut_lock);
	for (idx = 0; idx < pa->lut_num; idx++) {
		if (!pa->lut[idx])
			continue;
		pending_dbr += pa->lut[idx]->dbr_recv ? 1 : 0;
		pending_rx += pa->lut[idx]->tx_recv ? 1 : 0;
	}
	qdf_spin_unlock_bh(&pa->lut_lock);

	seq_printf(s, "pending_dbr=%u\n", pending_dbr);
	seq_printf(s, "pending_rx=%u\n", pending_rx);
	seq_printf(s, "successful_matches=%llu\n", READ_ONCE(pa->ppdu_match_cnt));
	seq_printf(s, "ppdu_mismatches=%llu\n", READ_ONCE(pa->ppdu_mismatch_cnt));
	seq_printf(s, "address_mismatches=%llu\n",
		   READ_ONCE(pa->rx_history_miss_paddr_mismatch_cnt));
	seq_printf(s, "history_timeouts=%llu\n",
		   READ_ONCE(pa->rx_history_miss_stale_cnt));
	seq_printf(s, "history_overwrites=%llu\n",
		   READ_ONCE(pa->rx_history_overwrite_cnt));
	seq_printf(s, "stale_entries=%llu\n", READ_ONCE(pa->rx_history_stale_cnt));
	seq_printf(s, "discarded_at_stop=%llu\n",
		   READ_ONCE(pa->reset_dbr_release_ok_cnt));
	seq_printf(s, "correlation_misses=%llu\n",
		   READ_ONCE(pa->rx_history_miss_cnt));
#else
	seq_puts(s, "enhanced_cfr=disabled\n");
#endif

	return 0;
}

static int cfr_debugfs_continuous_show(struct seq_file *s, void *unused)
{
	struct pdev_cfr *pa = s->private;

#ifdef WLAN_ENH_CFR_ENABLE
	uint64_t now_ns, ppdu_ns, dbr_ns;

	qdf_mutex_acquire(&pa->continuous_config_lock);
	ppdu_ns = pa->continuous_last_ppdu_ns;
	dbr_ns = pa->continuous_last_dbr_ns;
	now_ns = qdf_ktime_to_ns(qdf_ktime_get());
	seq_printf(s, "enabled=%u\n", pa->continuous_enabled);
	seq_printf(s, "capture_active=%u\n", pa->continuous_capture_active);
	seq_printf(s, "snapshot_valid=%u\n", pa->continuous_snapshot_valid);
	seq_printf(s, "capture_count_supported=%u\n",
		   pa->is_cap_interval_mode_sel_support);
	seq_printf(s, "capture_interval_mode=%u\n",
		   pa->continuous_rcc_snapshot.capture_intval_mode_sel);
	seq_printf(s, "capture_count=%u\n",
		   pa->continuous_rcc_snapshot.capture_count + 1U);
	seq_printf(s, "capture_duration_us=%u\n",
		   pa->continuous_rcc_snapshot.capture_duration);
	seq_printf(s, "capture_interval_us=%u\n",
		   pa->continuous_rcc_snapshot.capture_interval);
	seq_printf(s, "poll_ms=%u\n", pa->continuous_poll_ms);
	seq_printf(s, "stall_ms=%u\n", pa->continuous_stall_ms);
	seq_printf(s, "drain_ms=%u\n", pa->continuous_drain_ms);
	seq_printf(s, "backoff_ms=%u\n", pa->continuous_backoff_ms);
	seq_printf(s, "last_ppdu_age_ms=%llu\n",
		   cfr_continuous_age_ns(now_ns, ppdu_ns) / NSEC_PER_MSEC);
	seq_printf(s, "last_dbr_age_ms=%llu\n",
		   cfr_continuous_age_ns(now_ns, dbr_ns) / NSEC_PER_MSEC);
	seq_printf(s, "rearm_stage=%u\n", pa->continuous_rearm_stage);
	seq_printf(s, "rearm_epoch=%llu\n", pa->continuous_rearm_epoch);
	seq_printf(s, "stall_events=%llu\n", pa->continuous_stall_cnt);
	seq_printf(s, "soft_rearms=%llu\n", pa->continuous_soft_rearm_cnt);
	seq_printf(s, "hard_rearms=%llu\n", pa->continuous_hard_rearm_cnt);
	seq_printf(s, "blind_rearms=%llu\n", pa->continuous_blind_rearm_cnt);
	seq_printf(s, "blind_retry_pending=%u\n",
		   pa->continuous_blind_retry_pending);
	seq_printf(s, "lut_resets=%llu\n", pa->continuous_lut_reset_cnt);
	seq_printf(s, "lut_reset_failures=%llu\n",
		   pa->continuous_lut_reset_fail_cnt);
	seq_printf(s, "dp_cycles=%llu\n", pa->continuous_dp_cycle_cnt);
	seq_printf(s, "dp_cycle_failures=%llu\n",
		   pa->continuous_dp_cycle_fail_cnt);
	seq_printf(s, "rearm_failures=%llu\n", pa->continuous_rearm_fail_cnt);
	seq_printf(s, "last_rearm_status=%d\n", pa->continuous_last_status);
	seq_printf(s, "last_disable_status=%d\n",
		   pa->continuous_last_disable_status);
	seq_printf(s, "last_dp_disable_status=%d\n",
		   pa->continuous_last_dp_disable_status);
	seq_printf(s, "last_lut_reset_status=%d\n",
		   pa->continuous_last_lut_reset_status);
	seq_printf(s, "last_dp_enable_status=%d\n",
		   pa->continuous_last_dp_enable_status);
	seq_printf(s, "last_enable_status=%d\n",
		   pa->continuous_last_enable_status);
	qdf_mutex_release(&pa->continuous_config_lock);
#else
	seq_puts(s, "enhanced_cfr=disabled\n");
#endif

	return 0;
}

#define CFR_DEBUGFS_FOPS(_name) \
	static int cfr_debugfs_##_name##_open(struct inode *inode, struct file *file) \
	{ \
		struct pdev_cfr *pa = inode->i_private; \
		int ret; \
		if (!pa || !pa->pdev_obj || \
		    QDF_IS_STATUS_ERROR(wlan_objmgr_pdev_try_get_ref( \
					pa->pdev_obj, WLAN_CFR_ID))) \
			return -ENODEV; \
		ret = single_open(file, cfr_debugfs_##_name##_show, pa); \
		if (ret) \
			wlan_objmgr_pdev_release_ref(pa->pdev_obj, WLAN_CFR_ID); \
		return ret; \
	} \
	static int cfr_debugfs_##_name##_release(struct inode *inode, \
						   struct file *file) \
	{ \
		struct seq_file *seq = file->private_data; \
		struct pdev_cfr *pa = seq ? seq->private : NULL; \
		struct wlan_objmgr_pdev *pdev = pa ? pa->pdev_obj : NULL; \
		int ret = single_release(inode, file); \
		if (pdev) \
			wlan_objmgr_pdev_release_ref(pdev, WLAN_CFR_ID); \
		return ret; \
	} \
	static const struct file_operations cfr_debugfs_##_name##_fops = { \
		.owner = THIS_MODULE, \
		.open = cfr_debugfs_##_name##_open, \
		.read = seq_read, \
		.llseek = seq_lseek, \
		.release = cfr_debugfs_##_name##_release, \
	}

CFR_DEBUGFS_FOPS(status);
CFR_DEBUGFS_FOPS(relay_stats);
CFR_DEBUGFS_FOPS(session);
CFR_DEBUGFS_FOPS(correlation);
CFR_DEBUGFS_FOPS(continuous);

static QDF_STATUS cfr_streamfs_begin_session_locked(struct pdev_cfr *pa);
static QDF_STATUS
cfr_streamfs_end_session_locked(struct pdev_cfr *pa,
				enum cfr_streamfs_stop_reason reason,
				bool disable_relay);

#ifdef WLAN_ENH_CFR_ENABLE
/*
 * Bounded continuous-capture recovery
 *
 * RX PPDU and DBR callbacks only publish monotonic evidence timestamps. This
 * delayed-work state machine performs firmware transactions in process
 * context: soft RCC resubmit, hard disable/drain/DP-cycle/LUT-reset/re-enable,
 * and at most one blind hard retry when a successful hard rearm yields no new
 * evidence. Generation checks make stop and reconfiguration cancel-safe.
 */
static void cfr_continuous_emit_rearm(struct pdev_cfr *pa, uint32_t stage,
				      QDF_STATUS status, uint64_t now_ns)
{
	struct cfr_streamfs_rearm_v1 event = {0};

	event.version = cpu_to_le16(1);
	event.payload_len = cpu_to_le16(sizeof(event));
	event.stage = cpu_to_le32(stage);
	event.status = cpu_to_le32((uint32_t)status);
	event.epoch = cpu_to_le64(pa->continuous_rearm_epoch);
	event.last_ppdu_timestamp_ns = cpu_to_le64(
		READ_ONCE(pa->continuous_last_ppdu_ns));
	event.last_dbr_timestamp_ns = cpu_to_le64(
		READ_ONCE(pa->continuous_last_dbr_ns));
	event.rearm_timestamp_ns = cpu_to_le64(now_ns);
	event.stall_threshold_ms = cpu_to_le32(pa->continuous_stall_ms);
	event.poll_interval_ms = cpu_to_le32(pa->continuous_poll_ms);
	event.drain_interval_ms = cpu_to_le32(pa->continuous_drain_ms);

	(void)cfr_streamfs_write_record(pa, CFR_STREAMFS_RECORD_REARM,
					stage,
					(uint32_t)pa->continuous_rearm_epoch,
					&event, sizeof(event), NULL, 0,
					NULL, 0, true);
}

static uint32_t
cfr_continuous_effective_stall_ms(struct pdev_cfr *pa,
				  const struct cfr_rcc_param *active)
{
	uint32_t interval_ms, minimum_ms;

	if (!active->capture_intval_mode_sel)
		return pa->continuous_stall_ms;

	interval_ms = DIV_ROUND_UP(active->capture_interval, 1000U);
	minimum_ms = interval_ms + (2U * pa->continuous_poll_ms);

	return QDF_MAX(pa->continuous_stall_ms, minimum_ms);
}

static void cfr_continuous_disable_snapshot(struct cfr_rcc_param *disabled,
					    const struct cfr_rcc_param *active)
{
	qdf_mem_copy(disabled, active, sizeof(*disabled));
	disabled->m_directed_ftm = 0;
	disabled->m_all_ftm_ack = 0;
	disabled->m_ndpa_ndp_directed = 0;
	disabled->m_ndpa_ndp_all = 0;
	disabled->m_ta_ra_filter = 0;
	disabled->m_all_packet = 0;
	disabled->filter_group_bitmap = 0;
	disabled->num_grp_tlvs = 0;
	disabled->modified_in_curr_session = 0;
}

static void cfr_continuous_rearm_worker(void *context)
{
	struct pdev_cfr *pa = context;
	struct wlan_objmgr_pdev *pdev;
	struct cfr_rcc_param active, disabled;
	uint64_t now_ns, ppdu_ns, dbr_ns, stall_ns, hard_complete_ns;
	uint64_t ppdu_age_ns, dbr_age_ns, hard_complete_age_ns;
	uint64_t completion_ns = 0;
	uint64_t generation;
	uint32_t stage, next_delay;
	uint32_t drain_ms;
	bool canceled = false;
	bool reset_attempted = false;
	bool dp_cycle_attempted = false;
	bool blind_recovery = false;
	QDF_STATUS status, reset_status = QDF_STATUS_SUCCESS;
	QDF_STATUS disable_status = QDF_STATUS_SUCCESS;
	QDF_STATUS dp_disable_status = QDF_STATUS_SUCCESS;
	QDF_STATUS dp_enable_status = QDF_STATUS_SUCCESS;
	QDF_STATUS enable_status = QDF_STATUS_SUCCESS;

	if (!pa || !READ_ONCE(pa->continuous_work_initialized))
		return;

	pdev = pa->pdev_obj;
	if (!pdev || QDF_IS_STATUS_ERROR(
			wlan_objmgr_pdev_try_get_ref(pdev, WLAN_CFR_ID)))
		return;

	if (!READ_ONCE(pa->continuous_capture_active) ||
	    !READ_ONCE(pa->continuous_enabled) ||
	    !READ_ONCE(pa->continuous_snapshot_valid))
		goto out;

	qdf_mutex_acquire(&pa->continuous_config_lock);
	if (!READ_ONCE(pa->continuous_capture_active) ||
	    !READ_ONCE(pa->continuous_enabled) ||
	    !READ_ONCE(pa->continuous_snapshot_valid)) {
		qdf_mutex_release(&pa->continuous_config_lock);
		goto out;
	}

	qdf_mem_copy(&active, &pa->continuous_rcc_snapshot, sizeof(active));
	ppdu_ns = READ_ONCE(pa->continuous_last_ppdu_ns);
	dbr_ns = READ_ONCE(pa->continuous_last_dbr_ns);
	hard_complete_ns = pa->continuous_hard_complete_ns;
	now_ns = qdf_ktime_to_ns(qdf_ktime_get());
	ppdu_age_ns = cfr_continuous_age_ns(now_ns, ppdu_ns);
	dbr_age_ns = cfr_continuous_age_ns(now_ns, dbr_ns);
	hard_complete_age_ns =
		cfr_continuous_age_ns(now_ns, hard_complete_ns);
	stall_ns = (uint64_t)cfr_continuous_effective_stall_ms(pa, &active) *
		   NSEC_PER_MSEC;
	if (pa->continuous_blind_retry_pending) {
		if ((ppdu_ns && ppdu_ns > hard_complete_ns) ||
		    (dbr_ns && dbr_ns > hard_complete_ns)) {
			pa->continuous_blind_retry_pending = 0;
		} else if (hard_complete_ns &&
			   hard_complete_age_ns >= stall_ns) {
			pa->continuous_blind_retry_pending = 0;
			blind_recovery = true;
		} else {
			next_delay = pa->continuous_poll_ms;
			qdf_mutex_release(&pa->continuous_config_lock);
			goto reschedule;
		}
	}

	/* No PPDU traffic means there is no evidence that firmware is stalled. */
	if (!blind_recovery &&
	    (!ppdu_ns || ppdu_age_ns > stall_ns ||
	     (dbr_ns && dbr_age_ns < stall_ns))) {
		if (dbr_ns != pa->continuous_last_rearm_dbr_ns)
			pa->continuous_rearm_stage = CFR_CONTINUOUS_REARM_NONE;
		next_delay = pa->continuous_poll_ms;
		qdf_mutex_release(&pa->continuous_config_lock);
		goto reschedule;
	}

	if (dbr_ns != READ_ONCE(pa->continuous_last_rearm_dbr_ns))
		pa->continuous_rearm_stage = CFR_CONTINUOUS_REARM_NONE;

	stage = blind_recovery ? CFR_CONTINUOUS_REARM_BLIND :
		(pa->continuous_rearm_stage == CFR_CONTINUOUS_REARM_NONE ?
		 CFR_CONTINUOUS_REARM_SOFT : CFR_CONTINUOUS_REARM_HARD);
	generation = pa->continuous_generation;
	next_delay = stage == CFR_CONTINUOUS_REARM_SOFT ?
		cfr_continuous_effective_stall_ms(pa, &active) :
		pa->continuous_backoff_ms;
	drain_ms = pa->continuous_drain_ms;
	qdf_mutex_release(&pa->continuous_config_lock);

	/* Serialize firmware command transactions without blocking diagnostics. */
	qdf_mutex_acquire(&pa->continuous_fw_lock);
	qdf_mutex_acquire(&pa->continuous_config_lock);
	if (!pa->continuous_capture_active || !pa->continuous_enabled ||
	    !pa->continuous_snapshot_valid ||
	    generation != pa->continuous_generation) {
		qdf_mutex_release(&pa->continuous_config_lock);
		qdf_mutex_release(&pa->continuous_fw_lock);
		goto out;
	}
	if ((!blind_recovery && READ_ONCE(pa->continuous_last_dbr_ns) != dbr_ns) ||
	    (blind_recovery &&
	     (READ_ONCE(pa->continuous_last_ppdu_ns) > hard_complete_ns ||
	      READ_ONCE(pa->continuous_last_dbr_ns) > hard_complete_ns))) {
		pa->continuous_rearm_stage = CFR_CONTINUOUS_REARM_NONE;
		next_delay = pa->continuous_poll_ms;
		qdf_mutex_release(&pa->continuous_config_lock);
		qdf_mutex_release(&pa->continuous_fw_lock);
		goto reschedule;
	}
	pa->continuous_stall_cnt++;
	pa->continuous_rearm_epoch++;
	pa->continuous_last_rearm_ns = now_ns;
	pa->continuous_last_rearm_dbr_ns = dbr_ns;
	qdf_mutex_release(&pa->continuous_config_lock);

	if (stage == CFR_CONTINUOUS_REARM_SOFT) {
		status = tgt_cfr_config_rcc(pdev, &active);
	} else {
		cfr_continuous_disable_snapshot(&disabled, &active);
		disable_status = tgt_cfr_config_rcc(pdev, &disabled);
		if (QDF_IS_STATUS_SUCCESS(disable_status)) {
			qdf_sleep(drain_ms);
			qdf_mutex_acquire(&pa->continuous_config_lock);
			canceled = !pa->continuous_capture_active ||
				   !pa->continuous_enabled ||
				   generation != pa->continuous_generation;
			qdf_mutex_release(&pa->continuous_config_lock);
			if (!canceled) {
				dp_cycle_attempted = true;
				dp_disable_status =
					target_if_cfr_set_dp_pipeline(pdev, false);
				reset_attempted = true;
				reset_status = target_if_cfr_reset_lut_enh(pdev);
				dp_enable_status =
					target_if_cfr_set_dp_pipeline(pdev, true);
			}
		}
		/* Restore active RCC even when a concurrent control canceled work. */
		enable_status = tgt_cfr_config_rcc(pdev, &active);
		if (QDF_IS_STATUS_ERROR(enable_status)) {
			qdf_sleep(pa->continuous_poll_ms);
			enable_status = tgt_cfr_config_rcc(pdev, &active);
		}
		completion_ns = qdf_ktime_to_ns(qdf_ktime_get());

		status = disable_status;
		if (QDF_IS_STATUS_SUCCESS(status) &&
		    QDF_IS_STATUS_ERROR(dp_disable_status))
			status = dp_disable_status;
		if (QDF_IS_STATUS_SUCCESS(status) &&
		    QDF_IS_STATUS_ERROR(reset_status))
			status = reset_status;
		if (QDF_IS_STATUS_SUCCESS(status) &&
		    QDF_IS_STATUS_ERROR(dp_enable_status))
			status = dp_enable_status;
		if (QDF_IS_STATUS_SUCCESS(status) &&
		    QDF_IS_STATUS_ERROR(enable_status))
			status = enable_status;
	}
	if (!completion_ns)
		completion_ns = qdf_ktime_to_ns(qdf_ktime_get());

	qdf_mutex_acquire(&pa->continuous_config_lock);
	if (stage == CFR_CONTINUOUS_REARM_SOFT) {
		pa->continuous_soft_rearm_cnt++;
		pa->continuous_rearm_stage = CFR_CONTINUOUS_REARM_SOFT;
	} else {
		if (stage == CFR_CONTINUOUS_REARM_BLIND)
			pa->continuous_blind_rearm_cnt++;
		else
			pa->continuous_hard_rearm_cnt++;
		pa->continuous_last_disable_status = disable_status;
		pa->continuous_last_dp_disable_status = dp_disable_status;
		pa->continuous_last_lut_reset_status = reset_status;
		pa->continuous_last_dp_enable_status = dp_enable_status;
		pa->continuous_last_enable_status = enable_status;
		if (reset_attempted)
			pa->continuous_lut_reset_cnt++;
		if (reset_attempted && QDF_IS_STATUS_ERROR(reset_status))
			pa->continuous_lut_reset_fail_cnt++;
		if (dp_cycle_attempted)
			pa->continuous_dp_cycle_cnt++;
		if (dp_cycle_attempted &&
		    (QDF_IS_STATUS_ERROR(dp_disable_status) ||
		     QDF_IS_STATUS_ERROR(dp_enable_status)))
			pa->continuous_dp_cycle_fail_cnt++;
		pa->continuous_rearm_stage = CFR_CONTINUOUS_REARM_HARD;
		if (!canceled && QDF_IS_STATUS_SUCCESS(status) &&
		    stage == CFR_CONTINUOUS_REARM_HARD) {
			pa->continuous_hard_complete_ns = completion_ns;
			pa->continuous_blind_retry_pending = 1;
		} else {
			pa->continuous_blind_retry_pending = 0;
		}
	}
	pa->continuous_last_status = canceled ? QDF_STATUS_E_CANCELED : status;
	if (!canceled && QDF_IS_STATUS_ERROR(status))
		pa->continuous_rearm_fail_cnt++;
	qdf_mutex_release(&pa->continuous_config_lock);
	if (!canceled)
		cfr_continuous_emit_rearm(pa, stage, status, completion_ns);
	qdf_mutex_release(&pa->continuous_fw_lock);

reschedule:
	if (READ_ONCE(pa->continuous_capture_active) &&
	    READ_ONCE(pa->continuous_enabled))
		(void)qdf_delayed_work_start(&pa->continuous_rearm_work,
					     next_delay);
out:
	wlan_objmgr_pdev_release_ref(pdev, WLAN_CFR_ID);
}

QDF_STATUS cfr_continuous_init(struct pdev_cfr *pa)
{
	QDF_STATUS status;

	if (!pa)
		return QDF_STATUS_E_INVAL;

	qdf_mutex_create(&pa->continuous_config_lock);
	qdf_mutex_create(&pa->continuous_lifecycle_lock);
	qdf_mutex_create(&pa->continuous_fw_lock);
	status = qdf_delayed_work_create(&pa->continuous_rearm_work,
					 cfr_continuous_rearm_worker, pa);
	if (QDF_IS_STATUS_ERROR(status)) {
		qdf_mutex_destroy(&pa->continuous_fw_lock);
		qdf_mutex_destroy(&pa->continuous_lifecycle_lock);
		qdf_mutex_destroy(&pa->continuous_config_lock);
		return status;
	}

	pa->continuous_work_initialized = 1;
	pa->continuous_poll_ms = CFR_CONTINUOUS_DEFAULT_POLL_MS;
	pa->continuous_stall_ms = CFR_CONTINUOUS_DEFAULT_STALL_MS;
	pa->continuous_drain_ms = CFR_CONTINUOUS_DEFAULT_DRAIN_MS;
	pa->continuous_backoff_ms = CFR_CONTINUOUS_DEFAULT_BACKOFF_MS;
	return QDF_STATUS_SUCCESS;
}

void cfr_continuous_deinit(struct pdev_cfr *pa)
{
	if (!pa || !READ_ONCE(pa->continuous_work_initialized))
		return;

	qdf_mutex_acquire(&pa->continuous_lifecycle_lock);
	qdf_mutex_acquire(&pa->continuous_config_lock);
	pa->continuous_capture_active = 0;
	pa->continuous_enabled = 0;
	pa->continuous_generation++;
	pa->continuous_blind_retry_pending = 0;
	pa->continuous_hard_complete_ns = 0;
	qdf_mutex_release(&pa->continuous_config_lock);
	qdf_delayed_work_destroy(&pa->continuous_rearm_work);
	WRITE_ONCE(pa->continuous_work_initialized, 0);
	qdf_mutex_release(&pa->continuous_lifecycle_lock);
	qdf_mutex_destroy(&pa->continuous_fw_lock);
	qdf_mutex_destroy(&pa->continuous_lifecycle_lock);
	qdf_mutex_destroy(&pa->continuous_config_lock);
}

QDF_STATUS cfr_continuous_configure(struct wlan_objmgr_pdev *pdev,
				    bool enabled, uint32_t poll_ms,
				    uint32_t stall_ms, uint32_t drain_ms)
{
	struct pdev_cfr *pa;
	bool active;

	if (!pdev || poll_ms < CFR_CONTINUOUS_MIN_POLL_MS ||
	    poll_ms > CFR_CONTINUOUS_MAX_POLL_MS ||
	    stall_ms < CFR_CONTINUOUS_MIN_STALL_MS ||
	    stall_ms > CFR_CONTINUOUS_MAX_STALL_MS ||
	    stall_ms < 2U * poll_ms ||
	    drain_ms < CFR_CONTINUOUS_MIN_DRAIN_MS ||
	    drain_ms > CFR_CONTINUOUS_MAX_DRAIN_MS)
		return QDF_STATUS_E_INVAL;

	pa = wlan_objmgr_pdev_get_comp_private_obj(pdev, WLAN_UMAC_COMP_CFR);
	if (!pa || !READ_ONCE(pa->continuous_work_initialized))
		return QDF_STATUS_E_FAILURE;

	qdf_mutex_acquire(&pa->continuous_lifecycle_lock);
	qdf_mutex_acquire(&pa->continuous_config_lock);
	pa->continuous_enabled = 0;
	pa->continuous_generation++;
	pa->continuous_blind_retry_pending = 0;
	pa->continuous_hard_complete_ns = 0;
	qdf_mutex_release(&pa->continuous_config_lock);
	(void)qdf_delayed_work_stop_sync(&pa->continuous_rearm_work);
	qdf_mutex_acquire(&pa->continuous_config_lock);
	active = pa->continuous_capture_active;
	pa->continuous_enabled = enabled;
	pa->continuous_poll_ms = poll_ms;
	pa->continuous_stall_ms = stall_ms;
	pa->continuous_drain_ms = drain_ms;
	pa->continuous_generation++;
	qdf_mutex_release(&pa->continuous_config_lock);
	if (enabled && active)
		(void)qdf_delayed_work_start(&pa->continuous_rearm_work, poll_ms);
	qdf_mutex_release(&pa->continuous_lifecycle_lock);

	return QDF_STATUS_SUCCESS;
}

void cfr_continuous_update_snapshot(struct pdev_cfr *pa,
				    const struct cfr_rcc_param *rcc)
{
	if (!pa || !rcc)
		return;

	qdf_mem_copy(&pa->continuous_rcc_snapshot, rcc, sizeof(*rcc));
	WRITE_ONCE(pa->continuous_snapshot_valid, 1);
}

QDF_STATUS cfr_continuous_start(struct wlan_objmgr_pdev *pdev)
{
	struct pdev_cfr *pa;
	bool schedule;
	uint32_t poll_ms;

	if (!pdev)
		return QDF_STATUS_E_INVAL;

	pa = wlan_objmgr_pdev_get_comp_private_obj(pdev, WLAN_UMAC_COMP_CFR);
	if (!pa || !READ_ONCE(pa->continuous_work_initialized))
		return QDF_STATUS_E_FAILURE;

	qdf_mutex_acquire(&pa->continuous_lifecycle_lock);
	qdf_mutex_acquire(&pa->continuous_config_lock);
	pa->continuous_capture_active = 0;
	pa->continuous_generation++;
	qdf_mutex_release(&pa->continuous_config_lock);
	(void)qdf_delayed_work_stop_sync(&pa->continuous_rearm_work);
	qdf_mutex_acquire(&pa->continuous_config_lock);
	pa->continuous_last_ppdu_ns = 0;
	pa->continuous_last_dbr_ns = 0;
	pa->continuous_last_rearm_dbr_ns = 0;
	pa->continuous_rearm_stage = CFR_CONTINUOUS_REARM_NONE;
	pa->continuous_blind_retry_pending = 0;
	pa->continuous_hard_complete_ns = 0;
	pa->continuous_capture_active = 1;
	pa->continuous_generation++;
	schedule = pa->continuous_enabled && pa->continuous_snapshot_valid;
	poll_ms = pa->continuous_poll_ms;
	qdf_mutex_release(&pa->continuous_config_lock);
	if (schedule)
		(void)qdf_delayed_work_start(&pa->continuous_rearm_work,
					     poll_ms);
	qdf_mutex_release(&pa->continuous_lifecycle_lock);

	return QDF_STATUS_SUCCESS;
}

void cfr_continuous_stop(struct wlan_objmgr_pdev *pdev)
{
	struct pdev_cfr *pa;

	if (!pdev)
		return;

	pa = wlan_objmgr_pdev_get_comp_private_obj(pdev, WLAN_UMAC_COMP_CFR);
	if (!pa || !READ_ONCE(pa->continuous_work_initialized))
		return;

	qdf_mutex_acquire(&pa->continuous_lifecycle_lock);
	qdf_mutex_acquire(&pa->continuous_config_lock);
	pa->continuous_capture_active = 0;
	pa->continuous_generation++;
	pa->continuous_blind_retry_pending = 0;
	qdf_mutex_release(&pa->continuous_config_lock);
	(void)qdf_delayed_work_stop_sync(&pa->continuous_rearm_work);
	qdf_mutex_acquire(&pa->continuous_config_lock);
	pa->continuous_rearm_stage = CFR_CONTINUOUS_REARM_NONE;
	pa->continuous_hard_complete_ns = 0;
	qdf_mutex_release(&pa->continuous_config_lock);
	qdf_mutex_release(&pa->continuous_lifecycle_lock);
}
#endif /* WLAN_ENH_CFR_ENABLE */

/**
 * wlan_cfr_is_ini_disabled() - Check if cfr feature is disabled
 * @pdev - the physical device object.
 *
 * Return : true if cfr is disabled, else false.
 */
static bool
wlan_cfr_is_ini_disabled(struct wlan_objmgr_pdev *pdev)
{
	struct wlan_objmgr_psoc *psoc;
	uint8_t cfr_disable_bitmap;

	psoc = wlan_pdev_get_psoc(pdev);
	if (!psoc) {
		cfr_err("psoc is null");
		return true;
	}

	cfr_disable_bitmap = cfg_get(psoc, CFG_CFR_DISABLE);

	if (cfr_disable_bitmap & (1 << wlan_objmgr_pdev_get_pdev_id(pdev))) {
		cfr_info("cfr is disabled for pdev[%d]",
			 wlan_objmgr_pdev_get_pdev_id(pdev));
		return true;
	}

	return false;
}

/**
 * wlan_cfr_get_dbr_num_entries() - Get entry number of DBR ring
 * @pdev - the physical device object.
 *
 * Return : Entry number of DBR ring.
 */
static uint32_t
wlan_cfr_get_dbr_num_entries(struct wlan_objmgr_pdev *pdev)
{
	struct wlan_objmgr_psoc *psoc;
	struct wlan_psoc_host_dbr_ring_caps *dbr_ring_cap;
	uint8_t num_dbr_ring_caps, cap_idx, pdev_id;
	struct target_psoc_info *tgt_psoc_info;
	uint32_t num_entries = MAX_LUT_ENTRIES;

	if (!pdev) {
		cfr_err("Invalid pdev");
		return num_entries;
	}

	psoc = wlan_pdev_get_psoc(pdev);
	if (!psoc) {
		cfr_err("psoc is null");
		return num_entries;
	}

	tgt_psoc_info = wlan_psoc_get_tgt_if_handle(psoc);
	if (!tgt_psoc_info) {
		cfr_err("target_psoc_info is null");
		return num_entries;
	}

	num_dbr_ring_caps = target_psoc_get_num_dbr_ring_caps(tgt_psoc_info);
	dbr_ring_cap = target_psoc_get_dbr_ring_caps(tgt_psoc_info);
	pdev_id = wlan_objmgr_pdev_get_pdev_id(pdev);

	for (cap_idx = 0; cap_idx < num_dbr_ring_caps; cap_idx++) {
		if (dbr_ring_cap[cap_idx].pdev_id == pdev_id &&
		    dbr_ring_cap[cap_idx].mod_id == DBR_MODULE_CFR)
			num_entries = dbr_ring_cap[cap_idx].ring_elems_min;
	}

	num_entries = QDF_MIN(num_entries, MAX_LUT_ENTRIES);
	cfr_debug("pdev id %d, num_entries %d", pdev_id, num_entries);

	return num_entries;
}

#ifdef WLAN_CFR_PM
/**
 * cfr_wakelock_init(): Create/init wake lock for CFR
 *
 * Create/init wake lock for CFR
 *
 * Return None
 */
static void cfr_wakelock_init(struct pdev_cfr *pcfr)
{
	if (!pcfr) {
		cfr_debug("NULL pa");
		return;
	}

	pcfr->is_prevent_suspend = false;
	qdf_wake_lock_create(&pcfr->wake_lock, "wlan_cfr");
	qdf_runtime_lock_init(&pcfr->runtime_lock);
}

/**
 * cfr_wakelock_deinit(): Destroy/deinit wake lock for CFR
 *
 * Destroy/deinit wake lock for CFR
 *
 * Return None
 */
static void cfr_wakelock_deinit(struct pdev_cfr *pcfr)
{
	if (!pcfr) {
		cfr_debug("NULL pa");
		return;
	}

	qdf_runtime_lock_deinit(&pcfr->runtime_lock);
	qdf_wake_lock_destroy(&pcfr->wake_lock);
}
#else
static inline void cfr_wakelock_init(struct pdev_cfr *pcfr)
{
}

static inline void cfr_wakelock_deinit(struct pdev_cfr *pcfr)
{
}
#endif

QDF_STATUS
wlan_cfr_psoc_obj_create_handler(struct wlan_objmgr_psoc *psoc, void *arg)
{
	struct psoc_cfr *cfr_sc = NULL;

	cfr_sc = (struct psoc_cfr *)qdf_mem_malloc(sizeof(struct psoc_cfr));
	if (!cfr_sc) {
		cfr_err("Failed to allocate cfr_ctx object\n");
		return QDF_STATUS_E_NOMEM;
	}

	cfr_sc->psoc_obj = psoc;

	wlan_objmgr_psoc_component_obj_attach(psoc, WLAN_UMAC_COMP_CFR,
					      (void *)cfr_sc,
					      QDF_STATUS_SUCCESS);

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS
wlan_cfr_psoc_obj_destroy_handler(struct wlan_objmgr_psoc *psoc, void *arg)
{
	struct psoc_cfr *cfr_sc = NULL;

	cfr_sc = wlan_objmgr_psoc_get_comp_private_obj(psoc,
						       WLAN_UMAC_COMP_CFR);
	if (cfr_sc) {
		wlan_objmgr_psoc_component_obj_detach(psoc, WLAN_UMAC_COMP_CFR,
						      (void *)cfr_sc);
		qdf_mem_free(cfr_sc);
	}

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS
wlan_cfr_pdev_obj_create_handler(struct wlan_objmgr_pdev *pdev, void *arg)
{
	struct pdev_cfr *pa = NULL;
	uint32_t idx;

	if (!pdev) {
		cfr_err("PDEV is NULL\n");
		return QDF_STATUS_E_FAILURE;
	}
	BUILD_BUG_ON(sizeof(struct cfr_streamfs_record_hdr) != 64);
	BUILD_BUG_ON(sizeof(struct cfr_streamfs_session_start_v1) != 64);
	BUILD_BUG_ON(sizeof(struct cfr_streamfs_session_start_v2) != 80);
	BUILD_BUG_ON(sizeof(struct cfr_streamfs_session_end_v1) != 80);
	BUILD_BUG_ON(sizeof(struct cfr_streamfs_rearm_v1) != 60);
	BUILD_BUG_ON(sizeof(struct cfr_streamfs_dbr_meta_v1) != 112);
	BUILD_BUG_ON(sizeof(struct cfr_streamfs_rx_ppdu_v1) != 40);

	if (wlan_cfr_is_ini_disabled(pdev)) {
		wlan_pdev_nif_feat_ext_cap_clear(pdev, WLAN_PDEV_FEXT_CFR_EN);
		return QDF_STATUS_E_NOSUPPORT;
	}

	wlan_pdev_nif_feat_ext_cap_set(pdev, WLAN_PDEV_FEXT_CFR_EN);

	pa = (struct pdev_cfr *)qdf_mem_malloc(sizeof(struct pdev_cfr));
	if (!pa) {
		cfr_err("Failed to allocate pdev_cfr object\n");
		return QDF_STATUS_E_NOMEM;
	}
	pa->pdev_obj = pdev;
	pa->lut_num = wlan_cfr_get_dbr_num_entries(pdev);
	if (!pa->lut_num) {
		cfr_err("lut num is 0");
		qdf_mem_free(pa);
		return QDF_STATUS_E_INVAL;
	}
	pa->lut = (struct look_up_table **)qdf_mem_malloc(pa->lut_num *
			sizeof(struct look_up_table *));
	if (!pa->lut) {
		cfr_err("Failed to allocate lut, lut num %d", pa->lut_num);
		qdf_mem_free(pa);
		return QDF_STATUS_E_NOMEM;
	}
	qdf_spinlock_create(&pa->streamfs_record_lock);
	qdf_mutex_create(&pa->streamfs_lifecycle_lock);
	qdf_spinlock_create(&pa->netlink_lock);
	qdf_mutex_create(&pa->ppdu_sub_lock);
	for (idx = 0; idx < pa->lut_num; idx++) {
		pa->lut[idx] = (struct look_up_table *)qdf_mem_malloc(
			sizeof(struct look_up_table));
		if (!pa->lut[idx]) {
			cfr_err("failed to allocate LUT entry %u", idx);
			goto free_lut_entries;
		}
	}

	pa->streamfs_record_buf = qdf_mem_malloc(CFR_STREAMFS_MAX_RECORD_SIZE);
	if (!pa->streamfs_record_buf) {
		cfr_err("failed to allocate bounded CFR relay staging buffer");
		goto free_lut_entries;
	}
	pa->netlink_buf = qdf_mem_malloc(CFR_STREAMFS_MAX_RECORD_SIZE);
	if (!pa->netlink_buf) {
		cfr_err("failed to allocate bounded CFR netlink buffer");
		goto free_lut_entries;
	}
	pa->netlink_buf_size = CFR_STREAMFS_MAX_RECORD_SIZE;
	pa->streamfs_session_state = CFR_STREAMFS_SESSION_DISABLED;
#ifdef WLAN_ENH_CFR_ENABLE
	if (QDF_IS_STATUS_ERROR(cfr_continuous_init(pa))) {
		cfr_err("failed to initialize CFR continuous capture work");
		goto free_lut_entries;
	}
#endif

	cfr_wakelock_init(pa);
	wlan_objmgr_pdev_component_obj_attach(pdev, WLAN_UMAC_COMP_CFR,
					      (void *)pa, QDF_STATUS_SUCCESS);

	return QDF_STATUS_SUCCESS;

free_lut_entries:
#ifdef WLAN_ENH_CFR_ENABLE
	cfr_continuous_deinit(pa);
#endif
	qdf_mem_free(pa->netlink_buf);
	qdf_mem_free(pa->streamfs_record_buf);
	while (idx)
		qdf_mem_free(pa->lut[--idx]);
	qdf_mem_free(pa->lut);
	qdf_mutex_destroy(&pa->ppdu_sub_lock);
	qdf_spinlock_destroy(&pa->netlink_lock);
	qdf_spinlock_destroy(&pa->streamfs_record_lock);
	qdf_mutex_destroy(&pa->streamfs_lifecycle_lock);
	qdf_mem_free(pa);
	return QDF_STATUS_E_NOMEM;
}

QDF_STATUS
wlan_cfr_pdev_obj_destroy_handler(struct wlan_objmgr_pdev *pdev, void *arg)
{
	struct pdev_cfr *pa = NULL;
	uint32_t idx;
#ifdef WLAN_ENH_CFR_ENABLE
	bool ppdu_ctx_detached = true;
#endif

	if (!pdev) {
		cfr_err("PDEV is NULL\n");
		return QDF_STATUS_E_FAILURE;
	}

	if (wlan_cfr_is_feature_disabled(pdev)) {
		cfr_info("cfr is disabled");
		return QDF_STATUS_E_NOSUPPORT;
	}

	pa = wlan_objmgr_pdev_get_comp_private_obj(pdev, WLAN_UMAC_COMP_CFR);
	if (pa) {
#ifdef WLAN_ENH_CFR_ENABLE
		cfr_continuous_deinit(pa);
		if (pa->ppdu_subscribed &&
		    QDF_IS_STATUS_ERROR(tgt_cfr_subscribe_ppdu_desc(pdev, false)))
			cfr_err("failed to unsubscribe CFR PPDU callback at teardown");
		ppdu_ctx_detached = !READ_ONCE(pa->ppdu_subscribed);
		if (!ppdu_ctx_detached && pa->ppdu_subscribe_ctx)
			((wdi_event_subscribe *)pa->ppdu_subscribe_ctx)->context = NULL;
		if (pa->ppdu_subscribe_ctx)
			synchronize_net();
#endif
		(void)cfr_streamfs_end_session(pdev, CFR_STREAMFS_STOP_SHUTDOWN,
					   true);
		(void)cfr_streamfs_remove(pdev);
		cfr_wakelock_deinit(pa);
		wlan_objmgr_pdev_component_obj_detach(pdev, WLAN_UMAC_COMP_CFR,
						      (void *)pa);
		if (pa->lut) {
			for (idx = 0; idx < pa->lut_num; idx++)
				qdf_mem_free(pa->lut[idx]);
			qdf_mem_free(pa->lut);
		}
#ifdef WLAN_ENH_CFR_ENABLE
		if (ppdu_ctx_detached)
			qdf_mem_free(pa->ppdu_subscribe_ctx);
		else if (pa->ppdu_subscribe_ctx)
			cfr_err("leaking active CFR PPDU subscription context");
#else
		qdf_mem_free(pa->ppdu_subscribe_ctx);
#endif
		qdf_mem_free(pa->netlink_buf);
		qdf_mem_free(pa->streamfs_record_buf);
		qdf_mutex_destroy(&pa->ppdu_sub_lock);
		qdf_spinlock_destroy(&pa->netlink_lock);
		qdf_spinlock_destroy(&pa->streamfs_record_lock);
		qdf_mutex_destroy(&pa->streamfs_lifecycle_lock);
		qdf_mem_free(pa);
	}

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS
wlan_cfr_peer_obj_create_handler(struct wlan_objmgr_peer *peer, void *arg)
{
	struct peer_cfr *pe = NULL;
	struct wlan_objmgr_vdev *vdev;
	struct wlan_objmgr_pdev *pdev = NULL;

	if (!peer) {
		cfr_err("PEER is NULL\n");
		return QDF_STATUS_E_FAILURE;
	}

	vdev = wlan_peer_get_vdev(peer);
	if (vdev)
		pdev = wlan_vdev_get_pdev(vdev);

	if (!pdev) {
		cfr_err("PDEV is NULL\n");
		return QDF_STATUS_E_FAILURE;
	}

	if (wlan_cfr_is_feature_disabled(pdev)) {
		cfr_debug("cfr is disabled");
		return QDF_STATUS_E_NOSUPPORT;
	}

	pe = (struct peer_cfr *)qdf_mem_malloc(sizeof(struct peer_cfr));
	if (!pe) {
		cfr_err("Failed to allocate peer_cfr object\n");
		return QDF_STATUS_E_FAILURE;
	}

	pe->peer_obj = peer;

	/* Remaining will be populated when we give CFR capture command */
	wlan_objmgr_peer_component_obj_attach(peer, WLAN_UMAC_COMP_CFR,
					      (void *)pe, QDF_STATUS_SUCCESS);
	return QDF_STATUS_SUCCESS;
}

QDF_STATUS
wlan_cfr_peer_obj_destroy_handler(struct wlan_objmgr_peer *peer, void *arg)
{
	struct peer_cfr *pe = NULL;
	struct wlan_objmgr_vdev *vdev;
	struct wlan_objmgr_pdev *pdev = NULL;
	struct pdev_cfr *pa = NULL;

	if (!peer) {
		cfr_err("PEER is NULL\n");
		return QDF_STATUS_E_FAILURE;
	}

	vdev = wlan_peer_get_vdev(peer);
	if (vdev)
		pdev = wlan_vdev_get_pdev(vdev);

	if (wlan_cfr_is_feature_disabled(pdev)) {
		cfr_info("cfr is disabled");
		return QDF_STATUS_E_NOSUPPORT;
	}

	if (pdev)
		pa = wlan_objmgr_pdev_get_comp_private_obj(pdev,
							   WLAN_UMAC_COMP_CFR);

	pe = wlan_objmgr_peer_get_comp_private_obj(peer, WLAN_UMAC_COMP_CFR);

	if (pa && pe) {
		if (pe->period && pe->request)
			pa->cfr_current_sta_count--;
	}

	if (pe) {
		wlan_objmgr_peer_component_obj_detach(peer, WLAN_UMAC_COMP_CFR,
						      (void *)pe);
		qdf_mem_free(pe);
	}

	return QDF_STATUS_SUCCESS;
}

#ifdef CFR_USE_FIXED_FOLDER
static char *cfr_get_dev_name(struct wlan_objmgr_pdev *pdev)
{
	char *default_name = "wlan0";

	return default_name;
}
#else
/**
 * cfr_get_dev_name() - Get net device name from pdev
 *  @pdev: objmgr pdev
 *
 *  Return: netdev name
 */
static char *cfr_get_dev_name(struct wlan_objmgr_pdev *pdev)
{
	struct pdev_osif_priv *pdev_ospriv;
	struct qdf_net_if *nif;

	pdev_ospriv = wlan_pdev_get_ospriv(pdev);
	if (!pdev_ospriv) {
		cfr_err("pdev_ospriv is NULL\n");
		return NULL;
	}

	nif = pdev_ospriv->nif;
	if (!nif) {
		cfr_err("pdev nif is NULL\n");
		return NULL;
	}

	return  qdf_net_if_get_devname(nif);
}
#endif

QDF_STATUS cfr_streamfs_init(struct wlan_objmgr_pdev *pdev)
{
	struct pdev_cfr *pa = NULL;
	qdf_streamfs_chan_t old_chan = NULL, new_chan = NULL;
	qdf_dentry_t old_dir = NULL, new_dir = NULL;
	QDF_STATUS status = QDF_STATUS_E_FAILURE;
	char *devname;
	char folder[32];

	if (!pdev) {
		cfr_err("PDEV is NULL\n");
		return QDF_STATUS_E_FAILURE;
	}

	if (wlan_cfr_is_feature_disabled(pdev)) {
		cfr_info("cfr is disabled");
		return QDF_STATUS_COMP_DISABLED;
	}

	pa = wlan_objmgr_pdev_get_comp_private_obj(pdev, WLAN_UMAC_COMP_CFR);

	if (pa == NULL) {
		cfr_err("pdev_cfr is NULL\n");
		return QDF_STATUS_E_FAILURE;
	}

	if (!pa->is_cfr_capable) {
		cfr_err("CFR IS NOT SUPPORTED\n");
		return QDF_STATUS_E_FAILURE;
	}

	qdf_mutex_acquire(&pa->streamfs_lifecycle_lock);
	qdf_spin_lock_bh(&pa->streamfs_record_lock);
	if (pa->chan_ptr && pa->dir_ptr) {
		pa->streamfs_teardown = 0;
		qdf_spin_unlock_bh(&pa->streamfs_record_lock);
		qdf_mutex_release(&pa->streamfs_lifecycle_lock);
		return QDF_STATUS_SUCCESS;
	}

	old_chan = pa->chan_ptr;
	old_dir = pa->dir_ptr;
	pa->chan_ptr = NULL;
	pa->dir_ptr = NULL;
	pa->streamfs_record_enabled = 0;
	pa->streamfs_teardown = 1;
	qdf_spin_unlock_bh(&pa->streamfs_record_lock);

	if (old_chan) {
		qdf_streamfs_flush(old_chan);
		qdf_streamfs_close(old_chan);
	}
	if (old_dir)
		qdf_streamfs_remove_dir_recursive(old_dir);

	devname = cfr_get_dev_name(pdev);
	if (!devname) {
		cfr_err("devname is NULL\n");
		goto out_not_ready;
	}

	snprintf(folder, sizeof(folder), "cfr%s", devname);

	new_dir = debugfs_create_dir((const char *)folder, NULL);

	if (IS_ERR_OR_NULL(new_dir)) {
		cfr_err("Directory create failed");
		new_dir = NULL;
		goto out_not_ready;
	}

	new_chan = qdf_streamfs_open("cfr_dump", new_dir, pa->subbuf_size,
				      pa->num_subbufs, NULL);

	if (!new_chan) {
		cfr_err("Chan create failed");
		qdf_streamfs_remove_dir_recursive(new_dir);
		new_dir = NULL;
		goto out_not_ready;
	}

	if (IS_ERR_OR_NULL(debugfs_create_file("status", 0440, new_dir, pa,
					     &cfr_debugfs_status_fops)) ||
	    IS_ERR_OR_NULL(debugfs_create_file("correlation", 0440, new_dir, pa,
					     &cfr_debugfs_correlation_fops)) ||
	    IS_ERR_OR_NULL(debugfs_create_file("relay_stats", 0440, new_dir, pa,
					     &cfr_debugfs_relay_stats_fops)) ||
	    IS_ERR_OR_NULL(debugfs_create_file("session", 0440, new_dir, pa,
					     &cfr_debugfs_session_fops)) ||
	    IS_ERR_OR_NULL(debugfs_create_file("continuous", 0440, new_dir, pa,
					     &cfr_debugfs_continuous_fops))) {
		cfr_err("failed to create CFR debugfs diagnostics");
		qdf_streamfs_close(new_chan);
		qdf_streamfs_remove_dir_recursive(new_dir);
		new_chan = NULL;
		new_dir = NULL;
		goto out_not_ready;
	}

	qdf_spin_lock_bh(&pa->streamfs_record_lock);
	pa->chan_ptr = new_chan;
	pa->dir_ptr = new_dir;
	pa->streamfs_teardown = 0;
	qdf_spin_unlock_bh(&pa->streamfs_record_lock);
	status = QDF_STATUS_SUCCESS;
	goto out;

out_not_ready:
	qdf_spin_lock_bh(&pa->streamfs_record_lock);
	pa->streamfs_teardown = 0;
	qdf_spin_unlock_bh(&pa->streamfs_record_lock);
out:
	qdf_mutex_release(&pa->streamfs_lifecycle_lock);
	return status;
}

QDF_STATUS cfr_streamfs_remove(struct wlan_objmgr_pdev *pdev)
{
	struct pdev_cfr *pa = NULL;
	qdf_streamfs_chan_t old_chan;
	qdf_dentry_t old_dir;

	if (!pdev) {
		cfr_err("PDEV is NULL\n");
		return QDF_STATUS_E_INVAL;
	}

	if (wlan_cfr_is_feature_disabled(pdev)) {
		cfr_info("cfr is disabled");
		return QDF_STATUS_COMP_DISABLED;
	}

	pa = wlan_objmgr_pdev_get_comp_private_obj(pdev, WLAN_UMAC_COMP_CFR);
	if (!pa)
		return QDF_STATUS_E_FAILURE;

	qdf_mutex_acquire(&pa->streamfs_lifecycle_lock);
	qdf_spin_lock_bh(&pa->streamfs_record_lock);
	(void)cfr_streamfs_end_session_locked(pa, CFR_STREAMFS_STOP_SHUTDOWN,
					  true);
	pa->streamfs_record_enabled = 0;
	pa->streamfs_user_disabled = 0;
	pa->streamfs_capture_active = 0;
	pa->streamfs_teardown = 1;
	old_chan = pa->chan_ptr;
	old_dir = pa->dir_ptr;
	pa->chan_ptr = NULL;
	pa->dir_ptr = NULL;
	qdf_spin_unlock_bh(&pa->streamfs_record_lock);

	if (old_chan) {
		qdf_streamfs_flush(old_chan);
		qdf_streamfs_close(old_chan);
	}
	if (old_dir)
		qdf_streamfs_remove_dir_recursive(old_dir);
	qdf_mutex_release(&pa->streamfs_lifecycle_lock);

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS cfr_streamfs_reset(struct wlan_objmgr_pdev *pdev)
{
	struct pdev_cfr *pa;
	qdf_streamfs_chan_t chan;
	bool restart_session;
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	if (!pdev)
		return QDF_STATUS_E_INVAL;

	pa = wlan_objmgr_pdev_get_comp_private_obj(pdev, WLAN_UMAC_COMP_CFR);
	if (!pa)
		return QDF_STATUS_E_FAILURE;

	qdf_mutex_acquire(&pa->streamfs_lifecycle_lock);
	qdf_spin_lock_bh(&pa->streamfs_record_lock);
	chan = pa->chan_ptr;
	if (!chan) {
		qdf_spin_unlock_bh(&pa->streamfs_record_lock);
		qdf_mutex_release(&pa->streamfs_lifecycle_lock);
		return QDF_STATUS_E_FAILURE;
	}
	restart_session = pa->streamfs_capture_active &&
			  !pa->streamfs_user_disabled;
	pa->streamfs_record_enabled = 0;
	pa->streamfs_session_state = CFR_STREAMFS_SESSION_STOPPED;
	qdf_spin_unlock_bh(&pa->streamfs_record_lock);

	qdf_streamfs_reset(chan);
	qdf_spin_lock_bh(&pa->streamfs_record_lock);
	pa->streamfs_record_seq = 0;
	pa->streamfs_session_reset_cnt++;
	if (pa->chan_ptr == chan && !pa->streamfs_teardown &&
	    restart_session)
		status = cfr_streamfs_begin_session_locked(pa);
	qdf_spin_unlock_bh(&pa->streamfs_record_lock);
	qdf_mutex_release(&pa->streamfs_lifecycle_lock);

	return status;
}

static void cfr_streamfs_count_drop_locked(struct pdev_cfr *pa, uint32_t type,
					   size_t record_len,
					   QDF_STATUS status,
					   bool invalid_length)
{
	pa->streamfs_record_fail_cnt++;
	pa->streamfs_record_drop_cnt++;
	pa->streamfs_record_drop_bytes += record_len;
	pa->streamfs_sequence_gap_cnt++;
	pa->streamfs_session_records_dropped++;
	pa->streamfs_session_bytes_dropped += record_len;
	if (invalid_length)
		pa->streamfs_record_invalid_len_cnt++;
	else
		pa->streamfs_record_reserve_fail_cnt++;

	switch (type) {
	case CFR_STREAMFS_RECORD_FINAL:
		pa->streamfs_drop_final_cnt++;
		break;
	case CFR_STREAMFS_RECORD_RAW_DBR:
		pa->streamfs_drop_raw_dbr_cnt++;
		break;
	case CFR_STREAMFS_RECORD_RX_PPDU:
		pa->streamfs_drop_rx_ppdu_cnt++;
		break;
	case CFR_STREAMFS_RECORD_DBR_META:
		pa->streamfs_drop_dbr_meta_cnt++;
		break;
	case CFR_STREAMFS_RECORD_SESSION_START:
	case CFR_STREAMFS_RECORD_SESSION_END:
		pa->streamfs_drop_session_cnt++;
		break;
	case CFR_STREAMFS_RECORD_REARM:
		pa->streamfs_drop_rearm_cnt++;
		break;
	default:
		break;
	}

	pa->streamfs_record_last_type = type;
	pa->streamfs_record_last_len = record_len > 0xffffffffU ?
		0xffffffffU : (uint32_t)record_len;
	pa->streamfs_record_last_status = status;
}

static QDF_STATUS
cfr_streamfs_write_record_locked(struct pdev_cfr *pa, uint32_t type,
				 uint32_t meta0, uint32_t meta1,
				 const void *head, size_t hlen,
				 const void *data, size_t dlen,
				 const void *tail, size_t tlen,
				 bool flush)
{
	struct cfr_streamfs_record_hdr hdr;
	uint8_t *record, *cursor;
	size_t payload_len = 0, record_len = 0;
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	/*
	 * Complete-frame CFRR transport
	 *
	 * Build in the fixed per-pdev staging buffer, then use the ordered QDF
	 * relay writer. A full relay drops this complete record and leaves a
	 * visible sequence gap; partial records are never published.
	 */
	if (!pa->streamfs_record_enabled) {
		status = QDF_STATUS_COMP_DISABLED;
		pa->streamfs_record_disabled_cnt++;
		if (pa->streamfs_teardown)
			pa->streamfs_record_disabled_teardown_cnt++;
		else if (pa->streamfs_user_disabled)
			pa->streamfs_record_disabled_user_cnt++;
		else
			pa->streamfs_record_disabled_not_ready_cnt++;
		pa->streamfs_record_last_type = type;
		pa->streamfs_record_last_len = 0;
		pa->streamfs_record_last_status = status;
		return status;
	}

	if (type != CFR_STREAMFS_RECORD_SESSION_START &&
	    type != CFR_STREAMFS_RECORD_SESSION_END &&
	    pa->streamfs_session_state != CFR_STREAMFS_SESSION_ACTIVE) {
		status = QDF_STATUS_COMP_DISABLED;
		pa->streamfs_record_disabled_cnt++;
		pa->streamfs_record_disabled_not_ready_cnt++;
		pa->streamfs_record_last_status = status;
		return status;
	}

	pa->streamfs_record_attempt_cnt++;
	pa->streamfs_session_records_attempted++;
	pa->streamfs_record_seq++;

	if (!pa->chan_ptr) {
		status = QDF_STATUS_E_FAILURE;
		cfr_streamfs_count_drop_locked(pa, type, sizeof(hdr), status,
					       false);
		return status;
	}

	if ((hlen && !head) || (dlen && !data) ||
	    (tlen && !tail) ||
	    check_add_overflow(hlen, dlen, &payload_len) ||
	    check_add_overflow(payload_len, tlen, &payload_len) ||
	    check_add_overflow(sizeof(hdr), payload_len, &record_len) ||
	    payload_len > 0xffffffffU ||
	    record_len > CFR_STREAMFS_MAX_RECORD_SIZE ||
	    record_len > pa->subbuf_size) {
		status = QDF_STATUS_E_INVAL;
		cfr_streamfs_count_drop_locked(pa, type,
					       record_len ? record_len :
					       CFR_STREAMFS_MAX_RECORD_SIZE,
					       status, true);
		return status;
	}

	hdr.magic = cpu_to_le32(CFR_STREAMFS_RECORD_MAGIC);
	hdr.version = cpu_to_le16(CFR_STREAMFS_RECORD_VERSION);
	hdr.hdr_len = cpu_to_le16(sizeof(hdr));
	hdr.type = cpu_to_le32(type);
	hdr.flags = 0;
	hdr.seq = cpu_to_le32(pa->streamfs_record_seq);
	hdr.payload_len = cpu_to_le32((uint32_t)payload_len);
	hdr.meta0 = cpu_to_le32(meta0);
	hdr.meta1 = cpu_to_le32(meta1);
	hdr.timestamp_ns = cpu_to_le64(qdf_ktime_to_ns(qdf_ktime_get()));
	hdr.session_id = cpu_to_le64(pa->streamfs_session_id);
	hdr.pdev_id = cpu_to_le32(
		wlan_objmgr_pdev_get_pdev_id(pa->pdev_obj));
	hdr.reserved0 = 0;
	hdr.reserved1 = 0;

	record = pa->streamfs_record_buf;
	if (!record) {
		status = QDF_STATUS_E_NOMEM;
		cfr_streamfs_count_drop_locked(pa, type, record_len, status,
					       false);
		return status;
	}

	cursor = record;
	qdf_mem_copy(cursor, &hdr, sizeof(hdr));
	cursor += sizeof(hdr);
	if (hlen) {
		qdf_mem_copy(cursor, head, hlen);
		cursor += hlen;
	}
	if (dlen) {
		qdf_mem_copy(cursor, data, dlen);
		cursor += dlen;
	}
	if (tlen)
		qdf_mem_copy(cursor, tail, tlen);
	if (!qdf_streamfs_write_atomic(pa->chan_ptr, record, record_len)) {
		status = QDF_STATUS_E_RESOURCES;
		cfr_streamfs_count_drop_locked(pa, type, record_len, status,
					       false);
		return status;
	}

	if (flush)
		qdf_streamfs_flush(pa->chan_ptr);

	pa->streamfs_record_write_cnt++;
	pa->streamfs_record_bytes += record_len;
	pa->streamfs_session_records_committed++;
	pa->streamfs_session_bytes_committed += record_len;
	pa->streamfs_record_last_type = type;
	pa->streamfs_record_last_len = (uint32_t)record_len;
	pa->streamfs_record_last_status = status;

	switch (type) {
	case CFR_STREAMFS_RECORD_FINAL:
		pa->streamfs_record_final_cnt++;
		break;
	case CFR_STREAMFS_RECORD_RAW_DBR:
		pa->streamfs_record_raw_dbr_cnt++;
		break;
	case CFR_STREAMFS_RECORD_RX_PPDU:
		pa->streamfs_record_rx_ppdu_cnt++;
		break;
	case CFR_STREAMFS_RECORD_DBR_META:
		pa->streamfs_record_dbr_meta_cnt++;
		break;
	case CFR_STREAMFS_RECORD_SESSION_START:
		pa->streamfs_record_session_start_cnt++;
		break;
	case CFR_STREAMFS_RECORD_SESSION_END:
		pa->streamfs_record_session_end_cnt++;
		break;
	case CFR_STREAMFS_RECORD_REARM:
		pa->streamfs_record_rearm_cnt++;
		break;
	default:
		break;
	}

	return status;
}

QDF_STATUS cfr_streamfs_write_record(struct pdev_cfr *pa, uint32_t type,
				     uint32_t meta0, uint32_t meta1,
				     const void *head, size_t hlen,
				     const void *data, size_t dlen,
				     const void *tail, size_t tlen,
				     bool flush)
{
	QDF_STATUS status;

	if (!pa)
		return QDF_STATUS_E_INVAL;
	/* spin_lock_bh serialization is not safe for hard-IRQ writers. */
	if (qdf_unlikely(in_irq()))
		return QDF_STATUS_E_NOSUPPORT;

	qdf_spin_lock_bh(&pa->streamfs_record_lock);
	status = cfr_streamfs_write_record_locked(pa, type, meta0, meta1,
						   head, hlen, data, dlen,
						   tail, tlen, flush);
	qdf_spin_unlock_bh(&pa->streamfs_record_lock);

	return status;
}

static uint32_t cfr_streamfs_capture_mode(struct pdev_cfr *pa)
{
#ifdef WLAN_ENH_CFR_ENABLE
	return (pa->rcc_param.m_directed_ftm ? BIT(0) : 0) |
		(pa->rcc_param.m_all_ftm_ack ? BIT(1) : 0) |
		(pa->rcc_param.m_ndpa_ndp_directed ? BIT(2) : 0) |
		(pa->rcc_param.m_ndpa_ndp_all ? BIT(3) : 0) |
		(pa->rcc_param.m_ta_ra_filter ? BIT(4) : 0) |
		(pa->rcc_param.m_all_packet ? BIT(5) : 0);
#else
	return 0;
#endif
}

static QDF_STATUS cfr_streamfs_begin_session_locked(struct pdev_cfr *pa)
{
	struct cfr_streamfs_session_start_v2 start = {0};
	uint64_t now_ns;
	QDF_STATUS status;

	if (pa->streamfs_session_state == CFR_STREAMFS_SESSION_ACTIVE)
		return QDF_STATUS_SUCCESS;

	if (!pa->chan_ptr || pa->streamfs_teardown || pa->streamfs_user_disabled)
		return QDF_STATUS_E_FAILURE;

	now_ns = qdf_ktime_to_ns(qdf_ktime_get());
	if (now_ns <= pa->streamfs_session_id)
		now_ns = pa->streamfs_session_id + 1;

	pa->streamfs_session_id = now_ns;
	pa->streamfs_session_start_ns = now_ns;
	pa->streamfs_session_end_ns = 0;
	pa->streamfs_session_count++;
	pa->streamfs_record_seq = 0;
	pa->streamfs_session_records_attempted = 0;
	pa->streamfs_session_records_committed = 0;
	pa->streamfs_session_records_dropped = 0;
	pa->streamfs_session_bytes_committed = 0;
	pa->streamfs_session_bytes_dropped = 0;
	pa->streamfs_session_state = CFR_STREAMFS_SESSION_STARTING;
	pa->streamfs_record_enabled = 1;

	start.v1.version = cpu_to_le16(2);
	start.v1.payload_len = cpu_to_le16(sizeof(start));
	start.v1.chip_type = cpu_to_le32(pa->chip_type);
	start.v1.pdev_id = cpu_to_le32(
		wlan_objmgr_pdev_get_pdev_id(pa->pdev_obj));
	start.v1.capture_mode = cpu_to_le32(cfr_streamfs_capture_mode(pa));
#ifdef WLAN_ENH_CFR_ENABLE
	start.v1.filter_group_bitmap = cpu_to_le32(
		pa->rcc_param.filter_group_bitmap);
	start.v1.capture_duration = cpu_to_le32(pa->rcc_param.capture_duration);
	start.v1.capture_interval = cpu_to_le32(pa->rcc_param.capture_interval);
	start.capture_count = cpu_to_le32(pa->rcc_param.capture_count + 1U);
	start.capture_interval_mode = cpu_to_le32(
		pa->rcc_param.capture_intval_mode_sel);
	start.continuous_enabled = cpu_to_le32(pa->continuous_enabled);
	start.watchdog_stall_ms = cpu_to_le32(pa->continuous_stall_ms);
#endif
	start.v1.relay_subbuf_size = cpu_to_le32(pa->subbuf_size);
	start.v1.relay_num_subbufs = cpu_to_le32(pa->num_subbufs);
	start.v1.max_record_size = cpu_to_le32(CFR_STREAMFS_MAX_RECORD_SIZE);
	start.v1.framing_version = cpu_to_le32(CFR_STREAMFS_RECORD_VERSION);
	start.v1.start_timestamp_ns = cpu_to_le64(now_ns);

	status = cfr_streamfs_write_record_locked(pa,
				CFR_STREAMFS_RECORD_SESSION_START,
				0, 0, &start, sizeof(start), NULL, 0,
				NULL, 0, true);
	if (status == QDF_STATUS_SUCCESS) {
		pa->streamfs_session_state = CFR_STREAMFS_SESSION_ACTIVE;
	} else {
		pa->streamfs_session_state = CFR_STREAMFS_SESSION_ERROR;
		pa->streamfs_record_enabled = 0;
	}

	return status;
}

QDF_STATUS cfr_streamfs_begin_session(struct wlan_objmgr_pdev *pdev)
{
	struct pdev_cfr *pa;
	QDF_STATUS status;

	if (!pdev)
		return QDF_STATUS_E_INVAL;

	pa = wlan_objmgr_pdev_get_comp_private_obj(pdev, WLAN_UMAC_COMP_CFR);
	if (!pa)
		return QDF_STATUS_E_FAILURE;

	qdf_mutex_acquire(&pa->streamfs_lifecycle_lock);
	qdf_spin_lock_bh(&pa->streamfs_record_lock);
	status = cfr_streamfs_begin_session_locked(pa);
	qdf_spin_unlock_bh(&pa->streamfs_record_lock);
	qdf_mutex_release(&pa->streamfs_lifecycle_lock);

	return status;
}

QDF_STATUS cfr_streamfs_set_capture_active(struct wlan_objmgr_pdev *pdev,
					   bool active)
{
	struct pdev_cfr *pa;
	bool relay_enabled;

	if (!pdev)
		return QDF_STATUS_E_INVAL;

	pa = wlan_objmgr_pdev_get_comp_private_obj(pdev, WLAN_UMAC_COMP_CFR);
	if (!pa)
		return QDF_STATUS_E_FAILURE;

	qdf_spin_lock_bh(&pa->streamfs_record_lock);
	pa->streamfs_capture_active = active;
	relay_enabled = !pa->streamfs_user_disabled;
	qdf_spin_unlock_bh(&pa->streamfs_record_lock);

	if (!active || !relay_enabled)
		return QDF_STATUS_SUCCESS;

	return cfr_streamfs_begin_session(pdev);
}

static QDF_STATUS
cfr_streamfs_end_session_locked(struct pdev_cfr *pa,
				enum cfr_streamfs_stop_reason reason,
				bool disable_relay)
{
	struct cfr_streamfs_session_end_v1 end = {0};
	uint64_t now_ns;
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	if (pa->streamfs_session_state != CFR_STREAMFS_SESSION_ACTIVE &&
	    pa->streamfs_session_state != CFR_STREAMFS_SESSION_STARTING) {
		if (disable_relay)
			pa->streamfs_record_enabled = 0;
		return QDF_STATUS_SUCCESS;
	}

	pa->streamfs_session_state = CFR_STREAMFS_SESSION_STOPPING;
	now_ns = qdf_ktime_to_ns(qdf_ktime_get());
	end.version = cpu_to_le16(1);
	end.payload_len = cpu_to_le16(sizeof(end));
	end.stop_reason = cpu_to_le32(reason);
	end.last_sequence = cpu_to_le32(pa->streamfs_record_seq);
	end.records_attempted = cpu_to_le64(
		pa->streamfs_session_records_attempted + 1);
	end.records_committed = cpu_to_le64(
		pa->streamfs_session_records_committed + 1);
	end.records_dropped = cpu_to_le64(pa->streamfs_session_records_dropped);
	end.bytes_committed = cpu_to_le64(pa->streamfs_session_bytes_committed +
					   sizeof(struct cfr_streamfs_record_hdr) +
					   sizeof(end));
	end.bytes_dropped = cpu_to_le64(pa->streamfs_session_bytes_dropped);
#ifdef WLAN_ENH_CFR_ENABLE
	end.correlation_successes = cpu_to_le64(pa->ppdu_match_cnt);
	end.correlation_misses = cpu_to_le64(pa->rx_history_miss_cnt);
#endif
	end.end_timestamp_ns = cpu_to_le64(now_ns);

	status = cfr_streamfs_write_record_locked(pa,
				CFR_STREAMFS_RECORD_SESSION_END,
				reason, pa->streamfs_record_seq,
				&end, sizeof(end), NULL, 0, NULL, 0, true);
	pa->streamfs_session_end_ns = now_ns;
	pa->streamfs_last_stop_reason = reason;
	pa->streamfs_session_state = status == QDF_STATUS_SUCCESS ?
		CFR_STREAMFS_SESSION_STOPPED : CFR_STREAMFS_SESSION_ERROR;
	if (disable_relay)
		pa->streamfs_record_enabled = 0;

	return status;
}

QDF_STATUS
cfr_streamfs_end_session(struct wlan_objmgr_pdev *pdev,
			 enum cfr_streamfs_stop_reason reason,
			 bool disable_relay)
{
	struct pdev_cfr *pa;
	QDF_STATUS status;

	if (!pdev)
		return QDF_STATUS_E_INVAL;

	pa = wlan_objmgr_pdev_get_comp_private_obj(pdev, WLAN_UMAC_COMP_CFR);
	if (!pa)
		return QDF_STATUS_E_FAILURE;

	qdf_mutex_acquire(&pa->streamfs_lifecycle_lock);
	qdf_spin_lock_bh(&pa->streamfs_record_lock);
	status = cfr_streamfs_end_session_locked(pa, reason, disable_relay);
	qdf_spin_unlock_bh(&pa->streamfs_record_lock);
	qdf_mutex_release(&pa->streamfs_lifecycle_lock);

	return status;
}

QDF_STATUS
cfr_streamfs_report_reader_stats(struct wlan_objmgr_pdev *pdev,
				 uint64_t sequence_gaps,
				 uint64_t resync_bytes,
				 uint64_t invalid_frames)
{
	struct pdev_cfr *pa;

	if (!pdev)
		return QDF_STATUS_E_INVAL;

	pa = wlan_objmgr_pdev_get_comp_private_obj(pdev, WLAN_UMAC_COMP_CFR);
	if (!pa)
		return QDF_STATUS_E_FAILURE;

	qdf_spin_lock_bh(&pa->streamfs_record_lock);
	pa->streamfs_reader_sequence_gaps += sequence_gaps;
	pa->streamfs_reader_resync_bytes += resync_bytes;
	pa->streamfs_reader_invalid_frames += invalid_frames;
	qdf_spin_unlock_bh(&pa->streamfs_record_lock);

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS cfr_streamfs_clear_counters(struct wlan_objmgr_pdev *pdev)
{
	struct pdev_cfr *pa;

	if (!pdev)
		return QDF_STATUS_E_INVAL;

	pa = wlan_objmgr_pdev_get_comp_private_obj(pdev, WLAN_UMAC_COMP_CFR);
	if (!pa)
		return QDF_STATUS_E_FAILURE;

	qdf_spin_lock_bh(&pa->streamfs_record_lock);
	if (pa->streamfs_session_state == CFR_STREAMFS_SESSION_ACTIVE ||
	    pa->streamfs_session_state == CFR_STREAMFS_SESSION_STARTING ||
	    pa->streamfs_session_state == CFR_STREAMFS_SESSION_STOPPING) {
		qdf_spin_unlock_bh(&pa->streamfs_record_lock);
		return QDF_STATUS_E_BUSY;
	}

	pa->streamfs_record_attempt_cnt = 0;
	pa->streamfs_record_write_cnt = 0;
	pa->streamfs_record_fail_cnt = 0;
	pa->streamfs_record_drop_cnt = 0;
	pa->streamfs_record_drop_bytes = 0;
	pa->streamfs_record_reserve_fail_cnt = 0;
	pa->streamfs_record_invalid_len_cnt = 0;
	pa->streamfs_sequence_gap_cnt = 0;
	pa->streamfs_record_disabled_cnt = 0;
	pa->streamfs_record_disabled_user_cnt = 0;
	pa->streamfs_record_disabled_not_ready_cnt = 0;
	pa->streamfs_record_disabled_teardown_cnt = 0;
	pa->streamfs_record_bytes = 0;
	pa->streamfs_record_final_cnt = 0;
	pa->streamfs_record_raw_dbr_cnt = 0;
	pa->streamfs_record_rx_ppdu_cnt = 0;
	pa->streamfs_record_dbr_meta_cnt = 0;
	pa->streamfs_record_session_start_cnt = 0;
	pa->streamfs_record_session_end_cnt = 0;
	pa->streamfs_record_rearm_cnt = 0;
	pa->streamfs_drop_final_cnt = 0;
	pa->streamfs_drop_raw_dbr_cnt = 0;
	pa->streamfs_drop_rx_ppdu_cnt = 0;
	pa->streamfs_drop_dbr_meta_cnt = 0;
	pa->streamfs_drop_session_cnt = 0;
	pa->streamfs_drop_rearm_cnt = 0;
	pa->streamfs_reader_sequence_gaps = 0;
	pa->streamfs_reader_resync_bytes = 0;
	pa->streamfs_reader_invalid_frames = 0;
	pa->streamfs_record_last_type = 0;
	pa->streamfs_record_last_len = 0;
	pa->streamfs_record_last_status = 0;
	qdf_spin_unlock_bh(&pa->streamfs_record_lock);
	qdf_spin_lock_bh(&pa->netlink_lock);
	pa->netlink_send_cnt = 0;
	pa->netlink_drop_cnt = 0;
	qdf_spin_unlock_bh(&pa->netlink_lock);
#ifdef WLAN_ENH_CFR_ENABLE
	qdf_mutex_acquire(&pa->continuous_config_lock);
	pa->continuous_stall_cnt = 0;
	pa->continuous_rearm_epoch = 0;
	pa->continuous_soft_rearm_cnt = 0;
	pa->continuous_hard_rearm_cnt = 0;
	pa->continuous_blind_rearm_cnt = 0;
	pa->continuous_blind_retry_pending = 0;
	pa->continuous_hard_complete_ns = 0;
	pa->continuous_lut_reset_cnt = 0;
	pa->continuous_lut_reset_fail_cnt = 0;
	pa->continuous_dp_cycle_cnt = 0;
	pa->continuous_dp_cycle_fail_cnt = 0;
	pa->continuous_rearm_fail_cnt = 0;
	pa->continuous_last_status = 0;
	pa->continuous_last_disable_status = 0;
	pa->continuous_last_dp_disable_status = 0;
	pa->continuous_last_lut_reset_status = 0;
	pa->continuous_last_dp_enable_status = 0;
	pa->continuous_last_enable_status = 0;
	qdf_mutex_release(&pa->continuous_config_lock);
#endif

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS cfr_streamfs_flush(struct pdev_cfr *pa)
{
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	if (!pa)
		return QDF_STATUS_E_INVAL;

	qdf_mutex_acquire(&pa->streamfs_lifecycle_lock);
	qdf_spin_lock_bh(&pa->streamfs_record_lock);
	if (pa->chan_ptr) {
	/* Flush the data write to channel buffer */
		qdf_streamfs_flush(pa->chan_ptr);
	} else
		status = QDF_STATUS_E_FAILURE;
	qdf_spin_unlock_bh(&pa->streamfs_record_lock);
	qdf_mutex_release(&pa->streamfs_lifecycle_lock);

	return status;
}

QDF_STATUS cfr_stop_indication(struct wlan_objmgr_vdev *vdev)
{
	struct wlan_objmgr_pdev *pdev;
	QDF_STATUS status;

	pdev = wlan_vdev_get_pdev(vdev);
	if (!pdev)
		return QDF_STATUS_E_INVAL;

#ifdef WLAN_ENH_CFR_ENABLE
	cfr_continuous_stop(pdev);
#endif
	status = cfr_streamfs_end_session(pdev, CFR_STREAMFS_STOP_CAPTURE, true);
	(void)cfr_streamfs_set_capture_active(pdev, false);

	return status;
}

#ifdef WLAN_CFR_PM
QDF_STATUS cfr_prevent_suspend(struct pdev_cfr *pcfr)
{
	if (!pcfr) {
		cfr_debug("NULL pcfr");
		return QDF_STATUS_E_INVAL;
	}

	if (pcfr->is_prevent_suspend) {
		cfr_debug("acquired wake lock");
		return QDF_STATUS_E_AGAIN;
	}
	qdf_wake_lock_acquire(&pcfr->wake_lock,
			      WIFI_POWER_EVENT_WAKELOCK_CFR);
	qdf_runtime_pm_prevent_suspend(&pcfr->runtime_lock);
	pcfr->is_prevent_suspend = true;

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS cfr_allow_suspend(struct pdev_cfr *pcfr)
{
	if (!pcfr) {
		cfr_debug("NULL pcfr");
		return QDF_STATUS_E_INVAL;
	}

	if (!pcfr->is_prevent_suspend) {
		cfr_debug("wake lock not acquired");
		return QDF_STATUS_E_INVAL;
	}
	qdf_wake_lock_release(&pcfr->wake_lock,
			      WIFI_POWER_EVENT_WAKELOCK_CFR);
	qdf_runtime_pm_allow_suspend(&pcfr->runtime_lock);
	pcfr->is_prevent_suspend = false;

	return QDF_STATUS_SUCCESS;
}
#endif
