/*
 *  Packet RX/TX history data structures and routines for TFRC-based protocols.
 *
 *  Copyright (c) 2007   The University of Aberdeen, Scotland, UK
 *  Copyright (c) 2005-6 The University of Waikato, Hamilton, New Zealand.
 *
 *  This code has been developed by the University of Waikato WAND
 *  research group. For further information please see http://www.wand.net.nz/
 *  or e-mail Ian McDonald - ian.mcdonald@jandi.co.nz
 *
 *  This code also uses code from Lulea University, rereleased as GPL by its
 *  authors:
 *  Copyright (c) 2003 Nils-Erik Mattsson, Joacim Haggmark, Magnus Erixzon
 *
 *  Changes to meet Linux coding standards, to make it meet latest ccid3 draft
 *  and to make it work as a loadable module in the DCCP stack written by
 *  Arnaldo Carvalho de Melo <acme@conectiva.com.br>.
 *
 *  Copyright (c) 2005 Arnaldo Carvalho de Melo <acme@conectiva.com.br>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef _DCCP_PKT_HIST_
#define _DCCP_PKT_HIST_

#include <linux/ktime.h>
#include <linux/types.h>

struct sk_buff;

struct tfrc_tx_hist_entry;

extern int  tfrc_tx_hist_add(struct tfrc_tx_hist_entry **headp, u64 seqno);
extern void tfrc_tx_hist_purge(struct tfrc_tx_hist_entry **headp);
extern u32  tfrc_tx_hist_rtt(struct tfrc_tx_hist_entry *head,
			     const u64 seqno, const ktime_t now);

/* Subtraction a-b modulo-16, respects circular wrap-around */
#define SUB16(a, b) (((a) + 16 - (b)) & 0xF)

/* Number of packets to wait after a missing packet (RFC 4342, 6.1) */
#define TFRC_NDUPACK 3

/**
 * tfrc_rx_hist_entry - Store information about a single received packet
 * @tfrchrx_seqno:	DCCP packet sequence number
 * @tfrchrx_ccval:	window counter value of packet (RFC 4342, 8.1)
 * @tfrchrx_ndp:	the NDP count (if any) of the packet
 * @tfrchrx_tstamp:	actual receive time of packet
 */
struct tfrc_rx_hist_entry {
	u64		 tfrchrx_seqno:48,
			 tfrchrx_ccval:4,
			 tfrchrx_type:4;
	u32		 tfrchrx_ndp; /* In fact it is from 8 to 24 bits */
	ktime_t		 tfrchrx_tstamp;
};

/**
 * tfrc_rx_hist  -  RX history structure for TFRC-based protocols
 *
 * @ring:		Packet history for RTT sampling and loss detection
 * @loss_count:		Number of entries in circular history
 * @loss_start:		Movable index (for loss detection)
 * @rtt_sample_prev:	Used during RTT sampling, points to candidate entry
 */
struct tfrc_rx_hist {
	struct tfrc_rx_hist_entry *ring[TFRC_NDUPACK + 1];
	u8			  loss_count:2,
				  loss_start:2;
#define rtt_sample_prev		  loss_start
};

extern void tfrc_rx_hist_add_packet(struct tfrc_rx_hist *h,
				    const struct sk_buff *skb, const u32 ndp);

extern int tfrc_rx_hist_duplicate(struct tfrc_rx_hist *h, struct sk_buff *skb);
extern int tfrc_rx_hist_new_loss_indicated(struct tfrc_rx_hist *h,
					   const struct sk_buff *skb, u32 ndp);
extern u32 tfrc_rx_hist_sample_rtt(struct tfrc_rx_hist *h,
				   const struct sk_buff *skb);
extern int tfrc_rx_hist_alloc(struct tfrc_rx_hist *h);
extern void tfrc_rx_hist_purge(struct tfrc_rx_hist *h);

#endif /* _DCCP_PKT_HIST_ */
