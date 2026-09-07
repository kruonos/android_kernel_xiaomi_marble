/*
 * Copyright (c) 2020 The Linux Foundation. All rights reserved.
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

/**
 * DOC : target_if_cfr_6490.c
 *
 * Target interface of CFR for QCA6490 implementation
 *
 */

#include <cdp_txrx_ctrl.h>
#include "target_if_cfr.h"
#include <qdf_nbuf.h>
#include "wlan_cfr_utils_api.h"
#include "target_if_cfr_6490.h"
#include "target_if_cfr_enh.h"
#include "init_deinit_lmac.h"
#include "cfg_ucfg_api.h"
#include "cfr_cfg.h"

#ifdef WLAN_ENH_CFR_ENABLE
#ifdef CFR_USE_FIXED_FOLDER
static void target_cfr_callback(void *pdev_obj, enum WDI_EVENT event,
				void *data, u_int16_t peer_id,
				uint32_t status)
{
	struct wlan_objmgr_pdev *pdev;
	struct pdev_cfr *pcfr;
	qdf_nbuf_t nbuf = (qdf_nbuf_t)data;

	pdev = (struct wlan_objmgr_pdev *)pdev_obj;
	if (qdf_unlikely((!pdev || !data))) {
		cfr_err("Invalid pdev %pK or data %pK for event %d",
			pdev, data, event);
		qdf_nbuf_free(nbuf);
		return;
	}
	if (QDF_IS_STATUS_ERROR(
			wlan_objmgr_pdev_try_get_ref(pdev, WLAN_CFR_ID))) {
		qdf_nbuf_free(nbuf);
		return;
	}

	pcfr = wlan_objmgr_pdev_get_comp_private_obj(pdev,
						      WLAN_UMAC_COMP_CFR);
	if (pcfr)
		pcfr->ppdu_cb_cnt++;

	if (event != WDI_EVENT_RX_PPDU_DESC) {
		cfr_debug("event is %d", event);
		qdf_nbuf_free(nbuf);
		wlan_objmgr_pdev_release_ref(pdev, WLAN_CFR_ID);
		return;
	}

	/* WDI transfers ownership of this dedicated CFR snapshot callback. */
	wlan_cfr_rx_tlv_process(pdev, data);
	wlan_objmgr_pdev_release_ref(pdev, WLAN_CFR_ID);
}

QDF_STATUS
target_if_cfr_subscribe_ppdu_desc(struct wlan_objmgr_pdev *pdev,
				  bool is_subscribe)
{
	ol_txrx_soc_handle soc;
	struct wlan_objmgr_psoc *psoc;
	struct pdev_cfr *pcfr;
	wdi_event_subscribe *cfr_subscribe;
	uint8_t dp_pdev_id;
	int status;
	QDF_STATUS result = QDF_STATUS_SUCCESS;

	if (!pdev) {
		cfr_err("Null pdev");
		return QDF_STATUS_E_INVAL;
	}

	pcfr = wlan_objmgr_pdev_get_comp_private_obj(
				pdev, WLAN_UMAC_COMP_CFR);
	if (!pcfr) {
		cfr_err("pcfr is NULL");
		return QDF_STATUS_E_INVAL;
	}

	psoc = wlan_pdev_get_psoc(pdev);
	if (!psoc) {
		cfr_err("Null psoc");
		return QDF_STATUS_E_INVAL;
	}

	soc = wlan_psoc_get_dp_handle(psoc);
	if (!soc) {
		cfr_err("Null soc");
		return QDF_STATUS_E_INVAL;
	}

	dp_pdev_id = wlan_objmgr_pdev_get_pdev_id(pdev);
	qdf_mutex_acquire(&pcfr->ppdu_sub_lock);
	pcfr->ppdu_sub_dp_pdev_id = dp_pdev_id;
	cfr_subscribe = pcfr->ppdu_subscribe_ctx;
	if (!cfr_subscribe && is_subscribe) {
		cfr_subscribe = qdf_mem_malloc(sizeof(*cfr_subscribe));
		if (!cfr_subscribe) {
			result = QDF_STATUS_E_NOMEM;
			goto out;
		}
		cfr_subscribe->callback = target_cfr_callback;
		pcfr->ppdu_subscribe_ctx = cfr_subscribe;
	}

	if (is_subscribe) {
		if (pcfr->ppdu_subscribed) {
			cfr_subscribe->context = pdev;
			goto out;
		}
		cfr_subscribe->context = pdev;
		status = cdp_wdi_event_sub(soc, dp_pdev_id, cfr_subscribe,
					 WDI_EVENT_RX_PPDU_DESC);
		pcfr->ppdu_sub_status = status;
		if (status) {
			pcfr->ppdu_sub_fail_cnt++;
			cfr_err("wdi event sub fail");
			cfr_subscribe->context = NULL;
			result = QDF_STATUS_E_FAILURE;
			goto out;
		}
		cdp_set_cfr_rcc(soc, dp_pdev_id, true);
		cdp_enable_mon_reap_timer(soc, dp_pdev_id, true);
		pcfr->ppdu_subscribed = 1;
	} else {
		if (!pcfr->ppdu_subscribed) {
			if (cfr_subscribe)
				cfr_subscribe->context = NULL;
			goto out;
		}
		if (!cfr_subscribe) {
			result = QDF_STATUS_E_FAILURE;
			goto out;
		}
		status = cdp_wdi_event_unsub(soc, dp_pdev_id, cfr_subscribe,
					   WDI_EVENT_RX_PPDU_DESC);
		pcfr->ppdu_sub_status = status;
		if (status) {
			pcfr->ppdu_sub_fail_cnt++;
			cfr_err("wdi event unsub fail");
			cfr_subscribe->context = NULL;
			result = QDF_STATUS_E_FAILURE;
			goto out;
		}
		cdp_set_cfr_rcc(soc, dp_pdev_id, false);
		cdp_enable_mon_reap_timer(soc, dp_pdev_id, false);
		pcfr->ppdu_subscribed = 0;
		cfr_subscribe->context = NULL;
	}

out:
	qdf_mutex_release(&pcfr->ppdu_sub_lock);
	return result;
}
#endif /* CFR_USE_FIXED_FOLDER */

QDF_STATUS target_if_cfr_set_dp_pipeline(struct wlan_objmgr_pdev *pdev,
					 bool enable)
{
	ol_txrx_soc_handle soc;
	struct wlan_objmgr_psoc *psoc;
	struct pdev_cfr *pcfr;
	uint8_t dp_pdev_id;
	QDF_STATUS status = QDF_STATUS_SUCCESS;

	if (!pdev)
		return QDF_STATUS_E_INVAL;

	pcfr = wlan_objmgr_pdev_get_comp_private_obj(pdev,
						    WLAN_UMAC_COMP_CFR);
	psoc = wlan_pdev_get_psoc(pdev);
	if (!pcfr || !psoc)
		return QDF_STATUS_E_INVAL;

	soc = wlan_psoc_get_dp_handle(psoc);
	if (!soc)
		return QDF_STATUS_E_INVAL;

	dp_pdev_id = wlan_objmgr_pdev_get_pdev_id(pdev);
	qdf_mutex_acquire(&pcfr->ppdu_sub_lock);
	if (!pcfr->ppdu_subscribed || !pcfr->ppdu_subscribe_ctx) {
		status = QDF_STATUS_E_FAILURE;
		goto out;
	}

	cdp_set_cfr_rcc(soc, dp_pdev_id, enable);
	cdp_enable_mon_reap_timer(soc, dp_pdev_id, enable);
out:
	qdf_mutex_release(&pcfr->ppdu_sub_lock);
	return status;
}
#endif /* WLAN_ENH_CFR_ENABLE */
