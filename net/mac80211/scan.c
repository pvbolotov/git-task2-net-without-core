/*
 * Scanning implementation
 *
 * Copyright 2003, Jouni Malinen <jkmaline@cc.hut.fi>
 * Copyright 2004, Instant802 Networks, Inc.
 * Copyright 2005, Devicescape Software, Inc.
 * Copyright 2006-2007	Jiri Benc <jbenc@suse.cz>
 * Copyright 2007, Michael Wu <flamingice@sourmilk.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/* TODO: figure out how to avoid that the "current BSS" expires */

#include <linux/wireless.h>
#include <linux/if_arp.h>
#include <linux/rtnetlink.h>
#include <net/mac80211.h>
#include <net/iw_handler.h>

#include "ieee80211_i.h"
#include "driver-ops.h"
#include "mesh.h"

#define IEEE80211_PROBE_DELAY (HZ / 33)
#define IEEE80211_CHANNEL_TIME (HZ / 33)
#define IEEE80211_PASSIVE_CHANNEL_TIME (HZ / 5)

struct ieee80211_bss *
ieee80211_rx_bss_get(struct ieee80211_local *local, u8 *bssid, int freq,
		     u8 *ssid, u8 ssid_len)
{
	return (void *)cfg80211_get_bss(local->hw.wiphy,
					ieee80211_get_channel(local->hw.wiphy,
							      freq),
					bssid, ssid, ssid_len,
					0, 0);
}

static void ieee80211_rx_bss_free(struct cfg80211_bss *cbss)
{
	struct ieee80211_bss *bss = (void *)cbss;

	kfree(bss_mesh_id(bss));
	kfree(bss_mesh_cfg(bss));
}

void ieee80211_rx_bss_put(struct ieee80211_local *local,
			  struct ieee80211_bss *bss)
{
	cfg80211_put_bss((struct cfg80211_bss *)bss);
}

struct ieee80211_bss *
ieee80211_bss_info_update(struct ieee80211_local *local,
			  struct ieee80211_rx_status *rx_status,
			  struct ieee80211_mgmt *mgmt,
			  size_t len,
			  struct ieee802_11_elems *elems,
			  struct ieee80211_channel *channel,
			  bool beacon)
{
	struct ieee80211_bss *bss;
	int clen;
	s32 signal = 0;

	if (local->hw.flags & IEEE80211_HW_SIGNAL_DBM)
		signal = rx_status->signal * 100;
	else if (local->hw.flags & IEEE80211_HW_SIGNAL_UNSPEC)
		signal = (rx_status->signal * 100) / local->hw.max_signal;

	bss = (void *)cfg80211_inform_bss_frame(local->hw.wiphy, channel,
						mgmt, len, signal, GFP_ATOMIC);

	if (!bss)
		return NULL;

	bss->cbss.free_priv = ieee80211_rx_bss_free;

	/* save the ERP value so that it is available at association time */
	if (elems->erp_info && elems->erp_info_len >= 1) {
		bss->erp_value = elems->erp_info[0];
		bss->has_erp_value = 1;
	}

	if (elems->tim) {
		struct ieee80211_tim_ie *tim_ie =
			(struct ieee80211_tim_ie *)elems->tim;
		bss->dtim_period = tim_ie->dtim_period;
	}

	/* set default value for buggy APs */
	if (!elems->tim || bss->dtim_period == 0)
		bss->dtim_period = 1;

	bss->supp_rates_len = 0;
	if (elems->supp_rates) {
		clen = IEEE80211_MAX_SUPP_RATES - bss->supp_rates_len;
		if (clen > elems->supp_rates_len)
			clen = elems->supp_rates_len;
		memcpy(&bss->supp_rates[bss->supp_rates_len], elems->supp_rates,
		       clen);
		bss->supp_rates_len += clen;
	}
	if (elems->ext_supp_rates) {
		clen = IEEE80211_MAX_SUPP_RATES - bss->supp_rates_len;
		if (clen > elems->ext_supp_rates_len)
			clen = elems->ext_supp_rates_len;
		memcpy(&bss->supp_rates[bss->supp_rates_len],
		       elems->ext_supp_rates, clen);
		bss->supp_rates_len += clen;
	}

	bss->wmm_used = elems->wmm_param || elems->wmm_info;

	if (!beacon)
		bss->last_probe_resp = jiffies;

	return bss;
}

void ieee80211_rx_bss_remove(struct ieee80211_sub_if_data *sdata, u8 *bssid,
			     int freq, u8 *ssid, u8 ssid_len)
{
	struct ieee80211_bss *bss;
	struct ieee80211_local *local = sdata->local;

	bss = ieee80211_rx_bss_get(local, bssid, freq, ssid, ssid_len);
	if (bss) {
		cfg80211_unlink_bss(local->hw.wiphy, (void *)bss);
		ieee80211_rx_bss_put(local, bss);
	}
}

ieee80211_rx_result
ieee80211_scan_rx(struct ieee80211_sub_if_data *sdata, struct sk_buff *skb,
		  struct ieee80211_rx_status *rx_status)
{
	struct ieee80211_mgmt *mgmt;
	struct ieee80211_bss *bss;
	u8 *elements;
	struct ieee80211_channel *channel;
	size_t baselen;
	int freq;
	__le16 fc;
	bool presp, beacon = false;
	struct ieee802_11_elems elems;

	if (skb->len < 2)
		return RX_DROP_UNUSABLE;

	mgmt = (struct ieee80211_mgmt *) skb->data;
	fc = mgmt->frame_control;

	if (ieee80211_is_ctl(fc))
		return RX_CONTINUE;

	if (skb->len < 24)
		return RX_DROP_MONITOR;

	presp = ieee80211_is_probe_resp(fc);
	if (presp) {
		/* ignore ProbeResp to foreign address */
		if (memcmp(mgmt->da, sdata->dev->dev_addr, ETH_ALEN))
			return RX_DROP_MONITOR;

		presp = true;
		elements = mgmt->u.probe_resp.variable;
		baselen = offsetof(struct ieee80211_mgmt, u.probe_resp.variable);
	} else {
		beacon = ieee80211_is_beacon(fc);
		baselen = offsetof(struct ieee80211_mgmt, u.beacon.variable);
		elements = mgmt->u.beacon.variable;
	}

	if (!presp && !beacon)
		return RX_CONTINUE;

	if (baselen > skb->len)
		return RX_DROP_MONITOR;

	ieee802_11_parse_elems(elements, skb->len - baselen, &elems);

	if (elems.ds_params && elems.ds_params_len == 1)
		freq = ieee80211_channel_to_frequency(elems.ds_params[0]);
	else
		freq = rx_status->freq;

	channel = ieee80211_get_channel(sdata->local->hw.wiphy, freq);

	if (!channel || channel->flags & IEEE80211_CHAN_DISABLED)
		return RX_DROP_MONITOR;

	bss = ieee80211_bss_info_update(sdata->local, rx_status,
					mgmt, skb->len, &elems,
					channel, beacon);
	if (bss)
		ieee80211_rx_bss_put(sdata->local, bss);

	dev_kfree_skb(skb);
	return RX_QUEUED;
}

/*
 * inform AP that we will go to sleep so that it will buffer the frames
 * while we scan
 */
static void ieee80211_scan_ps_enable(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_local *local = sdata->local;
	bool ps = false;

	/* FIXME: what to do when local->pspolling is true? */

	del_timer_sync(&local->dynamic_ps_timer);
	cancel_work_sync(&local->dynamic_ps_enable_work);

	if (local->hw.conf.flags & IEEE80211_CONF_PS) {
		ps = true;
		local->hw.conf.flags &= ~IEEE80211_CONF_PS;
		ieee80211_hw_config(local, IEEE80211_CONF_CHANGE_PS);
	}

	if (!ps || !(local->hw.flags & IEEE80211_HW_PS_NULLFUNC_STACK))
		/*
		 * If power save was enabled, no need to send a nullfunc
		 * frame because AP knows that we are sleeping. But if the
		 * hardware is creating the nullfunc frame for power save
		 * status (ie. IEEE80211_HW_PS_NULLFUNC_STACK is not
		 * enabled) and power save was enabled, the firmware just
		 * sent a null frame with power save disabled. So we need
		 * to send a new nullfunc frame to inform the AP that we
		 * are again sleeping.
		 */
		ieee80211_send_nullfunc(local, sdata, 1);
}

/* inform AP that we are awake again, unless power save is enabled */
static void ieee80211_scan_ps_disable(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_local *local = sdata->local;

	if (!local->ps_sdata)
		ieee80211_send_nullfunc(local, sdata, 0);
	else {
		/*
		 * In !IEEE80211_HW_PS_NULLFUNC_STACK case the hardware
		 * will send a nullfunc frame with the powersave bit set
		 * even though the AP already knows that we are sleeping.
		 * This could be avoided by sending a null frame with power
		 * save bit disabled before enabling the power save, but
		 * this doesn't gain anything.
		 *
		 * When IEEE80211_HW_PS_NULLFUNC_STACK is enabled, no need
		 * to send a nullfunc frame because AP already knows that
		 * we are sleeping, let's just enable power save mode in
		 * hardware.
		 */
		local->hw.conf.flags |= IEEE80211_CONF_PS;
		ieee80211_hw_config(local, IEEE80211_CONF_CHANGE_PS);
	}
}

static void ieee80211_restore_scan_ies(struct ieee80211_local *local)
{
	kfree(local->scan_req->ie);
	local->scan_req->ie = local->orig_ies;
	local->scan_req->ie_len = local->orig_ies_len;
}

void ieee80211_scan_completed(struct ieee80211_hw *hw, bool aborted)
{
	struct ieee80211_local *local = hw_to_local(hw);
	struct ieee80211_sub_if_data *sdata;
	bool was_hw_scan;

	mutex_lock(&local->scan_mtx);

	if (WARN_ON(!local->hw_scanning && !local->sw_scanning)) {
		mutex_unlock(&local->scan_mtx);
		return;
	}

	if (WARN_ON(!local->scan_req)) {
		mutex_unlock(&local->scan_mtx);
		return;
	}

	if (local->hw_scanning)
		ieee80211_restore_scan_ies(local);

	if (local->scan_req != &local->int_scan_req)
		cfg80211_scan_done(local->scan_req, aborted);
	local->scan_req = NULL;

	was_hw_scan = local->hw_scanning;
	local->hw_scanning = false;
	local->sw_scanning = false;

	/* we only have to protect scan_req and hw/sw scan */
	mutex_unlock(&local->scan_mtx);

	ieee80211_hw_config(local, IEEE80211_CONF_CHANGE_CHANNEL);
	if (was_hw_scan)
		goto done;

	netif_tx_lock_bh(local->mdev);
	netif_addr_lock(local->mdev);
	local->filter_flags &= ~FIF_BCN_PRBRESP_PROMISC;
	drv_configure_filter(local, FIF_BCN_PRBRESP_PROMISC,
			     &local->filter_flags,
			     local->mdev->mc_count,
			     local->mdev->mc_list);

	netif_addr_unlock(local->mdev);
	netif_tx_unlock_bh(local->mdev);

	drv_sw_scan_complete(local);

	mutex_lock(&local->iflist_mtx);
	list_for_each_entry(sdata, &local->interfaces, list) {
		if (!netif_running(sdata->dev))
			continue;

		/* Tell AP we're back */
		if (sdata->vif.type == NL80211_IFTYPE_STATION) {
			if (sdata->u.mgd.flags & IEEE80211_STA_ASSOCIATED) {
				ieee80211_scan_ps_disable(sdata);
				netif_tx_wake_all_queues(sdata->dev);
			}
		} else
			netif_tx_wake_all_queues(sdata->dev);

		/* re-enable beaconing */
		if (sdata->vif.type == NL80211_IFTYPE_AP ||
		    sdata->vif.type == NL80211_IFTYPE_ADHOC ||
		    sdata->vif.type == NL80211_IFTYPE_MESH_POINT)
			ieee80211_bss_info_change_notify(
				sdata, BSS_CHANGED_BEACON_ENABLED);
	}
	mutex_unlock(&local->iflist_mtx);

 done:
	ieee80211_recalc_idle(local);
	ieee80211_mlme_notify_scan_completed(local);
	ieee80211_ibss_notify_scan_completed(local);
	ieee80211_mesh_notify_scan_completed(local);
}
EXPORT_SYMBOL(ieee80211_scan_completed);

static int ieee80211_start_sw_scan(struct ieee80211_local *local)
{
	struct ieee80211_sub_if_data *sdata;

	/*
	 * Hardware/driver doesn't support hw_scan, so use software
	 * scanning instead. First send a nullfunc frame with power save
	 * bit on so that AP will buffer the frames for us while we are not
	 * listening, then send probe requests to each channel and wait for
	 * the responses. After all channels are scanned, tune back to the
	 * original channel and send a nullfunc frame with power save bit
	 * off to trigger the AP to send us all the buffered frames.
	 *
	 * Note that while local->sw_scanning is true everything else but
	 * nullfunc frames and probe requests will be dropped in
	 * ieee80211_tx_h_check_assoc().
	 */
	drv_sw_scan_start(local);

	mutex_lock(&local->iflist_mtx);
	list_for_each_entry(sdata, &local->interfaces, list) {
		if (!netif_running(sdata->dev))
			continue;

		/* disable beaconing */
		if (sdata->vif.type == NL80211_IFTYPE_AP ||
		    sdata->vif.type == NL80211_IFTYPE_ADHOC ||
		    sdata->vif.type == NL80211_IFTYPE_MESH_POINT)
			ieee80211_bss_info_change_notify(
				sdata, BSS_CHANGED_BEACON_ENABLED);

		if (sdata->vif.type == NL80211_IFTYPE_STATION) {
			if (sdata->u.mgd.flags & IEEE80211_STA_ASSOCIATED) {
				netif_tx_stop_all_queues(sdata->dev);
				ieee80211_scan_ps_enable(sdata);
			}
		} else
			netif_tx_stop_all_queues(sdata->dev);
	}
	mutex_unlock(&local->iflist_mtx);

	local->scan_state = SCAN_SET_CHANNEL;
	local->scan_channel_idx = 0;

	netif_addr_lock_bh(local->mdev);
	local->filter_flags |= FIF_BCN_PRBRESP_PROMISC;
	drv_configure_filter(local, FIF_BCN_PRBRESP_PROMISC,
			     &local->filter_flags,
			     local->mdev->mc_count,
			     local->mdev->mc_list);
	netif_addr_unlock_bh(local->mdev);

	/* TODO: start scan as soon as all nullfunc frames are ACKed */
	queue_delayed_work(local->hw.workqueue, &local->scan_work,
			   IEEE80211_CHANNEL_TIME);

	return 0;
}


static int __ieee80211_start_scan(struct ieee80211_sub_if_data *sdata,
				  struct cfg80211_scan_request *req)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_managed *ifmgd = &sdata->u.mgd;
	int rc;

	if (local->scan_req)
		return -EBUSY;

	if (local->ops->hw_scan) {
		u8 *ies;
		int ielen;

		ies = kmalloc(2 + IEEE80211_MAX_SSID_LEN +
			      local->scan_ies_len + req->ie_len, GFP_KERNEL);
		if (!ies)
			return -ENOMEM;

		ielen = ieee80211_build_preq_ies(local, ies,
						 req->ie, req->ie_len);
		local->orig_ies = req->ie;
		local->orig_ies_len = req->ie_len;
		req->ie = ies;
		req->ie_len = ielen;
	}

	local->scan_req = req;
	local->scan_sdata = sdata;

	if (req != &local->int_scan_req &&
	    sdata->vif.type == NL80211_IFTYPE_STATION &&
	    (ifmgd->state == IEEE80211_STA_MLME_DIRECT_PROBE ||
	     ifmgd->state == IEEE80211_STA_MLME_AUTHENTICATE ||
	     ifmgd->state == IEEE80211_STA_MLME_ASSOCIATE)) {
		/* actually wait for the assoc to finish/time out */
		set_bit(IEEE80211_STA_REQ_SCAN, &ifmgd->request);
		return 0;
	}

	if (local->ops->hw_scan)
		local->hw_scanning = true;
	else
		local->sw_scanning = true;
	/*
	 * Kicking off the scan need not be protected,
	 * only the scan variable stuff, since now
	 * local->scan_req is assigned and other callers
	 * will abort their scan attempts.
	 *
	 * This avoids getting a scan_mtx -> iflist_mtx
	 * dependency, so that the scan completed calls
	 * have more locking freedom.
	 */

	ieee80211_recalc_idle(local);
	mutex_unlock(&local->scan_mtx);

	if (local->ops->hw_scan)
		rc = drv_hw_scan(local, local->scan_req);
	else
		rc = ieee80211_start_sw_scan(local);

	mutex_lock(&local->scan_mtx);

	if (rc) {
		if (local->ops->hw_scan) {
			local->hw_scanning = false;
			ieee80211_restore_scan_ies(local);
		} else
			local->sw_scanning = false;

		ieee80211_recalc_idle(local);

		local->scan_req = NULL;
		local->scan_sdata = NULL;
	}

	return rc;
}

void ieee80211_scan_work(struct work_struct *work)
{
	struct ieee80211_local *local =
		container_of(work, struct ieee80211_local, scan_work.work);
	struct ieee80211_sub_if_data *sdata = local->scan_sdata;
	struct ieee80211_channel *chan;
	int skip, i;
	unsigned long next_delay = 0;

	mutex_lock(&local->scan_mtx);
	if (!sdata || !local->scan_req) {
		mutex_unlock(&local->scan_mtx);
		return;
	}

	if (local->scan_req && !(local->sw_scanning || local->hw_scanning)) {
		struct cfg80211_scan_request *req = local->scan_req;
		int rc;

		local->scan_req = NULL;

		rc = __ieee80211_start_scan(sdata, req);
		mutex_unlock(&local->scan_mtx);

		if (rc)
			ieee80211_scan_completed(&local->hw, true);
		return;
	}

	mutex_unlock(&local->scan_mtx);

	/*
	 * Avoid re-scheduling when the sdata is going away.
	 */
	if (!netif_running(sdata->dev)) {
		ieee80211_scan_completed(&local->hw, true);
		return;
	}

	switch (local->scan_state) {
	case SCAN_SET_CHANNEL:
		/* if no more bands/channels left, complete scan */
		if (local->scan_channel_idx >= local->scan_req->n_channels) {
			ieee80211_scan_completed(&local->hw, false);
			return;
		}
		skip = 0;
		chan = local->scan_req->channels[local->scan_channel_idx];

		if (chan->flags & IEEE80211_CHAN_DISABLED ||
		    (sdata->vif.type == NL80211_IFTYPE_ADHOC &&
		     chan->flags & IEEE80211_CHAN_NO_IBSS))
			skip = 1;

		if (!skip) {
			local->scan_channel = chan;
			if (ieee80211_hw_config(local,
						IEEE80211_CONF_CHANGE_CHANNEL))
				skip = 1;
		}

		/* advance state machine to next channel/band */
		local->scan_channel_idx++;

		if (skip)
			break;

		next_delay = IEEE80211_PROBE_DELAY +
			     usecs_to_jiffies(local->hw.channel_change_time);
		local->scan_state = SCAN_SEND_PROBE;
		break;
	case SCAN_SEND_PROBE:
		next_delay = IEEE80211_PASSIVE_CHANNEL_TIME;
		local->scan_state = SCAN_SET_CHANNEL;

		if (local->scan_channel->flags & IEEE80211_CHAN_PASSIVE_SCAN ||
		    !local->scan_req->n_ssids)
			break;
		for (i = 0; i < local->scan_req->n_ssids; i++)
			ieee80211_send_probe_req(
				sdata, NULL,
				local->scan_req->ssids[i].ssid,
				local->scan_req->ssids[i].ssid_len,
				local->scan_req->ie, local->scan_req->ie_len);
		next_delay = IEEE80211_CHANNEL_TIME;
		break;
	}

	queue_delayed_work(local->hw.workqueue, &local->scan_work,
			   next_delay);
}

int ieee80211_request_scan(struct ieee80211_sub_if_data *sdata,
			   struct cfg80211_scan_request *req)
{
	int res;

	mutex_lock(&sdata->local->scan_mtx);
	res = __ieee80211_start_scan(sdata, req);
	mutex_unlock(&sdata->local->scan_mtx);

	return res;
}

int ieee80211_request_internal_scan(struct ieee80211_sub_if_data *sdata,
				    const u8 *ssid, u8 ssid_len)
{
	struct ieee80211_local *local = sdata->local;
	int ret = -EBUSY;

	mutex_lock(&local->scan_mtx);

	/* busy scanning */
	if (local->scan_req)
		goto unlock;

	memcpy(local->int_scan_req.ssids[0].ssid, ssid, IEEE80211_MAX_SSID_LEN);
	local->int_scan_req.ssids[0].ssid_len = ssid_len;

	ret = __ieee80211_start_scan(sdata, &sdata->local->int_scan_req);
 unlock:
	mutex_unlock(&local->scan_mtx);
	return ret;
}
