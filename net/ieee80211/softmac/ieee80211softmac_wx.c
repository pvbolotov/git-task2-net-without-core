/*
 * This file contains our _wx handlers. Make sure you EXPORT_SYMBOL_GPL them
 */

#include "ieee80211softmac_priv.h"

#include <net/iw_handler.h>


int
ieee80211softmac_wx_trigger_scan(struct net_device *net_dev,
				 struct iw_request_info *info,
				 union iwreq_data *data,
				 char *extra)
{
	struct ieee80211softmac_device *sm = ieee80211_priv(net_dev);
	return ieee80211softmac_start_scan(sm);
}
EXPORT_SYMBOL_GPL(ieee80211softmac_wx_trigger_scan);


int
ieee80211softmac_wx_get_scan_results(struct net_device *net_dev,
				     struct iw_request_info *info,
				     union iwreq_data *data,
				     char *extra)
{
	struct ieee80211softmac_device *sm = ieee80211_priv(net_dev);
	return ieee80211_wx_get_scan(sm->ieee, info, data, extra);
}
EXPORT_SYMBOL_GPL(ieee80211softmac_wx_get_scan_results);

int
ieee80211softmac_wx_set_essid(struct net_device *net_dev,
			      struct iw_request_info *info,
			      union iwreq_data *data,
			      char *extra)
{
	struct ieee80211softmac_device *sm = ieee80211_priv(net_dev);
	int length = 0;
	unsigned long flags;
	
	spin_lock_irqsave(&sm->lock, flags);
	
	sm->associnfo.static_essid = 0;

	if (data->essid.flags && data->essid.length && extra /*required?*/) {
		length = min(data->essid.length - 1, IW_ESSID_MAX_SIZE);
		if (length) {
			memcpy(sm->associnfo.req_essid.data, extra, length);
			sm->associnfo.static_essid = 1;
		}
	}
	sm->associnfo.scan_retry = IEEE80211SOFTMAC_ASSOC_SCAN_RETRY_LIMIT;

	/* set our requested ESSID length.
	 * If applicable, we have already copied the data in */
	sm->associnfo.req_essid.len = length;

	/* queue lower level code to do work (if necessary) */
	schedule_work(&sm->associnfo.work);

	spin_unlock_irqrestore(&sm->lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(ieee80211softmac_wx_set_essid);

int
ieee80211softmac_wx_get_essid(struct net_device *net_dev,
			      struct iw_request_info *info,
			      union iwreq_data *data,
			      char *extra)
{
	struct ieee80211softmac_device *sm = ieee80211_priv(net_dev);
	unsigned long flags;

	/* avoid getting inconsistent information */
	spin_lock_irqsave(&sm->lock, flags);
	/* If all fails, return ANY (empty) */
	data->essid.length = 0;
	data->essid.flags = 0;  /* active */
	
	/* If we have a statically configured ESSID then return it */
	if (sm->associnfo.static_essid) {
		data->essid.length = sm->associnfo.req_essid.len;
		data->essid.flags = 1;  /* active */
		memcpy(extra, sm->associnfo.req_essid.data, sm->associnfo.req_essid.len);
	}
	
	/* If we're associating/associated, return that */
	if (sm->associated || sm->associnfo.associating) {
		data->essid.length = sm->associnfo.associate_essid.len;
		data->essid.flags = 1;  /* active */
		memcpy(extra, sm->associnfo.associate_essid.data, sm->associnfo.associate_essid.len);
	}
	spin_unlock_irqrestore(&sm->lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(ieee80211softmac_wx_get_essid);

int
ieee80211softmac_wx_set_rate(struct net_device *net_dev,
			     struct iw_request_info *info,
			     union iwreq_data *data,
			     char *extra)
{
	struct ieee80211softmac_device *mac = ieee80211_priv(net_dev);
	struct ieee80211_device *ieee = mac->ieee;
	unsigned long flags;
	s32 in_rate = data->bitrate.value;
	u8 rate;
	int is_ofdm = 0;
	int err = -EINVAL;

	if (in_rate == -1) {
		/* automatic detect */
		if (ieee->modulation & IEEE80211_OFDM_MODULATION)
			in_rate = 54000000;
		else
			in_rate = 11000000;
	}

	switch (in_rate) {
	case 1000000:
		rate = IEEE80211_CCK_RATE_1MB;
		break;
	case 2000000:
		rate = IEEE80211_CCK_RATE_2MB;
		break;
	case 5500000:
		rate = IEEE80211_CCK_RATE_5MB;
		break;
	case 11000000:
		rate = IEEE80211_CCK_RATE_11MB;
		break;
	case 6000000:
		rate = IEEE80211_OFDM_RATE_6MB;
		is_ofdm = 1;
		break;
	case 9000000:
		rate = IEEE80211_OFDM_RATE_9MB;
		is_ofdm = 1;
		break;
	case 12000000:
		rate = IEEE80211_OFDM_RATE_12MB;
		is_ofdm = 1;
		break;
	case 18000000:
		rate = IEEE80211_OFDM_RATE_18MB;
		is_ofdm = 1;
		break;
	case 24000000:
		rate = IEEE80211_OFDM_RATE_24MB;
		is_ofdm = 1;
		break;
	case 36000000:
		rate = IEEE80211_OFDM_RATE_36MB;
		is_ofdm = 1;
		break;
	case 48000000:
		rate = IEEE80211_OFDM_RATE_48MB;
		is_ofdm = 1;
		break;
	case 54000000:
		rate = IEEE80211_OFDM_RATE_54MB;
		is_ofdm = 1;
		break;
	default:
		goto out;
	}

	spin_lock_irqsave(&mac->lock, flags);

	/* Check if correct modulation for this PHY. */
	if (is_ofdm && !(ieee->modulation & IEEE80211_OFDM_MODULATION))
		goto out_unlock;

	mac->txrates.default_rate = rate;
	mac->txrates.default_fallback = lower_rate(mac, rate);
	err = 0;

out_unlock:	
	spin_unlock_irqrestore(&mac->lock, flags);
out:
	return err;
}
EXPORT_SYMBOL_GPL(ieee80211softmac_wx_set_rate);

int
ieee80211softmac_wx_get_rate(struct net_device *net_dev,
			     struct iw_request_info *info,
			     union iwreq_data *data,
			     char *extra)
{
	struct ieee80211softmac_device *mac = ieee80211_priv(net_dev);
	unsigned long flags;
	int err = -EINVAL;

	spin_lock_irqsave(&mac->lock, flags);
	switch (mac->txrates.default_rate) {
	case IEEE80211_CCK_RATE_1MB:
		data->bitrate.value = 1000000;
		break;
	case IEEE80211_CCK_RATE_2MB:
		data->bitrate.value = 2000000;
		break;
	case IEEE80211_CCK_RATE_5MB:
		data->bitrate.value = 5500000;
		break;
	case IEEE80211_CCK_RATE_11MB:
		data->bitrate.value = 11000000;
		break;
	case IEEE80211_OFDM_RATE_6MB:
		data->bitrate.value = 6000000;
		break;
	case IEEE80211_OFDM_RATE_9MB:
		data->bitrate.value = 9000000;
		break;
	case IEEE80211_OFDM_RATE_12MB:
		data->bitrate.value = 12000000;
		break;
	case IEEE80211_OFDM_RATE_18MB:
		data->bitrate.value = 18000000;
		break;
	case IEEE80211_OFDM_RATE_24MB:
		data->bitrate.value = 24000000;
		break;
	case IEEE80211_OFDM_RATE_36MB:
		data->bitrate.value = 36000000;
		break;
	case IEEE80211_OFDM_RATE_48MB:
		data->bitrate.value = 48000000;
		break;
	case IEEE80211_OFDM_RATE_54MB:
		data->bitrate.value = 54000000;
		break;
	default:
		assert(0);
		goto out_unlock;
	}
	err = 0;
out_unlock:
	spin_unlock_irqrestore(&mac->lock, flags);

	return err;
}
EXPORT_SYMBOL_GPL(ieee80211softmac_wx_get_rate);

int
ieee80211softmac_wx_get_wap(struct net_device *net_dev,
			    struct iw_request_info *info,
			    union iwreq_data *data,
			    char *extra)
{
	struct ieee80211softmac_device *mac = ieee80211_priv(net_dev);
	int err = 0;
	unsigned long flags;

	spin_lock_irqsave(&mac->lock, flags);
	if (mac->associnfo.bssvalid)
		memcpy(data->ap_addr.sa_data, mac->associnfo.bssid, ETH_ALEN);
	else
		memset(data->ap_addr.sa_data, 0xff, ETH_ALEN);
	data->ap_addr.sa_family = ARPHRD_ETHER;
	spin_unlock_irqrestore(&mac->lock, flags);
	return err;
}
EXPORT_SYMBOL_GPL(ieee80211softmac_wx_get_wap);

int
ieee80211softmac_wx_set_wap(struct net_device *net_dev,
			    struct iw_request_info *info,
			    union iwreq_data *data,
			    char *extra)
{
	struct ieee80211softmac_device *mac = ieee80211_priv(net_dev);
	static const unsigned char any[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	static const unsigned char off[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	unsigned long flags;

	/* sanity check */
	if (data->ap_addr.sa_family != ARPHRD_ETHER) {
		return -EINVAL;
	}

	spin_lock_irqsave(&mac->lock, flags);
	if (!memcmp(any, data->ap_addr.sa_data, ETH_ALEN) ||
	    !memcmp(off, data->ap_addr.sa_data, ETH_ALEN)) {
		schedule_work(&mac->associnfo.work);
		goto out;
        } else {
		if (!memcmp(mac->associnfo.bssid, data->ap_addr.sa_data, ETH_ALEN)) {
			if (mac->associnfo.associating || mac->associated) {
			/* bssid unchanged and associated or associating - just return */
				goto out;
			}
		} else {
			/* copy new value in data->ap_addr.sa_data to bssid */
			memcpy(mac->associnfo.bssid, data->ap_addr.sa_data, ETH_ALEN);
		}	
		/* queue associate if new bssid or (old one again and not associated) */
		schedule_work(&mac->associnfo.work);
        }

out:
	spin_unlock_irqrestore(&mac->lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(ieee80211softmac_wx_set_wap);

int
ieee80211softmac_wx_set_genie(struct net_device *dev,
			      struct iw_request_info *info,
			      union iwreq_data *wrqu,
			      char *extra)
{
	struct ieee80211softmac_device *mac = ieee80211_priv(dev);
	unsigned long flags;
	int err = 0;
	char *buf;
	int i;
	
	spin_lock_irqsave(&mac->lock, flags);
	/* bleh. shouldn't be locked for that kmalloc... */

	if (wrqu->data.length) {
		if ((wrqu->data.length < 2) || (extra[1]+2 != wrqu->data.length)) {
			/* this is an IE, so the length must be
			 * correct. Is it possible though that
			 * more than one IE is passed in?
			 */
			err = -EINVAL;
			goto out;
		}
		if (mac->wpa.IEbuflen <= wrqu->data.length) {
			buf = kmalloc(wrqu->data.length, GFP_ATOMIC);
			if (!buf) {
				err = -ENOMEM;
				goto out;
			}
			kfree(mac->wpa.IE);
			mac->wpa.IE = buf;
			mac->wpa.IEbuflen = wrqu->data.length;
		}
		memcpy(mac->wpa.IE, extra, wrqu->data.length);
		dprintk(KERN_INFO PFX "generic IE set to ");
		for (i=0;i<wrqu->data.length;i++)
			dprintk("%.2x", mac->wpa.IE[i]);
		dprintk("\n");
		mac->wpa.IElen = wrqu->data.length;
	} else {
		kfree(mac->wpa.IE);
		mac->wpa.IE = NULL;
		mac->wpa.IElen = 0;
		mac->wpa.IEbuflen = 0;
	}

 out:	
	spin_unlock_irqrestore(&mac->lock, flags);
	return err;
}
EXPORT_SYMBOL_GPL(ieee80211softmac_wx_set_genie);

int
ieee80211softmac_wx_get_genie(struct net_device *dev,
			      struct iw_request_info *info,
			      union iwreq_data *wrqu,
			      char *extra)
{
	struct ieee80211softmac_device *mac = ieee80211_priv(dev);
	unsigned long flags;
	int err = 0;
	int space = wrqu->data.length;
	
	spin_lock_irqsave(&mac->lock, flags);
	
	wrqu->data.length = 0;
	
	if (mac->wpa.IE && mac->wpa.IElen) {
		wrqu->data.length = mac->wpa.IElen;
		if (mac->wpa.IElen <= space)
			memcpy(extra, mac->wpa.IE, mac->wpa.IElen);
		else
			err = -E2BIG;
	}
	spin_unlock_irqrestore(&mac->lock, flags);
	return err;
}
EXPORT_SYMBOL_GPL(ieee80211softmac_wx_get_genie);

