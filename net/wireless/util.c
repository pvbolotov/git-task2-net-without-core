/*
 * Wireless utility functions
 *
 * Copyright 2007-2009	Johannes Berg <johannes@sipsolutions.net>
 */
#include <linux/bitops.h>
#include <net/cfg80211.h>
#include "core.h"

struct ieee80211_rate *
ieee80211_get_response_rate(struct ieee80211_supported_band *sband,
			    u32 basic_rates, int bitrate)
{
	struct ieee80211_rate *result = &sband->bitrates[0];
	int i;

	for (i = 0; i < sband->n_bitrates; i++) {
		if (!(basic_rates & BIT(i)))
			continue;
		if (sband->bitrates[i].bitrate > bitrate)
			continue;
		result = &sband->bitrates[i];
	}

	return result;
}
EXPORT_SYMBOL(ieee80211_get_response_rate);

int ieee80211_channel_to_frequency(int chan)
{
	if (chan < 14)
		return 2407 + chan * 5;

	if (chan == 14)
		return 2484;

	/* FIXME: 802.11j 17.3.8.3.2 */
	return (chan + 1000) * 5;
}
EXPORT_SYMBOL(ieee80211_channel_to_frequency);

int ieee80211_frequency_to_channel(int freq)
{
	if (freq == 2484)
		return 14;

	if (freq < 2484)
		return (freq - 2407) / 5;

	/* FIXME: 802.11j 17.3.8.3.2 */
	return freq/5 - 1000;
}
EXPORT_SYMBOL(ieee80211_frequency_to_channel);

struct ieee80211_channel *__ieee80211_get_channel(struct wiphy *wiphy,
						  int freq)
{
	enum ieee80211_band band;
	struct ieee80211_supported_band *sband;
	int i;

	for (band = 0; band < IEEE80211_NUM_BANDS; band++) {
		sband = wiphy->bands[band];

		if (!sband)
			continue;

		for (i = 0; i < sband->n_channels; i++) {
			if (sband->channels[i].center_freq == freq)
				return &sband->channels[i];
		}
	}

	return NULL;
}
EXPORT_SYMBOL(__ieee80211_get_channel);

static void set_mandatory_flags_band(struct ieee80211_supported_band *sband,
				     enum ieee80211_band band)
{
	int i, want;

	switch (band) {
	case IEEE80211_BAND_5GHZ:
		want = 3;
		for (i = 0; i < sband->n_bitrates; i++) {
			if (sband->bitrates[i].bitrate == 60 ||
			    sband->bitrates[i].bitrate == 120 ||
			    sband->bitrates[i].bitrate == 240) {
				sband->bitrates[i].flags |=
					IEEE80211_RATE_MANDATORY_A;
				want--;
			}
		}
		WARN_ON(want);
		break;
	case IEEE80211_BAND_2GHZ:
		want = 7;
		for (i = 0; i < sband->n_bitrates; i++) {
			if (sband->bitrates[i].bitrate == 10) {
				sband->bitrates[i].flags |=
					IEEE80211_RATE_MANDATORY_B |
					IEEE80211_RATE_MANDATORY_G;
				want--;
			}

			if (sband->bitrates[i].bitrate == 20 ||
			    sband->bitrates[i].bitrate == 55 ||
			    sband->bitrates[i].bitrate == 110 ||
			    sband->bitrates[i].bitrate == 60 ||
			    sband->bitrates[i].bitrate == 120 ||
			    sband->bitrates[i].bitrate == 240) {
				sband->bitrates[i].flags |=
					IEEE80211_RATE_MANDATORY_G;
				want--;
			}

			if (sband->bitrates[i].bitrate != 10 &&
			    sband->bitrates[i].bitrate != 20 &&
			    sband->bitrates[i].bitrate != 55 &&
			    sband->bitrates[i].bitrate != 110)
				sband->bitrates[i].flags |=
					IEEE80211_RATE_ERP_G;
		}
		WARN_ON(want != 0 && want != 3 && want != 6);
		break;
	case IEEE80211_NUM_BANDS:
		WARN_ON(1);
		break;
	}
}

void ieee80211_set_bitrate_flags(struct wiphy *wiphy)
{
	enum ieee80211_band band;

	for (band = 0; band < IEEE80211_NUM_BANDS; band++)
		if (wiphy->bands[band])
			set_mandatory_flags_band(wiphy->bands[band], band);
}

int cfg80211_validate_key_settings(struct key_params *params, int key_idx,
				   const u8 *mac_addr)
{
	if (key_idx > 5)
		return -EINVAL;

	/*
	 * Disallow pairwise keys with non-zero index unless it's WEP
	 * (because current deployments use pairwise WEP keys with
	 * non-zero indizes but 802.11i clearly specifies to use zero)
	 */
	if (mac_addr && key_idx &&
	    params->cipher != WLAN_CIPHER_SUITE_WEP40 &&
	    params->cipher != WLAN_CIPHER_SUITE_WEP104)
		return -EINVAL;

	/* TODO: add definitions for the lengths to linux/ieee80211.h */
	switch (params->cipher) {
	case WLAN_CIPHER_SUITE_WEP40:
		if (params->key_len != 5)
			return -EINVAL;
		break;
	case WLAN_CIPHER_SUITE_TKIP:
		if (params->key_len != 32)
			return -EINVAL;
		break;
	case WLAN_CIPHER_SUITE_CCMP:
		if (params->key_len != 16)
			return -EINVAL;
		break;
	case WLAN_CIPHER_SUITE_WEP104:
		if (params->key_len != 13)
			return -EINVAL;
		break;
	case WLAN_CIPHER_SUITE_AES_CMAC:
		if (params->key_len != 16)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}
