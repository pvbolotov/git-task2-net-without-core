/*
 *  net/dccp/options.c
 *
 *  An implementation of the DCCP protocol
 *  Copyright (c) 2005 Aristeu Sergio Rozanski Filho <aris@cathedrallabs.org>
 *  Copyright (c) 2005 Arnaldo Carvalho de Melo <acme@ghostprotocols.net>
 *  Copyright (c) 2005 Ian McDonald <iam4@cs.waikato.ac.nz>
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */
#include <linux/config.h>
#include <linux/dccp.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/skbuff.h>

#include "ackvec.h"
#include "ccid.h"
#include "dccp.h"
#include "feat.h"

int dccp_feat_default_sequence_window = DCCPF_INITIAL_SEQUENCE_WINDOW;
int dccp_feat_default_rx_ccid	      = DCCPF_INITIAL_CCID;
int dccp_feat_default_tx_ccid	      = DCCPF_INITIAL_CCID;
int dccp_feat_default_ack_ratio	      = DCCPF_INITIAL_ACK_RATIO;
int dccp_feat_default_send_ack_vector = DCCPF_INITIAL_SEND_ACK_VECTOR;
int dccp_feat_default_send_ndp_count  = DCCPF_INITIAL_SEND_NDP_COUNT;

void dccp_options_init(struct dccp_options *dccpo)
{
	dccpo->dccpo_sequence_window = dccp_feat_default_sequence_window;
	dccpo->dccpo_rx_ccid	     = dccp_feat_default_rx_ccid;
	dccpo->dccpo_tx_ccid	     = dccp_feat_default_tx_ccid;
	dccpo->dccpo_ack_ratio	     = dccp_feat_default_ack_ratio;
	dccpo->dccpo_send_ack_vector = dccp_feat_default_send_ack_vector;
	dccpo->dccpo_send_ndp_count  = dccp_feat_default_send_ndp_count;
}

static u32 dccp_decode_value_var(const unsigned char *bf, const u8 len)
{
	u32 value = 0;

	if (len > 3)
		value += *bf++ << 24;
	if (len > 2)
		value += *bf++ << 16;
	if (len > 1)
		value += *bf++ << 8;
	if (len > 0)
		value += *bf;

	return value;
}

int dccp_parse_options(struct sock *sk, struct sk_buff *skb)
{
	struct dccp_sock *dp = dccp_sk(sk);
#ifdef CONFIG_IP_DCCP_DEBUG
	const char *debug_prefix = dp->dccps_role == DCCP_ROLE_CLIENT ?
					"CLIENT rx opt: " : "server rx opt: ";
#endif
	const struct dccp_hdr *dh = dccp_hdr(skb);
	const u8 pkt_type = DCCP_SKB_CB(skb)->dccpd_type;
	unsigned char *options = (unsigned char *)dh + dccp_hdr_len(skb);
	unsigned char *opt_ptr = options;
	const unsigned char *opt_end = (unsigned char *)dh +
					(dh->dccph_doff * 4);
	struct dccp_options_received *opt_recv = &dp->dccps_options_received;
	unsigned char opt, len;
	unsigned char *value;
	u32 elapsed_time;
	int rc;
	int mandatory = 0;

	memset(opt_recv, 0, sizeof(*opt_recv));

	while (opt_ptr != opt_end) {
		opt   = *opt_ptr++;
		len   = 0;
		value = NULL;

		/* Check if this isn't a single byte option */
		if (opt > DCCPO_MAX_RESERVED) {
			if (opt_ptr == opt_end)
				goto out_invalid_option;

			len = *opt_ptr++;
			if (len < 3)
				goto out_invalid_option;
			/*
			 * Remove the type and len fields, leaving
			 * just the value size
			 */
			len	-= 2;
			value	= opt_ptr;
			opt_ptr += len;

			if (opt_ptr > opt_end)
				goto out_invalid_option;
		}

		switch (opt) {
		case DCCPO_PADDING:
			break;
		case DCCPO_MANDATORY:
			if (mandatory)
				goto out_invalid_option;
			if (pkt_type != DCCP_PKT_DATA)
				mandatory = 1;
			break;
		case DCCPO_NDP_COUNT:
			if (len > 3)
				goto out_invalid_option;

			opt_recv->dccpor_ndp = dccp_decode_value_var(value, len);
			dccp_pr_debug("%sNDP count=%d\n", debug_prefix,
				      opt_recv->dccpor_ndp);
			break;
		case DCCPO_CHANGE_L:
			/* fall through */
		case DCCPO_CHANGE_R:
			if (len < 2)
				goto out_invalid_option;
			rc = dccp_feat_change_recv(sk, opt, *value, value + 1,
						   len - 1);
			/*
			 * When there is a change error, change_recv is
			 * responsible for dealing with it.  i.e. reply with an
			 * empty confirm.
			 * If the change was mandatory, then we need to die.
			 */
			if (rc && mandatory)
				goto out_invalid_option;
			break;
		case DCCPO_CONFIRM_L:
			/* fall through */
		case DCCPO_CONFIRM_R:
			if (len < 2)
				goto out_invalid_option;
			if (dccp_feat_confirm_recv(sk, opt, *value,
						   value + 1, len - 1))
				goto out_invalid_option;
			break;
		case DCCPO_ACK_VECTOR_0:
		case DCCPO_ACK_VECTOR_1:
			if (pkt_type == DCCP_PKT_DATA)
				continue;

			if (dp->dccps_options.dccpo_send_ack_vector &&
			    dccp_ackvec_parse(sk, skb, opt, value, len))
				goto out_invalid_option;
			break;
		case DCCPO_TIMESTAMP:
			if (len != 4)
				goto out_invalid_option;

			opt_recv->dccpor_timestamp = ntohl(*(__be32 *)value);

			dp->dccps_timestamp_echo = opt_recv->dccpor_timestamp;
			dccp_timestamp(sk, &dp->dccps_timestamp_time);

			dccp_pr_debug("%sTIMESTAMP=%u, ackno=%llu\n",
				      debug_prefix, opt_recv->dccpor_timestamp,
				      (unsigned long long)
				      DCCP_SKB_CB(skb)->dccpd_ack_seq);
			break;
		case DCCPO_TIMESTAMP_ECHO:
			if (len != 4 && len != 6 && len != 8)
				goto out_invalid_option;

			opt_recv->dccpor_timestamp_echo = ntohl(*(__be32 *)value);

			dccp_pr_debug("%sTIMESTAMP_ECHO=%u, len=%d, ackno=%llu, ",
				      debug_prefix,
				      opt_recv->dccpor_timestamp_echo,
				      len + 2,
				      (unsigned long long)
				      DCCP_SKB_CB(skb)->dccpd_ack_seq);


			if (len == 4)
				break;

			if (len == 6)
				elapsed_time = ntohs(*(__be16 *)(value + 4));
			else
				elapsed_time = ntohl(*(__be32 *)(value + 4));

			/* Give precedence to the biggest ELAPSED_TIME */
			if (elapsed_time > opt_recv->dccpor_elapsed_time)
				opt_recv->dccpor_elapsed_time = elapsed_time;
			break;
		case DCCPO_ELAPSED_TIME:
			if (len != 2 && len != 4)
				goto out_invalid_option;

			if (pkt_type == DCCP_PKT_DATA)
				continue;

			if (len == 2)
				elapsed_time = ntohs(*(__be16 *)value);
			else
				elapsed_time = ntohl(*(__be32 *)value);

			if (elapsed_time > opt_recv->dccpor_elapsed_time)
				opt_recv->dccpor_elapsed_time = elapsed_time;

			dccp_pr_debug("%sELAPSED_TIME=%d\n", debug_prefix,
				      elapsed_time);
			break;
			/*
			 * From draft-ietf-dccp-spec-11.txt:
			 *
			 *	Option numbers 128 through 191 are for
			 *	options sent from the HC-Sender to the
			 *	HC-Receiver; option numbers 192 through 255
			 *	are for options sent from the HC-Receiver to
			 *	the HC-Sender.
			 */
		case 128 ... 191: {
			const u16 idx = value - options;

			if (ccid_hc_rx_parse_options(dp->dccps_hc_rx_ccid, sk,
						     opt, len, idx,
						     value) != 0)
				goto out_invalid_option;
		}
			break;
		case 192 ... 255: {
			const u16 idx = value - options;

			if (ccid_hc_tx_parse_options(dp->dccps_hc_tx_ccid, sk,
						     opt, len, idx,
						     value) != 0)
				goto out_invalid_option;
		}
			break;
		default:
			pr_info("DCCP(%p): option %d(len=%d) not "
				"implemented, ignoring\n",
				sk, opt, len);
			break;
	        }

		if (opt != DCCPO_MANDATORY)
			mandatory = 0;
	}

	/* mandatory was the last byte in option list -> reset connection */
	if (mandatory)
		goto out_invalid_option;

	return 0;

out_invalid_option:
	DCCP_INC_STATS_BH(DCCP_MIB_INVALIDOPT);
	DCCP_SKB_CB(skb)->dccpd_reset_code = DCCP_RESET_CODE_OPTION_ERROR;
	pr_info("DCCP(%p): invalid option %d, len=%d\n", sk, opt, len);
	return -1;
}

EXPORT_SYMBOL_GPL(dccp_parse_options);

static void dccp_encode_value_var(const u32 value, unsigned char *to,
				  const unsigned int len)
{
	if (len > 3)
		*to++ = (value & 0xFF000000) >> 24;
	if (len > 2)
		*to++ = (value & 0xFF0000) >> 16;
	if (len > 1)
		*to++ = (value & 0xFF00) >> 8;
	if (len > 0)
		*to++ = (value & 0xFF);
}

static inline int dccp_ndp_len(const int ndp)
{
	return likely(ndp <= 0xFF) ? 1 : ndp <= 0xFFFF ? 2 : 3;
}

void dccp_insert_option(struct sock *sk, struct sk_buff *skb,
			const unsigned char option,
			const void *value, const unsigned char len)
{
	unsigned char *to;

	if (DCCP_SKB_CB(skb)->dccpd_opt_len + len + 2 > DCCP_MAX_OPT_LEN) {
		LIMIT_NETDEBUG(KERN_INFO "DCCP: packet too small to insert "
			       "%d option!\n", option);
		return;
	}

	DCCP_SKB_CB(skb)->dccpd_opt_len += len + 2;

	to    = skb_push(skb, len + 2);
	*to++ = option;
	*to++ = len + 2;

	memcpy(to, value, len);
}

EXPORT_SYMBOL_GPL(dccp_insert_option);

static void dccp_insert_option_ndp(struct sock *sk, struct sk_buff *skb)
{
	struct dccp_sock *dp = dccp_sk(sk);
	int ndp = dp->dccps_ndp_count;

	if (dccp_non_data_packet(skb))
		++dp->dccps_ndp_count;
	else
		dp->dccps_ndp_count = 0;

	if (ndp > 0) {
		unsigned char *ptr;
		const int ndp_len = dccp_ndp_len(ndp);
		const int len = ndp_len + 2;

		if (DCCP_SKB_CB(skb)->dccpd_opt_len + len > DCCP_MAX_OPT_LEN)
			return;

		DCCP_SKB_CB(skb)->dccpd_opt_len += len;

		ptr = skb_push(skb, len);
		*ptr++ = DCCPO_NDP_COUNT;
		*ptr++ = len;
		dccp_encode_value_var(ndp, ptr, ndp_len);
	}
}

static inline int dccp_elapsed_time_len(const u32 elapsed_time)
{
	return elapsed_time == 0 ? 0 : elapsed_time <= 0xFFFF ? 2 : 4;
}

void dccp_insert_option_elapsed_time(struct sock *sk,
				     struct sk_buff *skb,
				     u32 elapsed_time)
{
#ifdef CONFIG_IP_DCCP_DEBUG
	struct dccp_sock *dp = dccp_sk(sk);
	const char *debug_prefix = dp->dccps_role == DCCP_ROLE_CLIENT ?
					"CLIENT TX opt: " : "server TX opt: ";
#endif
	const int elapsed_time_len = dccp_elapsed_time_len(elapsed_time);
	const int len = 2 + elapsed_time_len;
	unsigned char *to;

	if (elapsed_time_len == 0)
		return;

	if (DCCP_SKB_CB(skb)->dccpd_opt_len + len > DCCP_MAX_OPT_LEN) {
		LIMIT_NETDEBUG(KERN_INFO "DCCP: packet too small to "
					 "insert elapsed time!\n");
		return;
	}

	DCCP_SKB_CB(skb)->dccpd_opt_len += len;

	to    = skb_push(skb, len);
	*to++ = DCCPO_ELAPSED_TIME;
	*to++ = len;

	if (elapsed_time_len == 2) {
		const __be16 var16 = htons((u16)elapsed_time);
		memcpy(to, &var16, 2);
	} else {
		const __be32 var32 = htonl(elapsed_time);
		memcpy(to, &var32, 4);
	}

	dccp_pr_debug("%sELAPSED_TIME=%u, len=%d, seqno=%llu\n",
		      debug_prefix, elapsed_time,
		      len,
		      (unsigned long long) DCCP_SKB_CB(skb)->dccpd_seq);
}

EXPORT_SYMBOL_GPL(dccp_insert_option_elapsed_time);

void dccp_timestamp(const struct sock *sk, struct timeval *tv)
{
	const struct dccp_sock *dp = dccp_sk(sk);

	do_gettimeofday(tv);
	tv->tv_sec  -= dp->dccps_epoch.tv_sec;
	tv->tv_usec -= dp->dccps_epoch.tv_usec;

	while (tv->tv_usec < 0) {
		tv->tv_sec--;
		tv->tv_usec += USEC_PER_SEC;
	}
}

EXPORT_SYMBOL_GPL(dccp_timestamp);

void dccp_insert_option_timestamp(struct sock *sk, struct sk_buff *skb)
{
	struct timeval tv;
	__be32 now;

	dccp_timestamp(sk, &tv);
	now = htonl(timeval_usecs(&tv) / 10);
	/* yes this will overflow but that is the point as we want a
	 * 10 usec 32 bit timer which mean it wraps every 11.9 hours */

	dccp_insert_option(sk, skb, DCCPO_TIMESTAMP, &now, sizeof(now));
}

EXPORT_SYMBOL_GPL(dccp_insert_option_timestamp);

static void dccp_insert_option_timestamp_echo(struct sock *sk,
					      struct sk_buff *skb)
{
	struct dccp_sock *dp = dccp_sk(sk);
#ifdef CONFIG_IP_DCCP_DEBUG
	const char *debug_prefix = dp->dccps_role == DCCP_ROLE_CLIENT ?
					"CLIENT TX opt: " : "server TX opt: ";
#endif
	struct timeval now;
	__be32 tstamp_echo;
	u32 elapsed_time;
	int len, elapsed_time_len;
	unsigned char *to;

	dccp_timestamp(sk, &now);
	elapsed_time = timeval_delta(&now, &dp->dccps_timestamp_time) / 10;
	elapsed_time_len = dccp_elapsed_time_len(elapsed_time);
	len = 6 + elapsed_time_len;

	if (DCCP_SKB_CB(skb)->dccpd_opt_len + len > DCCP_MAX_OPT_LEN) {
		LIMIT_NETDEBUG(KERN_INFO "DCCP: packet too small to insert "
					 "timestamp echo!\n");
		return;
	}

	DCCP_SKB_CB(skb)->dccpd_opt_len += len;

	to    = skb_push(skb, len);
	*to++ = DCCPO_TIMESTAMP_ECHO;
	*to++ = len;

	tstamp_echo = htonl(dp->dccps_timestamp_echo);
	memcpy(to, &tstamp_echo, 4);
	to += 4;

	if (elapsed_time_len == 2) {
		const __be16 var16 = htons((u16)elapsed_time);
		memcpy(to, &var16, 2);
	} else if (elapsed_time_len == 4) {
		const __be32 var32 = htonl(elapsed_time);
		memcpy(to, &var32, 4);
	}

	dccp_pr_debug("%sTIMESTAMP_ECHO=%u, len=%d, seqno=%llu\n",
		      debug_prefix, dp->dccps_timestamp_echo,
		      len,
		      (unsigned long long) DCCP_SKB_CB(skb)->dccpd_seq);

	dp->dccps_timestamp_echo = 0;
	dp->dccps_timestamp_time.tv_sec = 0;
	dp->dccps_timestamp_time.tv_usec = 0;
}

static int dccp_insert_feat_opt(struct sk_buff *skb, u8 type, u8 feat,
			        u8 *val, u8 len)
{
	u8 *to;

	if (DCCP_SKB_CB(skb)->dccpd_opt_len + len + 3 > DCCP_MAX_OPT_LEN) {
		LIMIT_NETDEBUG(KERN_INFO "DCCP: packet too small"
			       " to insert feature %d option!\n", feat);
		return -1;
	}

	DCCP_SKB_CB(skb)->dccpd_opt_len += len + 3;

	to    = skb_push(skb, len + 3);
	*to++ = type;
	*to++ = len + 3;
	*to++ = feat;

	if (len)
		memcpy(to, val, len);
	dccp_pr_debug("option %d feat %d len %d\n", type, feat, len);

	return 0;
}

static void dccp_insert_feat(struct sock *sk, struct sk_buff *skb)
{
	struct dccp_sock *dp = dccp_sk(sk);
	struct dccp_opt_pend *opt, *next;
	int change = 0;

	/* confirm any options [NN opts] */
	list_for_each_entry_safe(opt, next, &dp->dccps_options.dccpo_conf,
				 dccpop_node) {
		dccp_insert_feat_opt(skb, opt->dccpop_type,
				     opt->dccpop_feat, opt->dccpop_val,
				     opt->dccpop_len);
		/* fear empty confirms */
		if (opt->dccpop_val)
			kfree(opt->dccpop_val);
		kfree(opt);
	}
	INIT_LIST_HEAD(&dp->dccps_options.dccpo_conf);

	/* see which features we need to send */
	list_for_each_entry(opt, &dp->dccps_options.dccpo_pending,
			    dccpop_node) {
		/* see if we need to send any confirm */
		if (opt->dccpop_sc) {
			dccp_insert_feat_opt(skb, opt->dccpop_type + 1,
					     opt->dccpop_feat,
					     opt->dccpop_sc->dccpoc_val,
					     opt->dccpop_sc->dccpoc_len);

			BUG_ON(!opt->dccpop_sc->dccpoc_val);
			kfree(opt->dccpop_sc->dccpoc_val);
			kfree(opt->dccpop_sc);
			opt->dccpop_sc = NULL;
		}

		/* any option not confirmed, re-send it */
		if (!opt->dccpop_conf) {
			dccp_insert_feat_opt(skb, opt->dccpop_type,
					     opt->dccpop_feat, opt->dccpop_val,
					     opt->dccpop_len);
			change++;
		}
	}

	/* Retransmit timer.
	 * If this is the master listening sock, we don't set a timer on it.  It
	 * should be fine because if the dude doesn't receive our RESPONSE
	 * [which will contain the CHANGE] he will send another REQUEST which
	 * will "retrnasmit" the change.
	 */
	if (change && dp->dccps_role != DCCP_ROLE_LISTEN) {
		dccp_pr_debug("reset feat negotiation timer %p\n", sk);

		/* XXX don't reset the timer on re-transmissions.  I.e. reset it
		 * only when sending new stuff i guess.  Currently the timer
		 * never backs off because on re-transmission it just resets it!
		 */
		inet_csk_reset_xmit_timer(sk, ICSK_TIME_RETRANS,
					  inet_csk(sk)->icsk_rto, DCCP_RTO_MAX);
	}
}

void dccp_insert_options(struct sock *sk, struct sk_buff *skb)
{
	struct dccp_sock *dp = dccp_sk(sk);

	DCCP_SKB_CB(skb)->dccpd_opt_len = 0;

	if (dp->dccps_options.dccpo_send_ndp_count)
		dccp_insert_option_ndp(sk, skb);

	if (!dccp_packet_without_ack(skb)) {
		if (dp->dccps_options.dccpo_send_ack_vector &&
		    dccp_ackvec_pending(dp->dccps_hc_rx_ackvec))
			dccp_insert_option_ackvec(sk, skb);
		if (dp->dccps_timestamp_echo != 0)
			dccp_insert_option_timestamp_echo(sk, skb);
	}

	if (dp->dccps_hc_rx_insert_options) {
		ccid_hc_rx_insert_options(dp->dccps_hc_rx_ccid, sk, skb);
		dp->dccps_hc_rx_insert_options = 0;
	}
	if (dp->dccps_hc_tx_insert_options) {
		ccid_hc_tx_insert_options(dp->dccps_hc_tx_ccid, sk, skb);
		dp->dccps_hc_tx_insert_options = 0;
	}

	/* Feature negotiation */
	switch(DCCP_SKB_CB(skb)->dccpd_type) {
		/* Data packets can't do feat negotiation */
	case DCCP_PKT_DATA:
	case DCCP_PKT_DATAACK:
		break;
	default:
		dccp_insert_feat(sk, skb);
		break;
	}

	/* XXX: insert other options when appropriate */

	if (DCCP_SKB_CB(skb)->dccpd_opt_len != 0) {
		/* The length of all options has to be a multiple of 4 */
		int padding = DCCP_SKB_CB(skb)->dccpd_opt_len % 4;

		if (padding != 0) {
			padding = 4 - padding;
			memset(skb_push(skb, padding), 0, padding);
			DCCP_SKB_CB(skb)->dccpd_opt_len += padding;
		}
	}
}
