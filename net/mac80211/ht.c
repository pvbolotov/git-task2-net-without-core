/*
 * HT handling
 *
 * Copyright 2003, Jouni Malinen <jkmaline@cc.hut.fi>
 * Copyright 2002-2005, Instant802 Networks, Inc.
 * Copyright 2005-2006, Devicescape Software, Inc.
 * Copyright 2006-2007	Jiri Benc <jbenc@suse.cz>
 * Copyright 2007, Michael Wu <flamingice@sourmilk.net>
 * Copyright 2007-2008, Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/ieee80211.h>
#include <net/wireless.h>
#include <net/mac80211.h>
#include "ieee80211_i.h"

void ieee80211_ht_cap_ie_to_sta_ht_cap(struct ieee80211_supported_band *sband,
				       struct ieee80211_ht_cap *ht_cap_ie,
				       struct ieee80211_sta_ht_cap *ht_cap)
{
	u8 ampdu_info, tx_mcs_set_cap;
	int i, max_tx_streams;

	BUG_ON(!ht_cap);

	memset(ht_cap, 0, sizeof(*ht_cap));

	if (!ht_cap_ie)
		return;

	ht_cap->ht_supported = true;

	ht_cap->cap = le16_to_cpu(ht_cap_ie->cap_info) & sband->ht_cap.cap;
	ht_cap->cap &= ~IEEE80211_HT_CAP_SM_PS;
	ht_cap->cap |= sband->ht_cap.cap & IEEE80211_HT_CAP_SM_PS;

	ampdu_info = ht_cap_ie->ampdu_params_info;
	ht_cap->ampdu_factor =
		ampdu_info & IEEE80211_HT_AMPDU_PARM_FACTOR;
	ht_cap->ampdu_density =
		(ampdu_info & IEEE80211_HT_AMPDU_PARM_DENSITY) >> 2;

	/* own MCS TX capabilities */
	tx_mcs_set_cap = sband->ht_cap.mcs.tx_params;

	/* can we TX with MCS rates? */
	if (!(tx_mcs_set_cap & IEEE80211_HT_MCS_TX_DEFINED))
		return;

	/* Counting from 0, therefore +1 */
	if (tx_mcs_set_cap & IEEE80211_HT_MCS_TX_RX_DIFF)
		max_tx_streams =
			((tx_mcs_set_cap & IEEE80211_HT_MCS_TX_MAX_STREAMS_MASK)
				>> IEEE80211_HT_MCS_TX_MAX_STREAMS_SHIFT) + 1;
	else
		max_tx_streams = IEEE80211_HT_MCS_TX_MAX_STREAMS;

	/*
	 * 802.11n D5.0 20.3.5 / 20.6 says:
	 * - indices 0 to 7 and 32 are single spatial stream
	 * - 8 to 31 are multiple spatial streams using equal modulation
	 *   [8..15 for two streams, 16..23 for three and 24..31 for four]
	 * - remainder are multiple spatial streams using unequal modulation
	 */
	for (i = 0; i < max_tx_streams; i++)
		ht_cap->mcs.rx_mask[i] =
			sband->ht_cap.mcs.rx_mask[i] & ht_cap_ie->mcs.rx_mask[i];

	if (tx_mcs_set_cap & IEEE80211_HT_MCS_TX_UNEQUAL_MODULATION)
		for (i = IEEE80211_HT_MCS_UNEQUAL_MODULATION_START_BYTE;
		     i < IEEE80211_HT_MCS_MASK_LEN; i++)
			ht_cap->mcs.rx_mask[i] =
				sband->ht_cap.mcs.rx_mask[i] &
					ht_cap_ie->mcs.rx_mask[i];

	/* handle MCS rate 32 too */
	if (sband->ht_cap.mcs.rx_mask[32/8] & ht_cap_ie->mcs.rx_mask[32/8] & 1)
		ht_cap->mcs.rx_mask[32/8] |= 1;
}

/*
 * ieee80211_enable_ht should be called only after the operating band
 * has been determined as ht configuration depends on the hw's
 * HT abilities for a specific band.
 */
u32 ieee80211_enable_ht(struct ieee80211_sub_if_data *sdata,
			struct ieee80211_ht_info *hti,
			u16 ap_ht_cap_flags)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_supported_band *sband;
	struct ieee80211_bss_ht_conf ht;
	u32 changed = 0;
	bool enable_ht = true, ht_changed;
	enum nl80211_channel_type channel_type = NL80211_CHAN_NO_HT;

	sband = local->hw.wiphy->bands[local->hw.conf.channel->band];

	memset(&ht, 0, sizeof(ht));

	/* HT is not supported */
	if (!sband->ht_cap.ht_supported)
		enable_ht = false;

	/* check that channel matches the right operating channel */
	if (local->hw.conf.channel->center_freq !=
	    ieee80211_channel_to_frequency(hti->control_chan))
		enable_ht = false;

	if (enable_ht) {
		channel_type = NL80211_CHAN_HT20;

		if (!(ap_ht_cap_flags & IEEE80211_HT_CAP_40MHZ_INTOLERANT) &&
		    (sband->ht_cap.cap & IEEE80211_HT_CAP_SUP_WIDTH_20_40) &&
		    (hti->ht_param & IEEE80211_HT_PARAM_CHAN_WIDTH_ANY)) {
			switch(hti->ht_param & IEEE80211_HT_PARAM_CHA_SEC_OFFSET) {
			case IEEE80211_HT_PARAM_CHA_SEC_ABOVE:
				channel_type = NL80211_CHAN_HT40PLUS;
				break;
			case IEEE80211_HT_PARAM_CHA_SEC_BELOW:
				channel_type = NL80211_CHAN_HT40MINUS;
				break;
			}
		}
	}

	ht_changed = conf_is_ht(&local->hw.conf) != enable_ht ||
		     channel_type != local->hw.conf.channel_type;

	local->oper_channel_type = channel_type;

	if (ht_changed) {
                /* channel_type change automatically detected */
		ieee80211_hw_config(local, 0);
        }

	/* disable HT */
	if (!enable_ht)
		return 0;

	ht.operation_mode = le16_to_cpu(hti->operation_mode);

	/* if bss configuration changed store the new one */
	if (memcmp(&sdata->vif.bss_conf.ht, &ht, sizeof(ht))) {
		changed |= BSS_CHANGED_HT;
		sdata->vif.bss_conf.ht = ht;
	}

	return changed;
}

void ieee80211_sta_tear_down_BA_sessions(struct ieee80211_sub_if_data *sdata, u8 *addr)
{
	struct ieee80211_local *local = sdata->local;
	int i;

	for (i = 0; i <  STA_TID_NUM; i++) {
		ieee80211_stop_tx_ba_session(&local->hw, addr, i,
					     WLAN_BACK_INITIATOR);
		ieee80211_sta_stop_rx_ba_session(sdata, addr, i,
						 WLAN_BACK_RECIPIENT,
						 WLAN_REASON_QSTA_LEAVE_QBSS);
	}
}

void ieee80211_send_delba(struct ieee80211_sub_if_data *sdata,
			  const u8 *da, u16 tid,
			  u16 initiator, u16 reason_code)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_sta *ifsta = &sdata->u.sta;
	struct sk_buff *skb;
	struct ieee80211_mgmt *mgmt;
	u16 params;

	skb = dev_alloc_skb(sizeof(*mgmt) + local->hw.extra_tx_headroom);

	if (!skb) {
		printk(KERN_ERR "%s: failed to allocate buffer "
					"for delba frame\n", sdata->dev->name);
		return;
	}

	skb_reserve(skb, local->hw.extra_tx_headroom);
	mgmt = (struct ieee80211_mgmt *) skb_put(skb, 24);
	memset(mgmt, 0, 24);
	memcpy(mgmt->da, da, ETH_ALEN);
	memcpy(mgmt->sa, sdata->dev->dev_addr, ETH_ALEN);
	if (sdata->vif.type == NL80211_IFTYPE_AP ||
	    sdata->vif.type == NL80211_IFTYPE_AP_VLAN)
		memcpy(mgmt->bssid, sdata->dev->dev_addr, ETH_ALEN);
	else
		memcpy(mgmt->bssid, ifsta->bssid, ETH_ALEN);
	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
					  IEEE80211_STYPE_ACTION);

	skb_put(skb, 1 + sizeof(mgmt->u.action.u.delba));

	mgmt->u.action.category = WLAN_CATEGORY_BACK;
	mgmt->u.action.u.delba.action_code = WLAN_ACTION_DELBA;
	params = (u16)(initiator << 11); 	/* bit 11 initiator */
	params |= (u16)(tid << 12); 		/* bit 15:12 TID number */

	mgmt->u.action.u.delba.params = cpu_to_le16(params);
	mgmt->u.action.u.delba.reason_code = cpu_to_le16(reason_code);

	ieee80211_tx_skb(sdata, skb, 1);
}

void ieee80211_process_delba(struct ieee80211_sub_if_data *sdata,
			     struct sta_info *sta,
			     struct ieee80211_mgmt *mgmt, size_t len)
{
	struct ieee80211_local *local = sdata->local;
	u16 tid, params;
	u16 initiator;

	params = le16_to_cpu(mgmt->u.action.u.delba.params);
	tid = (params & IEEE80211_DELBA_PARAM_TID_MASK) >> 12;
	initiator = (params & IEEE80211_DELBA_PARAM_INITIATOR_MASK) >> 11;

#ifdef CONFIG_MAC80211_HT_DEBUG
	if (net_ratelimit())
		printk(KERN_DEBUG "delba from %pM (%s) tid %d reason code %d\n",
			mgmt->sa, initiator ? "initiator" : "recipient", tid,
			mgmt->u.action.u.delba.reason_code);
#endif /* CONFIG_MAC80211_HT_DEBUG */

	if (initiator == WLAN_BACK_INITIATOR)
		ieee80211_sta_stop_rx_ba_session(sdata, sta->sta.addr, tid,
						 WLAN_BACK_INITIATOR, 0);
	else { /* WLAN_BACK_RECIPIENT */
		spin_lock_bh(&sta->lock);
		sta->ampdu_mlme.tid_state_tx[tid] =
				HT_AGG_STATE_OPERATIONAL;
		spin_unlock_bh(&sta->lock);
		ieee80211_stop_tx_ba_session(&local->hw, sta->sta.addr, tid,
					     WLAN_BACK_RECIPIENT);
	}
}
