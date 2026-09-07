/*
 * Copyright (c) 2020-2021, The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * DOC: wlan_hdd_cfr.c
 *
 * WLAN Host Device Driver CFR capture Implementation
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <net/cfg80211.h>
#include "wlan_hdd_includes.h"
#include "osif_sync.h"
#include "wlan_hdd_cfr.h"
#include "wlan_cfr_ucfg_api.h"
#include "wlan_hdd_object_manager.h"
#include "wlan_cmn.h"
#ifdef WLAN_ENH_CFR_ENABLE
#include "cdp_txrx_ctrl.h"
#endif

#define HDD_CFR_MAX_WINDOW_US 0x00ffffffU
#define HDD_CFR_DEFAULT_WINDOW_US 1000000U
#define HDD_CFR_CONTINUOUS_DEFAULT_POLL_MS 50U
#define HDD_CFR_CONTINUOUS_DEFAULT_STALL_MS 250U
#define HDD_CFR_CONTINUOUS_DEFAULT_DRAIN_MS 75U
#define HDD_CFR_CONTINUOUS_MIN_POLL_MS 20U
#define HDD_CFR_CONTINUOUS_MAX_POLL_MS 1000U
#define HDD_CFR_CONTINUOUS_MIN_STALL_MS 100U
#define HDD_CFR_CONTINUOUS_MAX_STALL_MS 10000U
#define HDD_CFR_CONTINUOUS_MIN_DRAIN_MS 10U
#define HDD_CFR_CONTINUOUS_MAX_DRAIN_MS 100U
#define HDD_CFR_MAX_CAPTURE_COUNT 65536U

static int hdd_cfr_last_start_status;
static int hdd_cfr_last_stop_status;
static uint32_t hdd_cfr_capture_duration = HDD_CFR_DEFAULT_WINDOW_US;
static uint32_t hdd_cfr_capture_interval = HDD_CFR_DEFAULT_WINDOW_US;
static uint32_t hdd_cfr_filter_group_bitmap = 1;
static uint32_t hdd_cfr_capture_count = 1;
static uint32_t hdd_cfr_capture_interval_mode;
static bool hdd_cfr_continuous_requested;
static uint32_t hdd_cfr_continuous_poll_ms =
	HDD_CFR_CONTINUOUS_DEFAULT_POLL_MS;
static uint32_t hdd_cfr_continuous_stall_ms =
	HDD_CFR_CONTINUOUS_DEFAULT_STALL_MS;
static uint32_t hdd_cfr_continuous_drain_ms =
	HDD_CFR_CONTINUOUS_DEFAULT_DRAIN_MS;
static DEFINE_MUTEX(hdd_cfr_config_lock);

const struct nla_policy cfr_config_policy[
		QCA_WLAN_VENDOR_ATTR_PEER_CFR_MAX + 1] = {
	[QCA_WLAN_VENDOR_ATTR_CFR_PEER_MAC_ADDR] =
		VENDOR_NLA_POLICY_MAC_ADDR,
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_ENABLE] = {.type = NLA_FLAG},
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_BANDWIDTH] = {.type = NLA_U8},
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_PERIODICITY] = {.type = NLA_U32},
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_METHOD] = {.type = NLA_U8},
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_VERSION] = {.type = NLA_U8},
	[QCA_WLAN_VENDOR_ATTR_PERIODIC_CFR_CAPTURE_ENABLE] = {
						.type = NLA_FLAG},
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_ENABLE_GROUP_BITMAP] = {
						.type = NLA_U32},
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_DURATION] = {.type = NLA_U32},
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_INTERVAL] = {.type = NLA_U32},
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_CAPTURE_TYPE] = {.type = NLA_U32},
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_UL_MU_MASK] = {.type = NLA_U64},
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_FREEZE_TLV_DELAY_COUNT] = {
						.type = NLA_U32},
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_TABLE] = {
						.type = NLA_NESTED},
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_ENTRY] = {
						.type = NLA_NESTED},
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_NUMBER] = {.type = NLA_U32},
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_TA] =
		VENDOR_NLA_POLICY_MAC_ADDR,
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_RA] =
		VENDOR_NLA_POLICY_MAC_ADDR,
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_TA_MASK] =
		VENDOR_NLA_POLICY_MAC_ADDR,
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_RA_MASK] =
		VENDOR_NLA_POLICY_MAC_ADDR,
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_NSS] = {.type = NLA_U32},
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_BW] = {.type = NLA_U32},
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_MGMT_FILTER] = {
						.type = NLA_U32},
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_CTRL_FILTER] = {
						.type = NLA_U32},
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_DATA_FILTER] = {
						.type = NLA_U32},
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_DATA_TRANSPORT_MODE] = {
						.type = NLA_U8},
	[QCA_WLAN_VENDOR_ATTR_PEER_CFR_DATA_RECEIVER_PID] = {
						.type = NLA_U32},
};

static void
wlan_hdd_transport_mode_cfg(struct wlan_objmgr_pdev *pdev,
			    uint8_t vdev_id, uint32_t pid,
			    enum qca_wlan_vendor_cfr_data_transport_modes tx_mode)
{
	struct pdev_cfr *pa;

	if (!pdev) {
		hdd_err("failed to %s transport mode cb for cfr, pdev is NULL for vdev id %d",
			tx_mode ? "register" : "deregister", vdev_id);
		return;
	}

	pa = wlan_objmgr_pdev_get_comp_private_obj(pdev, WLAN_UMAC_COMP_CFR);
	if (!pa) {
		hdd_err("cfr private obj is NULL for vdev id %d", vdev_id);
		return;
	}
	qdf_spin_lock_bh(&pa->netlink_lock);
	pa->nl_cb.vdev_id = vdev_id;
	pa->nl_cb.pid = pid;
	pa->netlink_enabled =
		tx_mode == QCA_WLAN_VENDOR_CFR_DATA_NETLINK_EVENTS;
	if (pa->netlink_enabled)
		pa->nl_cb.cfr_nl_cb = hdd_cfr_data_send_nl_event;
	else
		pa->nl_cb.cfr_nl_cb = NULL;
	qdf_spin_unlock_bh(&pa->netlink_lock);
}

#ifdef WLAN_ENH_CFR_ENABLE

#define DEFAULT_CFR_NSS 0xff
#define DEFAULT_CFR_BW  0xf
static QDF_STATUS
wlan_cfg80211_cfr_set_group_config(struct wlan_objmgr_vdev *vdev,
				   struct nlattr *tb[])
{
	struct cfr_wlanconfig_param params = { 0 };

	if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_NUMBER]) {
		params.grp_id = nla_get_u32(tb[
			QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_NUMBER]);
		hdd_debug("group_id %d", params.grp_id);
	}

	if (params.grp_id >= HDD_INVALID_GROUP_ID) {
		hdd_err("invalid group id");
		return QDF_STATUS_E_INVAL;
	}

	if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_TA]) {
		nla_memcpy(&params.ta[0],
			   tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_TA],
			   QDF_MAC_ADDR_SIZE);
		hdd_debug("ta " QDF_MAC_ADDR_FMT,
			  QDF_MAC_ADDR_REF(&params.ta[0]));
	}

	if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_TA_MASK]) {
		nla_memcpy(&params.ta_mask[0],
			   tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_TA_MASK],
			   QDF_MAC_ADDR_SIZE);
		hdd_debug("ta_mask " QDF_FULL_MAC_FMT,
			  QDF_FULL_MAC_REF(&params.ta_mask[0]));
	}

	if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_RA]) {
		nla_memcpy(&params.ra[0],
			   tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_RA],
			   QDF_MAC_ADDR_SIZE);
		hdd_debug("ra " QDF_MAC_ADDR_FMT,
			  QDF_MAC_ADDR_REF(&params.ra[0]));
	}

	if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_RA_MASK]) {
		nla_memcpy(&params.ra_mask[0],
			   tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_RA_MASK],
			   QDF_MAC_ADDR_SIZE);
		hdd_debug("ra_mask " QDF_FULL_MAC_FMT,
			  QDF_FULL_MAC_REF(&params.ra_mask[0]));
	}

	if (!qdf_is_macaddr_zero((struct qdf_mac_addr *)&params.ta) ||
	    !qdf_is_macaddr_zero((struct qdf_mac_addr *)&params.ra) ||
	    !qdf_is_macaddr_zero((struct qdf_mac_addr *)&params.ta_mask) ||
	    !qdf_is_macaddr_zero((struct qdf_mac_addr *)&params.ra_mask)) {
		hdd_debug("set tara config");
		ucfg_cfr_set_tara_config(vdev, &params);
	}

	params.nss = DEFAULT_CFR_NSS;
	if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_NSS]) {
		params.nss = nla_get_u32(tb[
			QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_NSS]);
		hdd_debug("nss %d", params.nss);
	}

	params.bw = DEFAULT_CFR_BW;
	if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_BW]) {
		params.bw = nla_get_u32(tb[
			QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_BW]);
		hdd_debug("bw %d", params.bw);
	}

	if (params.nss || params.bw) {
		hdd_debug("set bw nss");
		ucfg_cfr_set_bw_nss(vdev, &params);
	}

	if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_MGMT_FILTER]) {
		params.expected_mgmt_subtype = nla_get_u32(tb[
			QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_MGMT_FILTER]);
		hdd_debug("expected_mgmt_subtype %d(%x)",
			  params.expected_mgmt_subtype,
			  params.expected_mgmt_subtype);
	}

	if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_CTRL_FILTER]) {
		params.expected_ctrl_subtype = nla_get_u32(tb[
			QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_CTRL_FILTER]);
		hdd_debug("expected_mgmt_subtype %d(%x)",
			  params.expected_ctrl_subtype,
			  params.expected_ctrl_subtype);
	}

	if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_DATA_FILTER]) {
		params.expected_data_subtype = nla_get_u32(tb[
			QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_DATA_FILTER]);
		hdd_debug("expected_mgmt_subtype %d(%x)",
			  params.expected_data_subtype,
			  params.expected_data_subtype);
	}

	if (!params.expected_mgmt_subtype ||
	    !params.expected_ctrl_subtype ||
		!params.expected_data_subtype) {
		hdd_debug("set frame type");
		ucfg_cfr_set_frame_type_subtype(vdev, &params);
	}

	return QDF_STATUS_SUCCESS;
}

static enum capture_type convert_vendor_cfr_capture_type(
			enum qca_wlan_vendor_cfr_capture_type type)
{
	switch (type) {
	case QCA_WLAN_VENDOR_CFR_DIRECT_FTM:
		return RCC_DIRECTED_FTM_FILTER;
	case QCA_WLAN_VENDOR_CFR_ALL_FTM_ACK:
		return RCC_ALL_FTM_ACK_FILTER;
	case QCA_WLAN_VENDOR_CFR_DIRECT_NDPA_NDP:
		return RCC_DIRECTED_NDPA_NDP_FILTER;
	case QCA_WLAN_VENDOR_CFR_TA_RA:
		return RCC_TA_RA_FILTER;
	case QCA_WLAN_VENDOR_CFR_ALL_PACKET:
		return RCC_ALL_PACKET_FILTER;
	case QCA_WLAN_VENDOR_CFR_NDPA_NDP_ALL:
		return RCC_NDPA_NDP_ALL_FILTER;
	default:
		hdd_err("invalid capture type");
		return RCC_DIS_ALL_MODE;
	}
}

static int
wlan_cfg80211_cfr_set_config(struct wlan_objmgr_vdev *vdev,
			     struct nlattr *tb[])
{
	struct nlattr *group[QCA_WLAN_VENDOR_ATTR_PEER_CFR_MAX + 1];
	struct nlattr *group_list;
	struct cfr_wlanconfig_param params = { 0 };
	enum capture_type type;
	enum qca_wlan_vendor_cfr_capture_type vendor_capture_type;
	int rem = 0;
	int maxtype;
	int attr;
	uint64_t ul_mu_user_mask = 0;

	if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_DURATION]) {
		params.cap_dur = nla_get_u32(tb[
			QCA_WLAN_VENDOR_ATTR_PEER_CFR_DURATION]);
		ucfg_cfr_set_capture_duration(vdev, &params);
		hdd_debug("params.cap_dur %d", params.cap_dur);
	}

	if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_INTERVAL]) {
		params.cap_intvl = nla_get_u32(tb[
			QCA_WLAN_VENDOR_ATTR_PEER_CFR_INTERVAL]);
		ucfg_cfr_set_capture_interval(vdev, &params);
		hdd_debug("params.cap_intvl %d", params.cap_intvl);
	}

	if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_CAPTURE_TYPE]) {
		vendor_capture_type = nla_get_u32(tb[
			QCA_WLAN_VENDOR_ATTR_PEER_CFR_CAPTURE_TYPE]);
		if ((vendor_capture_type < QCA_WLAN_VENDOR_CFR_DIRECT_FTM) ||
		    (vendor_capture_type > QCA_WLAN_VENDOR_CFR_NDPA_NDP_ALL)) {
			hdd_err_rl("invalid capture type %d",
				   vendor_capture_type);
			return -EINVAL;
		}
		type = convert_vendor_cfr_capture_type(vendor_capture_type);
		ucfg_cfr_set_rcc_mode(vdev, type, 1);
		hdd_debug("type %d", type);
	}

	if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_UL_MU_MASK]) {
		ul_mu_user_mask = nla_get_u64(tb[
			QCA_WLAN_VENDOR_ATTR_PEER_CFR_UL_MU_MASK]);
		hdd_debug("ul_mu_user_mask_lower %d",
			  params.ul_mu_user_mask_lower);
	}

	if (ul_mu_user_mask) {
		params.ul_mu_user_mask_lower =
				(uint32_t)(ul_mu_user_mask & 0xffffffff);
		params.ul_mu_user_mask_lower =
				(uint32_t)(ul_mu_user_mask >> 32);
		hdd_debug("set ul mu user maks");
		ucfg_cfr_set_ul_mu_user_mask(vdev, &params);
	}

	if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_FREEZE_TLV_DELAY_COUNT]) {
		params.freeze_tlv_delay_cnt_thr = nla_get_u32(tb[
		QCA_WLAN_VENDOR_ATTR_PEER_CFR_FREEZE_TLV_DELAY_COUNT]);
		if (params.freeze_tlv_delay_cnt_thr) {
			params.freeze_tlv_delay_cnt_en = 1;
			ucfg_cfr_set_freeze_tlv_delay_cnt(vdev, &params);
			hdd_debug("freeze_tlv_delay_cnt_thr %d",
				  params.freeze_tlv_delay_cnt_thr);
		}
	}

	if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_TABLE]) {
		maxtype = QCA_WLAN_VENDOR_ATTR_PEER_CFR_MAX;
		attr = QCA_WLAN_VENDOR_ATTR_PEER_CFR_GROUP_TABLE;
		nla_for_each_nested(group_list, tb[attr], rem) {
			if (wlan_cfg80211_nla_parse(group, maxtype,
						    nla_data(group_list),
						    nla_len(group_list),
						    cfr_config_policy)) {
				hdd_err("nla_parse failed for cfr config group");
				return -EINVAL;
			}
			wlan_cfg80211_cfr_set_group_config(vdev, group);
		}
	}

	if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_DATA_TRANSPORT_MODE]) {
		uint8_t transport_mode = 0xff;
		uint32_t pid = 0;

		if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_DATA_RECEIVER_PID])
			pid = nla_get_u32(tb[
			QCA_WLAN_VENDOR_ATTR_PEER_CFR_DATA_RECEIVER_PID]);
		else
			hdd_debug("No PID received");

		transport_mode = nla_get_u8(tb[
			QCA_WLAN_VENDOR_ATTR_PEER_CFR_DATA_TRANSPORT_MODE]);

		hdd_debug("tx mode attr %d, pid %d", transport_mode, pid);
		if (transport_mode == QCA_WLAN_VENDOR_CFR_DATA_RELAY_FS ||
		    transport_mode == QCA_WLAN_VENDOR_CFR_DATA_NETLINK_EVENTS) {
			wlan_hdd_transport_mode_cfg(vdev->vdev_objmgr.wlan_pdev,
						    vdev->vdev_objmgr.vdev_id,
						    pid, transport_mode);
		} else {
			hdd_debug("invalid transport mode %d for vdev id %d",
				  transport_mode, vdev->vdev_objmgr.vdev_id);
		}
	}

	return 0;
}

static QDF_STATUS hdd_stop_enh_cfr(struct wlan_objmgr_vdev *vdev)
{
	if (!ucfg_cfr_get_rcc_enabled(vdev))
		return QDF_STATUS_SUCCESS;

	hdd_debug("cleanup rcc mode");
	wlan_objmgr_vdev_try_get_ref(vdev, WLAN_CFR_ID);
	ucfg_cfr_set_rcc_mode(vdev, RCC_DIS_ALL_MODE, 0);
	ucfg_cfr_subscribe_ppdu_desc(wlan_vdev_get_pdev(vdev),
				     false);
	ucfg_cfr_committed_rcc_config(vdev);
	ucfg_cfr_stop_indication(vdev);
	ucfg_cfr_suspend(wlan_vdev_get_pdev(vdev));
	hdd_debug("stop indication done");
	wlan_objmgr_vdev_release_ref(vdev, WLAN_CFR_ID);

	return QDF_STATUS_SUCCESS;
}

QDF_STATUS hdd_cfr_disconnect(struct wlan_objmgr_vdev *vdev)
{
	return hdd_stop_enh_cfr(vdev);
}

static int
wlan_cfg80211_peer_enh_cfr_capture(struct hdd_adapter *adapter,
				   struct nlattr **tb)
{
	struct cfr_wlanconfig_param params = { 0 };
	struct wlan_objmgr_vdev *vdev;
	bool is_start_capture = false;
	int ret = 0;

	if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_ENABLE]) {
		is_start_capture = nla_get_flag(tb[
			QCA_WLAN_VENDOR_ATTR_PEER_CFR_ENABLE]);
	}

	if (is_start_capture &&
	    !tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_ENABLE_GROUP_BITMAP]) {
		hdd_err("Invalid group bitmap");
		return -EINVAL;
	}

	vdev = hdd_objmgr_get_vdev_by_user(adapter, WLAN_CFR_ID);
	if (!vdev) {
		hdd_err("can't get vdev");
		return -EINVAL;
	}

	if (is_start_capture) {
		ret = wlan_cfg80211_cfr_set_config(vdev, tb);
		if (ret) {
			hdd_err("set config failed");
			goto out;
		}
		params.en_cfg = nla_get_u32(tb[
			QCA_WLAN_VENDOR_ATTR_PEER_CFR_ENABLE_GROUP_BITMAP]);
		hdd_debug("params.en_cfg %d", params.en_cfg);
		ucfg_cfr_set_en_bitmap(vdev, &params);
		ucfg_cfr_resume(wlan_vdev_get_pdev(vdev));
		ucfg_cfr_subscribe_ppdu_desc(wlan_vdev_get_pdev(vdev),
					     true);
		ucfg_cfr_committed_rcc_config(vdev);
	} else {
		hdd_stop_enh_cfr(vdev);
	}
out:
	hdd_objmgr_put_vdev_by_user(vdev, WLAN_CFR_ID);
	return ret;
}
#else
static int
wlan_cfg80211_peer_enh_cfr_capture(struct hdd_adapter *adapter,
				   struct nlattr **tb)
{
	return 0;
}
#endif

#ifdef WLAN_CFR_ADRASTEA
static QDF_STATUS
wlan_cfg80211_peer_cfr_capture_cfg_adrastea(struct hdd_adapter *adapter,
					    struct nlattr **tb)
{
	struct cfr_capture_params params = { 0 };
	struct wlan_objmgr_vdev *vdev;
	struct wlan_objmgr_pdev *pdev;
	struct wlan_objmgr_peer *peer;
	struct wlan_objmgr_psoc *psoc;
	struct qdf_mac_addr peer_addr;
	bool is_start_capture = false;
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	if (!tb[QCA_WLAN_VENDOR_ATTR_CFR_PEER_MAC_ADDR]) {
		hdd_err("peer mac addr not given");
		return QDF_STATUS_E_INVAL;
	}

	nla_memcpy(peer_addr.bytes, tb[QCA_WLAN_VENDOR_ATTR_CFR_PEER_MAC_ADDR],
		   QDF_MAC_ADDR_SIZE);

	if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_ENABLE]) {
		is_start_capture = nla_get_flag(tb[
			QCA_WLAN_VENDOR_ATTR_PEER_CFR_ENABLE]);
	}

	vdev = adapter->vdev;
	status = hdd_objmgr_get_vdev_by_user(vdev, WLAN_CFR_ID);
	if (QDF_IS_STATUS_ERROR(status)) {
		hdd_err("failed to get vdev");
		return status;
	}

	pdev = wlan_vdev_get_pdev(vdev);
	if (!pdev) {
		hdd_err("failed to get pdev");
		hdd_objmgr_put_vdev_by_user(vdev, WLAN_CFR_ID);
		return QDF_STATUS_E_INVAL;
	}

	psoc = wlan_vdev_get_psoc(vdev);
	if (!psoc) {
		hdd_err("Failed to get psoc");
		hdd_objmgr_put_vdev_by_user(vdev, WLAN_CFR_ID);
		return QDF_STATUS_E_INVAL;
	}

	peer = wlan_objmgr_get_peer_by_mac(psoc, peer_addr.bytes, WLAN_CFR_ID);
	if (!peer) {
		hdd_err("No peer object found");
		hdd_objmgr_put_vdev_by_user(vdev, WLAN_CFR_ID);
		return QDF_STATUS_E_INVAL;
	}

	if (is_start_capture) {
		if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_PERIODICITY]) {
			params.period = nla_get_u32(tb[
				QCA_WLAN_VENDOR_ATTR_PEER_CFR_PERIODICITY]);
			hdd_debug("params.periodicity %d", params.period);
			/* Set the periodic CFR */
			if (params.period)
				ucfg_cfr_set_timer(pdev, params.period);
		}

		if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_METHOD]) {
			params.method = nla_get_u8(tb[
				QCA_WLAN_VENDOR_ATTR_PEER_CFR_METHOD]);
			/* Adrastea supports only QOS NULL METHOD */
			if (params.method !=
					QCA_WLAN_VENDOR_CFR_METHOD_QOS_NULL) {
				hdd_err_rl("invalid capture method %d",
					   params.method);
				status = QDF_STATUS_E_INVAL;
				goto exit;
			}
		}

		if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_BANDWIDTH]) {
			params.bandwidth = nla_get_u8(tb[
				QCA_WLAN_VENDOR_ATTR_PEER_CFR_BANDWIDTH]);
			/* Adrastea supports only 20Mhz bandwidth CFR capture */
			if (params.bandwidth != NL80211_CHAN_WIDTH_20_NOHT) {
				hdd_err_rl("invalid capture bandwidth %d",
					   params.bandwidth);
				status = QDF_STATUS_E_INVAL;
				goto exit;
			}
		}
		ucfg_cfr_start_capture(pdev, peer, &params);
	} else {
		/* Disable the periodic CFR if enabled */
		if (ucfg_cfr_get_timer(pdev))
			ucfg_cfr_set_timer(pdev, 0);

		/* Disable the peer CFR capture */
		ucfg_cfr_stop_capture(pdev, peer);
	}
exit:
	wlan_objmgr_peer_release_ref(peer, WLAN_CFR_ID);
	hdd_objmgr_put_vdev_by_user(vdev, WLAN_CFR_ID);

	return status;
}
#elif defined(WLAN_CFR_DBR)
static enum
phy_ch_width convert_capture_bw(enum nl80211_chan_width capture_bw)
{
	switch (capture_bw) {
	case NL80211_CHAN_WIDTH_20_NOHT:
	case NL80211_CHAN_WIDTH_20:
		return CH_WIDTH_20MHZ;
	case NL80211_CHAN_WIDTH_40:
		return CH_WIDTH_40MHZ;
	case NL80211_CHAN_WIDTH_80:
		return CH_WIDTH_80MHZ;
	case NL80211_CHAN_WIDTH_80P80:
		return CH_WIDTH_80P80MHZ;
	case NL80211_CHAN_WIDTH_160:
		return CH_WIDTH_160MHZ;
	case NL80211_CHAN_WIDTH_5:
		return CH_WIDTH_5MHZ;
	case NL80211_CHAN_WIDTH_10:
		return CH_WIDTH_10MHZ;
	default:
		hdd_err("invalid capture bw");
		return CH_WIDTH_INVALID;
	}
}

static QDF_STATUS
wlan_cfg80211_peer_cfr_capture_cfg_adrastea(struct hdd_adapter *adapter,
					    struct nlattr **tb)
{
	struct cfr_capture_params params = { 0 };
	struct wlan_objmgr_vdev *vdev;
	struct wlan_objmgr_pdev *pdev;
	struct wlan_objmgr_peer *peer;
	struct wlan_objmgr_psoc *psoc;
	struct qdf_mac_addr peer_addr;
	bool is_start_capture = false;
	QDF_STATUS status = QDF_STATUS_SUCCESS;
	int id;

	id = QCA_WLAN_VENDOR_ATTR_CFR_PEER_MAC_ADDR;
	if (!tb[id]) {
		hdd_err("peer mac addr not given");
		return QDF_STATUS_E_INVAL;
	}

	nla_memcpy(peer_addr.bytes, tb[id],
		   QDF_MAC_ADDR_SIZE);

	id = QCA_WLAN_VENDOR_ATTR_PEER_CFR_ENABLE;
	if (tb[id])
		is_start_capture = nla_get_flag(tb[id]);

	vdev = adapter->vdev;
	status = hdd_objmgr_get_vdev_by_user(vdev, WLAN_CFR_ID);
	if (QDF_IS_STATUS_ERROR(status)) {
		hdd_err("failed to get vdev");
		return status;
	}

	pdev = wlan_vdev_get_pdev(vdev);
	if (!pdev) {
		hdd_err("failed to get pdev");
		hdd_objmgr_put_vdev_by_user(vdev, WLAN_CFR_ID);
		return QDF_STATUS_E_INVAL;
	}

	psoc = wlan_vdev_get_psoc(vdev);
	if (!psoc) {
		hdd_err("Failed to get psoc");
		hdd_objmgr_put_vdev_by_user(vdev, WLAN_CFR_ID);
		return QDF_STATUS_E_INVAL;
	}

	peer = wlan_objmgr_get_peer_by_mac(psoc, peer_addr.bytes, WLAN_CFR_ID);
	if (!peer) {
		hdd_err("No peer object found");
		hdd_objmgr_put_vdev_by_user(vdev, WLAN_CFR_ID);
		return QDF_STATUS_E_INVAL;
	}

	if (is_start_capture) {
		id = QCA_WLAN_VENDOR_ATTR_PEER_CFR_PERIODICITY;
		if (tb[id]) {
			params.period = nla_get_u32(tb[id]);
			hdd_debug("params.periodicity %d", params.period);
			/* Set the periodic CFR */
			if (params.period)
				ucfg_cfr_set_timer(pdev, params.period);
		}
		id = QCA_WLAN_VENDOR_ATTR_PEER_CFR_METHOD;
		if (tb[id]) {
			params.method = nla_get_u8(tb[id]);
			/* Adrastea supports only QOS NULL METHOD */
			if (params.method !=
					QCA_WLAN_VENDOR_CFR_METHOD_QOS_NULL) {
				hdd_err_rl("invalid capture method %d",
					   params.method);
				status = QDF_STATUS_E_INVAL;
				goto exit;
			}
		}
		id = QCA_WLAN_VENDOR_ATTR_PEER_CFR_BANDWIDTH;
		if (tb[id]) {
			params.bandwidth = nla_get_u8(tb[id]);
			params.bandwidth = convert_capture_bw(params.bandwidth);
			if (params.bandwidth > NL80211_CHAN_WIDTH_80) {
				hdd_err_rl("invalid capture bandwidth %d",
					   params.bandwidth);
				status = QDF_STATUS_E_INVAL;
				goto exit;
			}
		}
		ucfg_cfr_start_capture(pdev, peer, &params);
	} else {
		/* Disable the periodic CFR if enabled */
		if (ucfg_cfr_get_timer(pdev))
			ucfg_cfr_set_timer(pdev, 0);

		/* Disable the peer CFR capture */
		ucfg_cfr_stop_capture(pdev, peer);
		ucfg_cfr_stop_indication(vdev);
	}
exit:
	wlan_objmgr_peer_release_ref(peer, WLAN_CFR_ID);
	hdd_objmgr_put_vdev_by_user(vdev, WLAN_CFR_ID);

	return status;
}

#else
static QDF_STATUS
wlan_cfg80211_peer_cfr_capture_cfg_adrastea(struct hdd_adapter *adapter,
					    struct nlattr **tb)
{
	return QDF_STATUS_E_NOSUPPORT;
}
#endif

static int
wlan_cfg80211_peer_cfr_capture_cfg(struct wiphy *wiphy,
				   struct hdd_adapter *adapter,
				   const void *data,
				   int data_len)
{
	struct nlattr *tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_MAX + 1];
	uint8_t version = 0;
	QDF_STATUS status;

	if (wlan_cfg80211_nla_parse(
			tb,
			QCA_WLAN_VENDOR_ATTR_PEER_CFR_MAX,
			data,
			data_len,
			cfr_config_policy)) {
		hdd_err("Invalid ATTR");
		return -EINVAL;
	}

	if (tb[QCA_WLAN_VENDOR_ATTR_PEER_CFR_VERSION]) {
		version = nla_get_u8(tb[
			QCA_WLAN_VENDOR_ATTR_PEER_CFR_VERSION]);
		hdd_debug("version %d", version);
		if (version == LEGACY_CFR_VERSION) {
			status = wlan_cfg80211_peer_cfr_capture_cfg_adrastea(
								adapter, tb);
			return qdf_status_to_os_return(status);
		} else if (version != ENHANCED_CFR_VERSION) {
			hdd_err("unsupported version");
			return -EFAULT;
		}
	}

	return wlan_cfg80211_peer_enh_cfr_capture(adapter, tb);
}

static int __wlan_hdd_cfg80211_peer_cfr_capture_cfg(struct wiphy *wiphy,
						    struct wireless_dev *wdev,
						    const void *data,
						    int data_len)
{
	int ret;
	struct hdd_context *hdd_ctx = wiphy_priv(wiphy);
	struct net_device *dev = wdev->netdev;
	struct hdd_adapter *adapter;

	hdd_enter();

	ret = wlan_hdd_validate_context(hdd_ctx);
	if (ret)
		return ret;

	if (QDF_GLOBAL_FTM_MODE == hdd_get_conparam()) {
		hdd_err("Command not allowed in FTM mode");
		return -EPERM;
	}

	adapter = WLAN_HDD_GET_PRIV_PTR(dev);
	if (wlan_hdd_validate_vdev_id(adapter->vdev_id))
		return -EINVAL;

	wlan_cfg80211_peer_cfr_capture_cfg(wiphy, adapter,
					   data, data_len);

	hdd_exit();

	return ret;
}

int wlan_hdd_cfg80211_peer_cfr_capture_cfg(struct wiphy *wiphy,
					   struct wireless_dev *wdev,
					   const void *data,
					   int data_len)
{
	struct osif_psoc_sync *psoc_sync;
	int errno;

	errno = osif_psoc_sync_op_start(wiphy_dev(wiphy), &psoc_sync);
	if (errno)
		return errno;

	errno = __wlan_hdd_cfg80211_peer_cfr_capture_cfg(wiphy, wdev,
							 data, data_len);

	osif_psoc_sync_op_stop(psoc_sync);

	return errno;
}

static struct hdd_adapter *hdd_cfr_get_adapter(struct hdd_context *hdd_ctx)
{
	struct hdd_adapter *adapter;

	adapter = hdd_get_adapter(hdd_ctx, QDF_STA_MODE);
	if (adapter)
		return adapter;

	return hdd_get_adapter(hdd_ctx, QDF_P2P_CLIENT_MODE);
}

#ifdef WLAN_ENH_CFR_ENABLE
static int hdd_cfr_sysfs_stop_locked(struct wlan_objmgr_vdev *vdev)
{
	struct wlan_objmgr_pdev *pdev = wlan_vdev_get_pdev(vdev);
	QDF_STATUS status, first_status = QDF_STATUS_SUCCESS;

	if (!pdev)
		return -EINVAL;
	ucfg_cfr_continuous_stop(pdev);

	if (ucfg_cfr_get_rcc_enabled(vdev)) {
		status = ucfg_cfr_set_rcc_mode(vdev, RCC_DIS_ALL_MODE, 0);
		if (status != QDF_STATUS_SUCCESS && first_status == QDF_STATUS_SUCCESS)
			first_status = status;

		status = ucfg_cfr_subscribe_ppdu_desc(pdev, false);
		if (status != QDF_STATUS_SUCCESS && first_status == QDF_STATUS_SUCCESS)
			first_status = status;

		status = ucfg_cfr_committed_rcc_config(vdev);
		if (status != QDF_STATUS_SUCCESS && first_status == QDF_STATUS_SUCCESS)
			first_status = status;

		status = ucfg_cfr_suspend(pdev);
		if (status != QDF_STATUS_SUCCESS && first_status == QDF_STATUS_SUCCESS)
			first_status = status;
	}

	status = ucfg_cfr_stop_indication(vdev);
	if (status != QDF_STATUS_SUCCESS && first_status == QDF_STATUS_SUCCESS)
		first_status = status;

	status = ucfg_cfr_rcc_reset_lut(vdev);
	if (status != QDF_STATUS_SUCCESS && first_status == QDF_STATUS_SUCCESS)
		first_status = status;

	return first_status == QDF_STATUS_SUCCESS ? 0 : -EINVAL;
}

static int hdd_cfr_sysfs_start_mode(enum capture_type mode)
{
	struct hdd_context *hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);
	struct osif_psoc_sync *psoc_sync;
	struct hdd_adapter *adapter;
	struct wlan_objmgr_vdev *vdev;
	struct wlan_objmgr_pdev *pdev;
	struct pdev_cfr *pcfr;
	struct cfr_wlanconfig_param params = {0};
	QDF_STATUS status;
	uint32_t capture_duration, capture_interval, capture_count;
	uint32_t capture_interval_mode, filter_group_bitmap;
	uint32_t continuous_poll_ms, continuous_stall_ms, continuous_drain_ms;
	bool continuous_requested;
	int ret;

	ret = wlan_hdd_validate_context(hdd_ctx);
	if (ret)
		return ret;

	if (QDF_GLOBAL_FTM_MODE == hdd_get_conparam())
		return -EPERM;

	ret = osif_psoc_sync_op_start(hdd_ctx->parent_dev, &psoc_sync);
	if (ret)
		return ret;

	adapter = hdd_cfr_get_adapter(hdd_ctx);
	if (!adapter) {
		ret = -ENODEV;
		goto out;
	}

	vdev = hdd_objmgr_get_vdev_by_user(adapter, WLAN_CFR_ID);
	if (!vdev) {
		ret = -EINVAL;
		goto out;
	}

	pdev = wlan_vdev_get_pdev(vdev);
	if (!pdev) {
		ret = -EINVAL;
		goto put_vdev;
	}

	pcfr = wlan_objmgr_pdev_get_comp_private_obj(pdev,
						      WLAN_UMAC_COMP_CFR);
	if (!pcfr || !pcfr->is_cfr_capable) {
		ret = -EOPNOTSUPP;
		goto put_vdev;
	}
	WRITE_ONCE(pcfr->nl_cb.vdev_id, wlan_vdev_get_id(vdev));
	mutex_lock(&hdd_cfr_config_lock);
	capture_duration = hdd_cfr_capture_duration;
	capture_interval = hdd_cfr_capture_interval;
	capture_count = hdd_cfr_capture_count;
	capture_interval_mode = hdd_cfr_capture_interval_mode;
	filter_group_bitmap = hdd_cfr_filter_group_bitmap;
	continuous_requested = hdd_cfr_continuous_requested;
	continuous_poll_ms = hdd_cfr_continuous_poll_ms;
	continuous_stall_ms = hdd_cfr_continuous_stall_ms;
	continuous_drain_ms = hdd_cfr_continuous_drain_ms;
	mutex_unlock(&hdd_cfr_config_lock);

	status = ucfg_cfr_streamfs_init(pdev);
	if (status != QDF_STATUS_SUCCESS) {
		hdd_err("failed to initialize CFR relay streamfs: %d", status);
		ret = -EINVAL;
		goto put_vdev;
	}

	if (ucfg_cfr_get_rcc_enabled(vdev)) {
		ret = hdd_cfr_sysfs_stop_locked(vdev);
		if (ret)
			goto put_vdev;
	}

	status = ucfg_cfr_rcc_reset_lut(vdev);
	if (status != QDF_STATUS_SUCCESS) {
		ret = -EINVAL;
		goto put_vdev;
	}

	ucfg_cfr_rcc_clr_dbg_counters(vdev);

	if (capture_duration || capture_interval) {
		memset(&params, 0, sizeof(params));
		params.cap_dur = 0;
		(void)ucfg_cfr_set_capture_duration(vdev, &params);

		if (capture_interval) {
			memset(&params, 0, sizeof(params));
			params.cap_intvl = capture_interval;
			status = ucfg_cfr_set_capture_interval(vdev, &params);
			if (status != QDF_STATUS_SUCCESS) {
				ret = -EINVAL;
				goto fail_stop;
			}
		}

		if (capture_duration) {
			memset(&params, 0, sizeof(params));
			params.cap_dur = capture_duration;
			status = ucfg_cfr_set_capture_duration(vdev, &params);
			if (status != QDF_STATUS_SUCCESS) {
				ret = -EINVAL;
				goto fail_stop;
			}
		}
	}

	if (pcfr->is_cap_interval_mode_sel_support) {
		memset(&params, 0, sizeof(params));
		params.cap_count = capture_count - 1U;
		status = ucfg_cfr_set_capture_count(vdev, &params);
		if (status != QDF_STATUS_SUCCESS) {
			ret = -EINVAL;
			goto fail_stop;
		}

		memset(&params, 0, sizeof(params));
		params.cap_intval_mode_sel = capture_interval_mode;
		status = ucfg_cfr_set_capture_interval_mode_sel(vdev, &params);
		if (status != QDF_STATUS_SUCCESS) {
			ret = -EINVAL;
			goto fail_stop;
		}
	} else if (capture_interval_mode) {
		ret = -EOPNOTSUPP;
		goto fail_stop;
	}

	status = ucfg_cfr_set_rcc_mode(vdev, mode, 1);
	if (status != QDF_STATUS_SUCCESS) {
		ret = -EINVAL;
		goto fail_stop;
	}

	memset(&params, 0, sizeof(params));
	params.en_cfg = filter_group_bitmap ? filter_group_bitmap : 1;
	status = ucfg_cfr_set_en_bitmap(vdev, &params);
	if (status != QDF_STATUS_SUCCESS) {
		ret = -EINVAL;
		goto fail_stop;
	}

	status = ucfg_cfr_resume(pdev);
	if (status != QDF_STATUS_SUCCESS) {
		ret = -EINVAL;
		goto fail_stop;
	}

	status = ucfg_cfr_subscribe_ppdu_desc(pdev, true);
	if (status != QDF_STATUS_SUCCESS) {
		ret = -EINVAL;
		goto fail_stop;
	}

	status = ucfg_cfr_committed_rcc_config(vdev);
	if (status != QDF_STATUS_SUCCESS) {
		ret = -EINVAL;
		goto fail_stop;
	}

	status = ucfg_cfr_continuous_configure(
		pdev, continuous_requested ||
		mode == RCC_ALL_PACKET_FILTER,
		continuous_poll_ms, continuous_stall_ms, continuous_drain_ms);
	if (status != QDF_STATUS_SUCCESS) {
		ret = -EINVAL;
		goto fail_stop;
	}

	status = ucfg_cfr_streamfs_reset(pdev);
	if (status != QDF_STATUS_SUCCESS) {
		ret = -EINVAL;
		goto fail_stop;
	}

	status = ucfg_cfr_streamfs_set_capture_active(pdev, true);
	if (status != QDF_STATUS_SUCCESS) {
		ret = -EINVAL;
		goto fail_stop;
	}

	status = ucfg_cfr_continuous_start(pdev);
	if (status != QDF_STATUS_SUCCESS) {
		ret = -EINVAL;
		goto fail_stop;
	}

	ret = 0;
	goto put_vdev;

fail_stop:
	(void)hdd_cfr_sysfs_stop_locked(vdev);
put_vdev:
	hdd_objmgr_put_vdev_by_user(vdev, WLAN_CFR_ID);
out:
	osif_psoc_sync_op_stop(psoc_sync);

	return ret;
}

static int hdd_cfr_sysfs_stop(void)
{
	struct hdd_context *hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);
	struct osif_psoc_sync *psoc_sync;
	struct hdd_adapter *adapter;
	struct wlan_objmgr_vdev *vdev;
	int ret;

	ret = wlan_hdd_validate_context(hdd_ctx);
	if (ret)
		return ret;

	ret = osif_psoc_sync_op_start(hdd_ctx->parent_dev, &psoc_sync);
	if (ret)
		return ret;

	adapter = hdd_cfr_get_adapter(hdd_ctx);
	if (!adapter) {
		ret = -ENODEV;
		goto out;
	}

	vdev = hdd_objmgr_get_vdev_by_user(adapter, WLAN_CFR_ID);
	if (!vdev) {
		ret = -EINVAL;
		goto out;
	}

	ret = hdd_cfr_sysfs_stop_locked(vdev);
	hdd_objmgr_put_vdev_by_user(vdev, WLAN_CFR_ID);
out:
	osif_psoc_sync_op_stop(psoc_sync);

	return ret;
}

static int hdd_cfr_sysfs_set_relay(bool enable)
{
	struct hdd_context *hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);
	struct osif_psoc_sync *psoc_sync;
	struct hdd_adapter *adapter;
	struct wlan_objmgr_vdev *vdev;
	struct wlan_objmgr_pdev *pdev;
	QDF_STATUS status;
	int ret;

	ret = wlan_hdd_validate_context(hdd_ctx);
	if (ret)
		return ret;

	ret = osif_psoc_sync_op_start(hdd_ctx->parent_dev, &psoc_sync);
	if (ret)
		return ret;

	adapter = hdd_cfr_get_adapter(hdd_ctx);
	if (!adapter) {
		ret = -ENODEV;
		goto out;
	}

	vdev = hdd_objmgr_get_vdev_by_user(adapter, WLAN_CFR_ID);
	if (!vdev) {
		ret = -EINVAL;
		goto out;
	}

	pdev = wlan_vdev_get_pdev(vdev);
	if (!pdev) {
		ret = -EINVAL;
		goto put_vdev;
	}

	status = ucfg_cfr_streamfs_set_enabled(pdev, enable);
	ret = status == QDF_STATUS_SUCCESS ? 0 : -EINVAL;

put_vdev:
	hdd_objmgr_put_vdev_by_user(vdev, WLAN_CFR_ID);
out:
	osif_psoc_sync_op_stop(psoc_sync);

	return ret;
}

static int hdd_cfr_sysfs_reset_relay(void)
{
	struct hdd_context *hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);
	struct osif_psoc_sync *psoc_sync;
	struct hdd_adapter *adapter;
	struct wlan_objmgr_vdev *vdev;
	struct wlan_objmgr_pdev *pdev;
	QDF_STATUS status;
	int ret;

	ret = wlan_hdd_validate_context(hdd_ctx);
	if (ret)
		return ret;

	ret = osif_psoc_sync_op_start(hdd_ctx->parent_dev, &psoc_sync);
	if (ret)
		return ret;

	adapter = hdd_cfr_get_adapter(hdd_ctx);
	if (!adapter) {
		ret = -ENODEV;
		goto out;
	}

	vdev = hdd_objmgr_get_vdev_by_user(adapter, WLAN_CFR_ID);
	if (!vdev) {
		ret = -EINVAL;
		goto out;
	}

	pdev = wlan_vdev_get_pdev(vdev);
	if (!pdev) {
		ret = -EINVAL;
		goto put_vdev;
	}

	status = ucfg_cfr_streamfs_reset(pdev);
	ret = status == QDF_STATUS_SUCCESS ? 0 : -EINVAL;

put_vdev:
	hdd_objmgr_put_vdev_by_user(vdev, WLAN_CFR_ID);
out:
	osif_psoc_sync_op_stop(psoc_sync);

	return ret;
}
#else
static int hdd_cfr_sysfs_start_mode(int mode)
{
	(void)mode;

	return -EOPNOTSUPP;
}

static int hdd_cfr_sysfs_stop(void)
{
	return -EOPNOTSUPP;
}

static int hdd_cfr_sysfs_set_relay(bool enable)
{
	(void)enable;

	return -EOPNOTSUPP;
}

static int hdd_cfr_sysfs_reset_relay(void)
{
	return -EOPNOTSUPP;
}
#endif

typedef int (*hdd_cfr_pdev_action)(struct wlan_objmgr_pdev *pdev,
				   struct wlan_objmgr_vdev *vdev, void *arg);

static int hdd_cfr_run_pdev_action(hdd_cfr_pdev_action action, void *arg)
{
	struct hdd_context *hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);
	struct osif_psoc_sync *psoc_sync;
	struct hdd_adapter *adapter;
	struct wlan_objmgr_vdev *vdev;
	struct wlan_objmgr_pdev *pdev;
	int ret;

	ret = wlan_hdd_validate_context(hdd_ctx);
	if (ret)
		return ret;

	ret = osif_psoc_sync_op_start(hdd_ctx->parent_dev, &psoc_sync);
	if (ret)
		return ret;

	adapter = hdd_cfr_get_adapter(hdd_ctx);
	if (!adapter) {
		ret = -ENODEV;
		goto out;
	}

	vdev = hdd_objmgr_get_vdev_by_user(adapter, WLAN_CFR_ID);
	if (!vdev) {
		ret = -ENODEV;
		goto out;
	}

	pdev = wlan_vdev_get_pdev(vdev);
	ret = pdev ? action(pdev, vdev, arg) : -ENODEV;
	hdd_objmgr_put_vdev_by_user(vdev, WLAN_CFR_ID);
out:
	osif_psoc_sync_op_stop(psoc_sync);
	return ret;
}

static int hdd_cfr_clear_counters_action(struct wlan_objmgr_pdev *pdev,
					 struct wlan_objmgr_vdev *vdev,
					 void *arg)
{
	QDF_STATUS status;

	status = ucfg_cfr_streamfs_clear_counters(pdev);
	if (status != QDF_STATUS_SUCCESS)
		return -EBUSY;

	status = ucfg_cfr_rcc_clr_dbg_counters(vdev);
	return status == QDF_STATUS_SUCCESS ? 0 : -EINVAL;
}

struct hdd_cfr_reader_stats {
	uint64_t sequence_gaps;
	uint64_t resync_bytes;
	uint64_t invalid_frames;
};

static int hdd_cfr_report_reader_action(struct wlan_objmgr_pdev *pdev,
					struct wlan_objmgr_vdev *vdev,
					void *arg)
{
	struct hdd_cfr_reader_stats *stats = arg;
	QDF_STATUS status;

	status = ucfg_cfr_streamfs_report_reader_stats(
		pdev, stats->sequence_gaps, stats->resync_bytes,
		stats->invalid_frames);
	return status == QDF_STATUS_SUCCESS ? 0 : -EINVAL;
}

struct hdd_cfr_transport_snapshot {
	uint32_t capture_enabled;
	uint32_t capture_count_supported;
	uint32_t capture_interval_mode;
	uint32_t capture_count;
	uint32_t continuous_enabled;
	uint32_t continuous_active;
	uint32_t continuous_poll_ms;
	uint32_t continuous_stall_ms;
	uint32_t continuous_drain_ms;
	uint32_t session_state;
	uint32_t last_sequence;
	int32_t last_error;
	uint64_t session_id;
	uint64_t records_written;
	uint64_t bytes_written;
	uint64_t records_dropped;
	uint64_t malformed_records;
	uint64_t correlation_misses;
	uint64_t relay_overruns;
	uint64_t reader_resync_bytes;
	uint64_t rearm_epoch;
	uint64_t soft_rearms;
	uint64_t hard_rearms;
	uint64_t blind_rearms;
	uint64_t lut_resets;
	uint64_t lut_reset_failures;
	uint64_t dp_cycles;
	uint64_t dp_cycle_failures;
	uint64_t rearm_failures;
	uint64_t stall_events;
};

static int
hdd_cfr_read_transport_snapshot(struct hdd_cfr_transport_snapshot *snapshot)
{
	struct hdd_context *hdd_ctx = cds_get_context(QDF_MODULE_ID_HDD);
	struct osif_psoc_sync *psoc_sync;
	struct hdd_adapter *adapter;
	struct wlan_objmgr_vdev *vdev;
	struct wlan_objmgr_pdev *pdev;
	struct pdev_cfr *pcfr;
	int ret;

	if (!snapshot)
		return -EINVAL;

	ret = wlan_hdd_validate_context(hdd_ctx);
	if (ret)
		return ret;

	ret = osif_psoc_sync_op_start(hdd_ctx->parent_dev, &psoc_sync);
	if (ret)
		return ret;

	adapter = hdd_cfr_get_adapter(hdd_ctx);
	if (!adapter) {
		ret = -ENODEV;
		goto out;
	}

	vdev = hdd_objmgr_get_vdev_by_user(adapter, WLAN_CFR_ID);
	if (!vdev) {
		ret = -ENODEV;
		goto out;
	}

	pdev = wlan_vdev_get_pdev(vdev);
	pcfr = pdev ? wlan_objmgr_pdev_get_comp_private_obj(
		pdev, WLAN_UMAC_COMP_CFR) : NULL;
	if (!pcfr) {
		ret = -ENODEV;
		goto put_vdev;
	}

#ifdef WLAN_ENH_CFR_ENABLE
	snapshot->capture_enabled = ucfg_cfr_get_rcc_enabled(vdev);
	snapshot->capture_count_supported =
		READ_ONCE(pcfr->is_cap_interval_mode_sel_support);
	qdf_mutex_acquire(&pcfr->continuous_config_lock);
	snapshot->capture_interval_mode =
		pcfr->continuous_rcc_snapshot.capture_intval_mode_sel;
	snapshot->capture_count =
		pcfr->continuous_rcc_snapshot.capture_count + 1U;
	snapshot->continuous_enabled = pcfr->continuous_enabled;
	snapshot->continuous_active = pcfr->continuous_capture_active;
	snapshot->continuous_poll_ms = pcfr->continuous_poll_ms;
	snapshot->continuous_stall_ms = pcfr->continuous_stall_ms;
	snapshot->continuous_drain_ms = pcfr->continuous_drain_ms;
	snapshot->rearm_epoch = pcfr->continuous_rearm_epoch;
	snapshot->soft_rearms = pcfr->continuous_soft_rearm_cnt;
	snapshot->hard_rearms = pcfr->continuous_hard_rearm_cnt;
	snapshot->blind_rearms = pcfr->continuous_blind_rearm_cnt;
	snapshot->lut_resets = pcfr->continuous_lut_reset_cnt;
	snapshot->lut_reset_failures = pcfr->continuous_lut_reset_fail_cnt;
	snapshot->dp_cycles = pcfr->continuous_dp_cycle_cnt;
	snapshot->dp_cycle_failures = pcfr->continuous_dp_cycle_fail_cnt;
	snapshot->rearm_failures = pcfr->continuous_rearm_fail_cnt;
	snapshot->stall_events = pcfr->continuous_stall_cnt;
	qdf_mutex_release(&pcfr->continuous_config_lock);
#endif
	qdf_spin_lock_bh(&pcfr->streamfs_record_lock);
	snapshot->session_state = pcfr->streamfs_session_state;
	snapshot->last_sequence = pcfr->streamfs_record_seq;
	snapshot->last_error = pcfr->streamfs_record_last_status;
	snapshot->session_id = pcfr->streamfs_session_id;
	snapshot->records_written = pcfr->streamfs_record_write_cnt;
	snapshot->bytes_written = pcfr->streamfs_record_bytes;
	snapshot->records_dropped = pcfr->streamfs_record_drop_cnt;
	snapshot->relay_overruns = pcfr->streamfs_record_reserve_fail_cnt;
	snapshot->reader_resync_bytes = pcfr->streamfs_reader_resync_bytes;
	qdf_spin_unlock_bh(&pcfr->streamfs_record_lock);
#ifdef WLAN_ENH_CFR_ENABLE
	snapshot->malformed_records = READ_ONCE(pcfr->dbr_cb_invalid_payload_cnt) +
		READ_ONCE(pcfr->dbr_cb_invalid_length_cnt) +
		READ_ONCE(pcfr->dbr_cb_short_freeze_cnt) +
		READ_ONCE(pcfr->dbr_cb_short_mu_cnt) +
		READ_ONCE(pcfr->invalid_dma_length_cnt);
	snapshot->correlation_misses = READ_ONCE(pcfr->rx_history_miss_cnt) +
		READ_ONCE(pcfr->ppdu_mismatch_cnt);
#endif
	ret = 0;

put_vdev:
	hdd_objmgr_put_vdev_by_user(vdev, WLAN_CFR_ID);
out:
	osif_psoc_sync_op_stop(psoc_sync);
	return ret;
}

static ssize_t hdd_cfr_status_show(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  char *buf)
{
	struct hdd_cfr_transport_snapshot snapshot = {0};
	int ret = hdd_cfr_read_transport_snapshot(&snapshot);

	if (ret)
		return ret;

	return sysfs_emit(buf,
		"capture_enabled=%u\ncapture_count_supported=%u\n"
		"capture_interval_mode=%u\ncapture_count=%u\n"
		"continuous_enabled=%u\ncontinuous_active=%u\n"
		"continuous_poll_ms=%u\ncontinuous_stall_ms=%u\n"
		"continuous_drain_ms=%u\nrearm_epoch=%llu\n"
		"soft_rearms=%llu\nhard_rearms=%llu\nblind_rearms=%llu\n"
		"lut_resets=%llu\nlut_reset_failures=%llu\n"
		"dp_cycles=%llu\ndp_cycle_failures=%llu\n"
		"rearm_failures=%llu\nstall_events=%llu\n"
		"session_state=%u\nsession_id=%llu\n"
		"records_written=%llu\nbytes_written=%llu\nrecords_dropped=%llu\n"
		"malformed_records=%llu\ncorrelation_misses=%llu\n"
		"relay_overruns=%llu\nlast_error=%d\nlast_sequence=%u\n",
		snapshot.capture_enabled, snapshot.capture_count_supported,
		snapshot.capture_interval_mode, snapshot.capture_count,
		snapshot.continuous_enabled, snapshot.continuous_active,
		snapshot.continuous_poll_ms, snapshot.continuous_stall_ms,
		snapshot.continuous_drain_ms, snapshot.rearm_epoch,
		snapshot.soft_rearms, snapshot.hard_rearms, snapshot.blind_rearms,
		snapshot.lut_resets, snapshot.lut_reset_failures,
		snapshot.dp_cycles, snapshot.dp_cycle_failures,
		snapshot.rearm_failures, snapshot.stall_events,
		snapshot.session_state,
		snapshot.session_id, snapshot.records_written,
		snapshot.bytes_written, snapshot.records_dropped,
		snapshot.malformed_records, snapshot.correlation_misses,
		snapshot.relay_overruns, snapshot.last_error,
		snapshot.last_sequence);
}

#define HDD_CFR_SNAPSHOT_SHOW_U64(_name, _field) \
	static ssize_t hdd_cfr_##_name##_show(struct kobject *kobj, \
					       struct kobj_attribute *attr, char *buf) \
	{ \
		struct hdd_cfr_transport_snapshot snapshot = {0}; \
		int ret = hdd_cfr_read_transport_snapshot(&snapshot); \
		return ret ? ret : sysfs_emit(buf, "%llu\n", snapshot._field); \
	}

#define HDD_CFR_SNAPSHOT_SHOW_U32(_name, _field) \
	static ssize_t hdd_cfr_##_name##_show(struct kobject *kobj, \
					       struct kobj_attribute *attr, char *buf) \
	{ \
		struct hdd_cfr_transport_snapshot snapshot = {0}; \
		int ret = hdd_cfr_read_transport_snapshot(&snapshot); \
		return ret ? ret : sysfs_emit(buf, "%u\n", snapshot._field); \
	}

HDD_CFR_SNAPSHOT_SHOW_U32(capture_enabled, capture_enabled);
HDD_CFR_SNAPSHOT_SHOW_U32(capture_count_supported, capture_count_supported);
HDD_CFR_SNAPSHOT_SHOW_U32(capture_interval_mode, capture_interval_mode);
HDD_CFR_SNAPSHOT_SHOW_U32(capture_count, capture_count);
HDD_CFR_SNAPSHOT_SHOW_U32(continuous_enabled, continuous_enabled);
HDD_CFR_SNAPSHOT_SHOW_U32(session_state, session_state);
HDD_CFR_SNAPSHOT_SHOW_U64(session_id, session_id);
HDD_CFR_SNAPSHOT_SHOW_U64(records_written, records_written);
HDD_CFR_SNAPSHOT_SHOW_U64(bytes_written, bytes_written);
HDD_CFR_SNAPSHOT_SHOW_U64(records_dropped, records_dropped);
HDD_CFR_SNAPSHOT_SHOW_U64(malformed_records, malformed_records);
HDD_CFR_SNAPSHOT_SHOW_U64(correlation_misses, correlation_misses);
HDD_CFR_SNAPSHOT_SHOW_U64(relay_overruns, relay_overruns);
HDD_CFR_SNAPSHOT_SHOW_U64(reader_resync_bytes, reader_resync_bytes);
HDD_CFR_SNAPSHOT_SHOW_U64(rearm_epoch, rearm_epoch);
HDD_CFR_SNAPSHOT_SHOW_U64(rearm_failures, rearm_failures);
HDD_CFR_SNAPSHOT_SHOW_U32(last_sequence, last_sequence);

static ssize_t hdd_cfr_last_error_show(struct kobject *kobj,
				       struct kobj_attribute *attr, char *buf)
{
	struct hdd_cfr_transport_snapshot snapshot = {0};
	int ret = hdd_cfr_read_transport_snapshot(&snapshot);

	return ret ? ret : sysfs_emit(buf, "%d\n", snapshot.last_error);
}

static int hdd_cfr_set_capture_window(uint32_t duration, uint32_t interval)
{
	if (duration > HDD_CFR_MAX_WINDOW_US ||
	    interval > HDD_CFR_MAX_WINDOW_US)
		return -EINVAL;

	if (duration && interval && duration > interval)
		return -EINVAL;

	mutex_lock(&hdd_cfr_config_lock);
	hdd_cfr_capture_duration = duration;
	hdd_cfr_capture_interval = interval;
	mutex_unlock(&hdd_cfr_config_lock);

	return 0;
}

static int hdd_cfr_set_capture_duration(uint32_t duration)
{
	if (duration > HDD_CFR_MAX_WINDOW_US)
		return -EINVAL;

	mutex_lock(&hdd_cfr_config_lock);
	hdd_cfr_capture_duration = duration;
	if (hdd_cfr_capture_interval && duration > hdd_cfr_capture_interval)
		hdd_cfr_capture_interval = duration;
	mutex_unlock(&hdd_cfr_config_lock);

	return 0;
}

static int hdd_cfr_set_capture_interval(uint32_t interval)
{
	if (interval > HDD_CFR_MAX_WINDOW_US)
		return -EINVAL;

	mutex_lock(&hdd_cfr_config_lock);
	hdd_cfr_capture_interval = interval;
	if (interval && hdd_cfr_capture_duration > interval)
		hdd_cfr_capture_duration = interval;
	mutex_unlock(&hdd_cfr_config_lock);

	return 0;
}

static int hdd_cfr_set_capture_count(uint32_t captures)
{
	if (!captures || captures > HDD_CFR_MAX_CAPTURE_COUNT)
		return -EINVAL;

	mutex_lock(&hdd_cfr_config_lock);
	hdd_cfr_capture_count = captures;
	mutex_unlock(&hdd_cfr_config_lock);
	return 0;
}

static int hdd_cfr_set_continuous_watchdog(uint32_t stall_ms,
					   uint32_t poll_ms,
					   uint32_t drain_ms)
{
	if (poll_ms < HDD_CFR_CONTINUOUS_MIN_POLL_MS ||
	    poll_ms > HDD_CFR_CONTINUOUS_MAX_POLL_MS ||
	    stall_ms < HDD_CFR_CONTINUOUS_MIN_STALL_MS ||
	    stall_ms > HDD_CFR_CONTINUOUS_MAX_STALL_MS ||
	    stall_ms < 2U * poll_ms ||
	    drain_ms < HDD_CFR_CONTINUOUS_MIN_DRAIN_MS ||
	    drain_ms > HDD_CFR_CONTINUOUS_MAX_DRAIN_MS)
		return -EINVAL;

	mutex_lock(&hdd_cfr_config_lock);
	hdd_cfr_continuous_stall_ms = stall_ms;
	hdd_cfr_continuous_poll_ms = poll_ms;
	hdd_cfr_continuous_drain_ms = drain_ms;
	mutex_unlock(&hdd_cfr_config_lock);
	return 0;
}

static void hdd_cfr_set_default_profile(void)
{
	mutex_lock(&hdd_cfr_config_lock);
	hdd_cfr_filter_group_bitmap = 1;
	hdd_cfr_capture_duration = HDD_CFR_DEFAULT_WINDOW_US;
	hdd_cfr_capture_interval = HDD_CFR_DEFAULT_WINDOW_US;
	mutex_unlock(&hdd_cfr_config_lock);
}

static void hdd_cfr_set_aggressive_profile(void)
{
	mutex_lock(&hdd_cfr_config_lock);
	hdd_cfr_filter_group_bitmap = 0xffff;
	hdd_cfr_capture_duration = HDD_CFR_MAX_WINDOW_US;
	hdd_cfr_capture_interval = HDD_CFR_MAX_WINDOW_US;
	mutex_unlock(&hdd_cfr_config_lock);
}

static void hdd_cfr_set_continuous_profile(void)
{
	mutex_lock(&hdd_cfr_config_lock);
	hdd_cfr_filter_group_bitmap = 0xffff;
	hdd_cfr_capture_duration = 100000U;
	hdd_cfr_capture_interval = 100000U;
	hdd_cfr_capture_count = 256;
	hdd_cfr_capture_interval_mode = 1;
	hdd_cfr_continuous_requested = true;
	mutex_unlock(&hdd_cfr_config_lock);
}

static ssize_t hdd_cfr_control_show(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   char *buf)
{
	return sysfs_emit(buf,
			 "commands: start|direct_ftm|all_ftm_ack|direct_ndpa|all_ndpa|all_packet|aggressive|continuous_capture|stop|clear|relay_on|relay_off|relay_reset|reader_stats <gaps> <resync_bytes> <invalid>\n"
			 "config: window <duration_us> <interval_us>|duration <us>|interval <us>|count <captures>|interval_mode duration|count|continuous on|off|watchdog <stall_ms> <poll_ms> <drain_ms>|bitmap <hex>|profile_default|profile_aggressive|profile_continuous\n"
			 "relayfs is the CFRR data plane; sysfs is control and bounded health only\n"
			 "default/safest start mode: direct_ftm\n"
			 "detailed diagnostics: /sys/kernel/debug/cfrwlan0/{status,correlation,relay_stats,session}\n");
}

static ssize_t hdd_cfr_control_store(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    const char *buf, size_t count)
{
	struct hdd_cfr_reader_stats reader_stats;
	unsigned long long gaps, resync_bytes, invalid_frames;
	uint32_t value, value2, value3;
	int ret = -EINVAL;

	if (sscanf(buf, "reader_stats %llu %llu %llu", &gaps,
		   &resync_bytes, &invalid_frames) == 3) {
		reader_stats.sequence_gaps = gaps;
		reader_stats.resync_bytes = resync_bytes;
		reader_stats.invalid_frames = invalid_frames;
		ret = hdd_cfr_run_pdev_action(hdd_cfr_report_reader_action,
					      &reader_stats);
	} else if (sscanf(buf, "window %u %u", &value, &value2) == 2) {
		ret = hdd_cfr_set_capture_window(value, value2);
	} else if (sscanf(buf, "duration %u", &value) == 1) {
		ret = hdd_cfr_set_capture_duration(value);
	} else if (sscanf(buf, "interval %u", &value) == 1) {
		ret = hdd_cfr_set_capture_interval(value);
	} else if (sscanf(buf, "count %u", &value) == 1) {
		ret = hdd_cfr_set_capture_count(value);
	} else if (sysfs_streq(buf, "interval_mode duration")) {
		mutex_lock(&hdd_cfr_config_lock);
		hdd_cfr_capture_interval_mode = 0;
		mutex_unlock(&hdd_cfr_config_lock);
		ret = 0;
	} else if (sysfs_streq(buf, "interval_mode count")) {
		mutex_lock(&hdd_cfr_config_lock);
		hdd_cfr_capture_interval_mode = 1;
		mutex_unlock(&hdd_cfr_config_lock);
		ret = 0;
	} else if (sysfs_streq(buf, "continuous on")) {
		mutex_lock(&hdd_cfr_config_lock);
		hdd_cfr_continuous_requested = true;
		mutex_unlock(&hdd_cfr_config_lock);
		ret = 0;
	} else if (sysfs_streq(buf, "continuous off")) {
		mutex_lock(&hdd_cfr_config_lock);
		hdd_cfr_continuous_requested = false;
		mutex_unlock(&hdd_cfr_config_lock);
		ret = 0;
	} else if (sscanf(buf, "watchdog %u %u %u", &value, &value2,
			  &value3) == 3) {
		ret = hdd_cfr_set_continuous_watchdog(value, value2, value3);
	} else if (sscanf(buf, "bitmap %x", &value) == 1) {
		mutex_lock(&hdd_cfr_config_lock);
		hdd_cfr_filter_group_bitmap = value & 0xffff;
		mutex_unlock(&hdd_cfr_config_lock);
		ret = 0;
	} else if (sysfs_streq(buf, "profile_default")) {
		hdd_cfr_set_default_profile();
		ret = 0;
	} else if (sysfs_streq(buf, "profile_aggressive")) {
		hdd_cfr_set_aggressive_profile();
		ret = 0;
	} else if (sysfs_streq(buf, "profile_continuous")) {
		hdd_cfr_set_continuous_profile();
		ret = 0;
	} else if (sysfs_streq(buf, "stop") || sysfs_streq(buf, "0")) {
		ret = hdd_cfr_sysfs_stop();
		hdd_cfr_last_stop_status = ret;
	} else if (sysfs_streq(buf, "clear")) {
		ret = hdd_cfr_run_pdev_action(hdd_cfr_clear_counters_action,
					      NULL);
	} else if (sysfs_streq(buf, "relay_on")) {
		ret = hdd_cfr_sysfs_set_relay(true);
	} else if (sysfs_streq(buf, "relay_off")) {
		ret = hdd_cfr_sysfs_set_relay(false);
	} else if (sysfs_streq(buf, "relay_reset")) {
		ret = hdd_cfr_sysfs_reset_relay();
	} else if (sysfs_streq(buf, "start") || sysfs_streq(buf, "direct_ftm") ||
		   sysfs_streq(buf, "1")) {
#ifdef WLAN_ENH_CFR_ENABLE
		ret = hdd_cfr_sysfs_start_mode(RCC_DIRECTED_FTM_FILTER);
#else
		ret = hdd_cfr_sysfs_start_mode(0);
#endif
		hdd_cfr_last_start_status = ret;
	} else if (sysfs_streq(buf, "all_ftm_ack")) {
#ifdef WLAN_ENH_CFR_ENABLE
		ret = hdd_cfr_sysfs_start_mode(RCC_ALL_FTM_ACK_FILTER);
#else
		ret = hdd_cfr_sysfs_start_mode(0);
#endif
		hdd_cfr_last_start_status = ret;
	} else if (sysfs_streq(buf, "direct_ndpa")) {
#ifdef WLAN_ENH_CFR_ENABLE
		ret = hdd_cfr_sysfs_start_mode(RCC_DIRECTED_NDPA_NDP_FILTER);
#else
		ret = hdd_cfr_sysfs_start_mode(0);
#endif
		hdd_cfr_last_start_status = ret;
	} else if (sysfs_streq(buf, "all_ndpa")) {
#ifdef WLAN_ENH_CFR_ENABLE
		ret = hdd_cfr_sysfs_start_mode(RCC_NDPA_NDP_ALL_FILTER);
#else
		ret = hdd_cfr_sysfs_start_mode(0);
#endif
		hdd_cfr_last_start_status = ret;
	} else if (sysfs_streq(buf, "all_packet")) {
#ifdef WLAN_ENH_CFR_ENABLE
		ret = hdd_cfr_sysfs_start_mode(RCC_ALL_PACKET_FILTER);
#else
		ret = hdd_cfr_sysfs_start_mode(0);
#endif
		hdd_cfr_last_start_status = ret;
	} else if (sysfs_streq(buf, "continuous_capture")) {
		mutex_lock(&hdd_cfr_config_lock);
		hdd_cfr_continuous_requested = true;
		mutex_unlock(&hdd_cfr_config_lock);
#ifdef WLAN_ENH_CFR_ENABLE
		ret = hdd_cfr_sysfs_start_mode(RCC_ALL_PACKET_FILTER);
#else
		ret = hdd_cfr_sysfs_start_mode(0);
#endif
		hdd_cfr_last_start_status = ret;
	} else if (sysfs_streq(buf, "aggressive")) {
		hdd_cfr_set_aggressive_profile();
#ifdef WLAN_ENH_CFR_ENABLE
		ret = hdd_cfr_sysfs_start_mode(RCC_ALL_PACKET_FILTER);
#else
		ret = hdd_cfr_sysfs_start_mode(0);
#endif
		hdd_cfr_last_start_status = ret;
	}

	return ret ? ret : count;
}

static struct kobj_attribute hdd_cfr_status_attribute =
	__ATTR(cfr_status, 0440, hdd_cfr_status_show, NULL);
static struct kobj_attribute hdd_cfr_control_attribute =
	__ATTR(cfr_control, 0600, hdd_cfr_control_show, hdd_cfr_control_store);
static struct kobj_attribute hdd_cfr_capture_enabled_attribute =
	__ATTR(cfr_capture_enabled, 0440, hdd_cfr_capture_enabled_show, NULL);
static struct kobj_attribute hdd_cfr_capture_count_supported_attribute =
	__ATTR(cfr_capture_count_supported, 0440,
	       hdd_cfr_capture_count_supported_show, NULL);
static struct kobj_attribute hdd_cfr_capture_interval_mode_attribute =
	__ATTR(cfr_capture_interval_mode, 0440,
	       hdd_cfr_capture_interval_mode_show, NULL);
static struct kobj_attribute hdd_cfr_capture_count_attribute =
	__ATTR(cfr_capture_count, 0440, hdd_cfr_capture_count_show, NULL);
static struct kobj_attribute hdd_cfr_continuous_enabled_attribute =
	__ATTR(cfr_continuous_enabled, 0440,
	       hdd_cfr_continuous_enabled_show, NULL);
static struct kobj_attribute hdd_cfr_session_state_attribute =
	__ATTR(cfr_session_state, 0440, hdd_cfr_session_state_show, NULL);
static struct kobj_attribute hdd_cfr_session_id_attribute =
	__ATTR(cfr_session_id, 0440, hdd_cfr_session_id_show, NULL);
static struct kobj_attribute hdd_cfr_records_written_attribute =
	__ATTR(cfr_records_written, 0440, hdd_cfr_records_written_show, NULL);
static struct kobj_attribute hdd_cfr_bytes_written_attribute =
	__ATTR(cfr_bytes_written, 0440, hdd_cfr_bytes_written_show, NULL);
static struct kobj_attribute hdd_cfr_records_dropped_attribute =
	__ATTR(cfr_records_dropped, 0440, hdd_cfr_records_dropped_show, NULL);
static struct kobj_attribute hdd_cfr_malformed_records_attribute =
	__ATTR(cfr_malformed_records, 0440, hdd_cfr_malformed_records_show, NULL);
static struct kobj_attribute hdd_cfr_correlation_misses_attribute =
	__ATTR(cfr_correlation_misses, 0440, hdd_cfr_correlation_misses_show, NULL);
static struct kobj_attribute hdd_cfr_relay_overruns_attribute =
	__ATTR(cfr_relay_overruns, 0440, hdd_cfr_relay_overruns_show, NULL);
static struct kobj_attribute hdd_cfr_reader_resync_bytes_attribute =
	__ATTR(cfr_reader_resync_bytes, 0440,
	       hdd_cfr_reader_resync_bytes_show, NULL);
static struct kobj_attribute hdd_cfr_last_error_attribute =
	__ATTR(cfr_last_error, 0440, hdd_cfr_last_error_show, NULL);
static struct kobj_attribute hdd_cfr_last_sequence_attribute =
	__ATTR(cfr_last_sequence, 0440, hdd_cfr_last_sequence_show, NULL);
static struct kobj_attribute hdd_cfr_rearm_epoch_attribute =
	__ATTR(cfr_rearm_epoch, 0440, hdd_cfr_rearm_epoch_show, NULL);
static struct kobj_attribute hdd_cfr_rearm_failures_attribute =
	__ATTR(cfr_rearm_failures, 0440, hdd_cfr_rearm_failures_show, NULL);

static struct attribute *hdd_cfr_sysfs_attrs[] = {
	&hdd_cfr_status_attribute.attr,
	&hdd_cfr_control_attribute.attr,
	&hdd_cfr_capture_enabled_attribute.attr,
	&hdd_cfr_capture_count_supported_attribute.attr,
	&hdd_cfr_capture_interval_mode_attribute.attr,
	&hdd_cfr_capture_count_attribute.attr,
	&hdd_cfr_continuous_enabled_attribute.attr,
	&hdd_cfr_session_state_attribute.attr,
	&hdd_cfr_session_id_attribute.attr,
	&hdd_cfr_records_written_attribute.attr,
	&hdd_cfr_bytes_written_attribute.attr,
	&hdd_cfr_records_dropped_attribute.attr,
	&hdd_cfr_malformed_records_attribute.attr,
	&hdd_cfr_correlation_misses_attribute.attr,
	&hdd_cfr_relay_overruns_attribute.attr,
	&hdd_cfr_reader_resync_bytes_attribute.attr,
	&hdd_cfr_last_error_attribute.attr,
	&hdd_cfr_last_sequence_attribute.attr,
	&hdd_cfr_rearm_epoch_attribute.attr,
	&hdd_cfr_rearm_failures_attribute.attr,
	NULL,
};

static const struct attribute_group hdd_cfr_sysfs_group = {
	.attrs = hdd_cfr_sysfs_attrs,
};

void hdd_cfr_sysfs_create(struct kobject *parent)
{
	int ret;

	if (!parent)
		return;

	ret = sysfs_create_group(parent, &hdd_cfr_sysfs_group);
	if (ret)
		hdd_err("could not create bounded CFR sysfs group");
}

void hdd_cfr_sysfs_destroy(struct kobject *parent)
{
	if (!parent)
		return;

	sysfs_remove_group(parent, &hdd_cfr_sysfs_group);
}
