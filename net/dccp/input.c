/*
 *  net/dccp/input.c
 * 
 *  An implementation of the DCCP protocol
 *  Arnaldo Carvalho de Melo <acme@conectiva.com.br>
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 */

#include <linux/config.h>
#include <linux/dccp.h>
#include <linux/skbuff.h>

#include <net/sock.h>

#include "ccid.h"
#include "dccp.h"

static void dccp_fin(struct sock *sk, struct sk_buff *skb)
{
	sk->sk_shutdown |= RCV_SHUTDOWN;
	sock_set_flag(sk, SOCK_DONE);
	__skb_pull(skb, dccp_hdr(skb)->dccph_doff * 4);
	__skb_queue_tail(&sk->sk_receive_queue, skb);
	skb_set_owner_r(skb, sk);
	sk->sk_data_ready(sk, 0);
}

static void dccp_rcv_close(struct sock *sk, struct sk_buff *skb)
{
	switch (sk->sk_state) {
	case DCCP_PARTOPEN:
	case DCCP_OPEN:
		dccp_v4_send_reset(sk, DCCP_RESET_CODE_CLOSED);
		dccp_fin(sk, skb);
		dccp_set_state(sk, DCCP_CLOSED);
		break;
	}
}

static void dccp_rcv_closereq(struct sock *sk, struct sk_buff *skb)
{
	/*
	 *   Step 7: Check for unexpected packet types
	 *      If (S.is_server and P.type == CloseReq)
	 *	  Send Sync packet acknowledging P.seqno
	 *	  Drop packet and return
	 */
	if (dccp_sk(sk)->dccps_role != DCCP_ROLE_CLIENT) {
		dccp_send_sync(sk, DCCP_SKB_CB(skb)->dccpd_seq);
		return;
	}

	switch (sk->sk_state) {
	case DCCP_PARTOPEN:
	case DCCP_OPEN:
		dccp_set_state(sk, DCCP_CLOSING);
		dccp_send_close(sk);
		break;
	}
}

static inline void dccp_event_ack_recv(struct sock *sk, struct sk_buff *skb)
{
	struct dccp_sock *dp = dccp_sk(sk);

	if (dp->dccps_options.dccpo_send_ack_vector)
		dccp_ackpkts_check_rcv_ackno(dp->dccps_hc_rx_ackpkts, sk,
					     DCCP_SKB_CB(skb)->dccpd_ack_seq);
}

static int dccp_check_seqno(struct sock *sk, struct sk_buff *skb)
{
	const struct dccp_hdr *dh = dccp_hdr(skb);
	struct dccp_sock *dp = dccp_sk(sk);
	u64 lswl = dp->dccps_swl;
	u64 lawl = dp->dccps_awl;

	/*
	 *   Step 5: Prepare sequence numbers for Sync
	 *     If P.type == Sync or P.type == SyncAck,
	 *	  If S.AWL <= P.ackno <= S.AWH and P.seqno >= S.SWL,
	 *	     / * P is valid, so update sequence number variables
	 *		 accordingly.  After this update, P will pass the tests
	 *		 in Step 6.  A SyncAck is generated if necessary in
	 *		 Step 15 * /
	 *	     Update S.GSR, S.SWL, S.SWH
	 *	  Otherwise,
	 *	     Drop packet and return
	 */
	if (dh->dccph_type == DCCP_PKT_SYNC || 
	    dh->dccph_type == DCCP_PKT_SYNCACK) {
		if (between48(DCCP_SKB_CB(skb)->dccpd_ack_seq,
			      dp->dccps_awl, dp->dccps_awh) &&
		    !before48(DCCP_SKB_CB(skb)->dccpd_seq, dp->dccps_swl))
			dccp_update_gsr(sk, DCCP_SKB_CB(skb)->dccpd_seq);
		else
			return -1;
	/*
	 *   Step 6: Check sequence numbers
	 *      Let LSWL = S.SWL and LAWL = S.AWL
	 *      If P.type == CloseReq or P.type == Close or P.type == Reset,
	 *	  LSWL := S.GSR + 1, LAWL := S.GAR
	 *      If LSWL <= P.seqno <= S.SWH
	 *	     and (P.ackno does not exist or LAWL <= P.ackno <= S.AWH),
	 *	  Update S.GSR, S.SWL, S.SWH
	 *	  If P.type != Sync,
	 *	     Update S.GAR
	 *      Otherwise,
	 *	  Send Sync packet acknowledging P.seqno
	 *	  Drop packet and return
	 */
	} else if (dh->dccph_type == DCCP_PKT_CLOSEREQ ||
		   dh->dccph_type == DCCP_PKT_CLOSE ||
		   dh->dccph_type == DCCP_PKT_RESET) {
		lswl = dp->dccps_gsr;
		dccp_inc_seqno(&lswl);
		lawl = dp->dccps_gar;
	}

	if (between48(DCCP_SKB_CB(skb)->dccpd_seq, lswl, dp->dccps_swh) &&
	    (DCCP_SKB_CB(skb)->dccpd_ack_seq == DCCP_PKT_WITHOUT_ACK_SEQ ||
	     between48(DCCP_SKB_CB(skb)->dccpd_ack_seq,
		       lawl, dp->dccps_awh))) {
		dccp_update_gsr(sk, DCCP_SKB_CB(skb)->dccpd_seq);

		if (dh->dccph_type != DCCP_PKT_SYNC &&
		    (DCCP_SKB_CB(skb)->dccpd_ack_seq !=
		     DCCP_PKT_WITHOUT_ACK_SEQ))
			dp->dccps_gar = DCCP_SKB_CB(skb)->dccpd_ack_seq;
	} else {
		dccp_pr_debug("Step 6 failed, sending SYNC...\n");
		dccp_send_sync(sk, DCCP_SKB_CB(skb)->dccpd_seq);
		return -1;
	}

	return 0;
}

int dccp_rcv_established(struct sock *sk, struct sk_buff *skb,
			 const struct dccp_hdr *dh, const unsigned len)
{
	struct dccp_sock *dp = dccp_sk(sk);

	if (dccp_check_seqno(sk, skb))
		goto discard;

	if (dccp_parse_options(sk, skb))
		goto discard;

	if (DCCP_SKB_CB(skb)->dccpd_ack_seq != DCCP_PKT_WITHOUT_ACK_SEQ)
		dccp_event_ack_recv(sk, skb);

	/*
	 * FIXME: check ECN to see if we should use
	 * DCCP_ACKPKTS_STATE_ECN_MARKED
	 */
	if (dp->dccps_options.dccpo_send_ack_vector) {
		struct dccp_ackpkts *ap = dp->dccps_hc_rx_ackpkts;

		if (dccp_ackpkts_add(dp->dccps_hc_rx_ackpkts,
				     DCCP_SKB_CB(skb)->dccpd_seq,
				     DCCP_ACKPKTS_STATE_RECEIVED)) {
			LIMIT_NETDEBUG(KERN_INFO "DCCP: acknowledgeable "
						 "packets buffer full!\n");
			ap->dccpap_ack_seqno = DCCP_MAX_SEQNO + 1;
			inet_csk_schedule_ack(sk);
			inet_csk_reset_xmit_timer(sk, ICSK_TIME_DACK,
						  TCP_DELACK_MIN,
						  DCCP_RTO_MAX);
			goto discard;
		}

		/*
		 * FIXME: this activation is probably wrong, have to study more
		 * TCP delack machinery and how it fits into DCCP draft, but
		 * for now it kinda "works" 8)
		 */
		if (!inet_csk_ack_scheduled(sk)) {
			inet_csk_schedule_ack(sk);
			inet_csk_reset_xmit_timer(sk, ICSK_TIME_DACK, 5 * HZ,
						  DCCP_RTO_MAX);
		}
	}

	ccid_hc_rx_packet_recv(dp->dccps_hc_rx_ccid, sk, skb);
	ccid_hc_tx_packet_recv(dp->dccps_hc_tx_ccid, sk, skb);

	switch (dccp_hdr(skb)->dccph_type) {
	case DCCP_PKT_DATAACK:
	case DCCP_PKT_DATA:
		/*
		 * FIXME: check if sk_receive_queue is full, schedule DATA_DROPPED
		 * option if it is.
		 */
		__skb_pull(skb, dh->dccph_doff * 4);
		__skb_queue_tail(&sk->sk_receive_queue, skb);
		skb_set_owner_r(skb, sk);
		sk->sk_data_ready(sk, 0);
		return 0;
	case DCCP_PKT_ACK:
		goto discard;
	case DCCP_PKT_RESET:
		/*
		 *  Step 9: Process Reset
		 *	If P.type == Reset,
		 *		Tear down connection
		 *		S.state := TIMEWAIT
		 *		Set TIMEWAIT timer
		 *		Drop packet and return
		*/
		dccp_fin(sk, skb);
		dccp_time_wait(sk, DCCP_TIME_WAIT, 0);
		return 0;
	case DCCP_PKT_CLOSEREQ:
		dccp_rcv_closereq(sk, skb);
		goto discard;
	case DCCP_PKT_CLOSE:
		dccp_rcv_close(sk, skb);
		return 0;
	case DCCP_PKT_REQUEST:
		/* Step 7 
            	 *   or (S.is_server and P.type == Response)
		 *   or (S.is_client and P.type == Request)
		 *   or (S.state >= OPEN and P.type == Request
		 *	and P.seqno >= S.OSR)
		 *    or (S.state >= OPEN and P.type == Response
		 *	and P.seqno >= S.OSR)
		 *    or (S.state == RESPOND and P.type == Data),
		 *  Send Sync packet acknowledging P.seqno
		 *  Drop packet and return
		 */
		if (dp->dccps_role != DCCP_ROLE_LISTEN)
			goto send_sync;
		goto check_seq;
	case DCCP_PKT_RESPONSE:
		if (dp->dccps_role != DCCP_ROLE_CLIENT)
			goto send_sync;
check_seq:
		if (!before48(DCCP_SKB_CB(skb)->dccpd_seq, dp->dccps_osr)) {
send_sync:
			dccp_send_sync(sk, DCCP_SKB_CB(skb)->dccpd_seq);
		}
		break;
	}

	DCCP_INC_STATS_BH(DCCP_MIB_INERRS);
discard:
	__kfree_skb(skb);
	return 0;
}

static int dccp_rcv_request_sent_state_process(struct sock *sk,
					       struct sk_buff *skb,
					       const struct dccp_hdr *dh,
					       const unsigned len)
{
	/* 
	 *  Step 4: Prepare sequence numbers in REQUEST
	 *     If S.state == REQUEST,
	 *	  If (P.type == Response or P.type == Reset)
	 *		and S.AWL <= P.ackno <= S.AWH,
	 *	     / * Set sequence number variables corresponding to the
	 *		other endpoint, so P will pass the tests in Step 6 * /
	 *	     Set S.GSR, S.ISR, S.SWL, S.SWH
	 *	     / * Response processing continues in Step 10; Reset
	 *		processing continues in Step 9 * /
	*/
	if (dh->dccph_type == DCCP_PKT_RESPONSE) {
		const struct inet_connection_sock *icsk = inet_csk(sk);
		struct dccp_sock *dp = dccp_sk(sk);

		/* Stop the REQUEST timer */
		inet_csk_clear_xmit_timer(sk, ICSK_TIME_RETRANS);
		BUG_TRAP(sk->sk_send_head != NULL);
		__kfree_skb(sk->sk_send_head);
		sk->sk_send_head = NULL;

		if (!between48(DCCP_SKB_CB(skb)->dccpd_ack_seq,
			       dp->dccps_awl, dp->dccps_awh)) {
			dccp_pr_debug("invalid ackno: S.AWL=%llu, "
				      "P.ackno=%llu, S.AWH=%llu \n",
				      (unsigned long long)dp->dccps_awl,
			   (unsigned long long)DCCP_SKB_CB(skb)->dccpd_ack_seq,
				      (unsigned long long)dp->dccps_awh);
			goto out_invalid_packet;
		}

		dp->dccps_isr = DCCP_SKB_CB(skb)->dccpd_seq;
		dccp_update_gsr(sk, DCCP_SKB_CB(skb)->dccpd_seq);

		if (ccid_hc_rx_init(dp->dccps_hc_rx_ccid, sk) != 0 ||
		    ccid_hc_tx_init(dp->dccps_hc_tx_ccid, sk) != 0) {
			ccid_hc_rx_exit(dp->dccps_hc_rx_ccid, sk);
			ccid_hc_tx_exit(dp->dccps_hc_tx_ccid, sk);
			/* FIXME: send appropriate RESET code */
			goto out_invalid_packet;
		}

		dccp_sync_mss(sk, dp->dccps_pmtu_cookie);

		/*
		 *    Step 10: Process REQUEST state (second part)
		 *       If S.state == REQUEST,
		 *	  / * If we get here, P is a valid Response from the
		 *	      server (see Step 4), and we should move to
		 *	      PARTOPEN state. PARTOPEN means send an Ack,
		 *	      don't send Data packets, retransmit Acks
		 *	      periodically, and always include any Init Cookie
		 *	      from the Response * /
		 *	  S.state := PARTOPEN
		 *	  Set PARTOPEN timer
		 * 	  Continue with S.state == PARTOPEN
		 *	  / * Step 12 will send the Ack completing the
		 *	      three-way handshake * /
		 */
		dccp_set_state(sk, DCCP_PARTOPEN);

		/* Make sure socket is routed, for correct metrics. */
		inet_sk_rebuild_header(sk);

		if (!sock_flag(sk, SOCK_DEAD)) {
			sk->sk_state_change(sk);
			sk_wake_async(sk, 0, POLL_OUT);
		}

		if (sk->sk_write_pending || icsk->icsk_ack.pingpong ||
		    icsk->icsk_accept_queue.rskq_defer_accept) {
			/* Save one ACK. Data will be ready after
			 * several ticks, if write_pending is set.
			 *
			 * It may be deleted, but with this feature tcpdumps
			 * look so _wonderfully_ clever, that I was not able
			 * to stand against the temptation 8)     --ANK
			 */
			/*
			 * OK, in DCCP we can as well do a similar trick, its
			 * even in the draft, but there is no need for us to
			 * schedule an ack here, as dccp_sendmsg does this for
			 * us, also stated in the draft. -acme
			 */
			__kfree_skb(skb);
			return 0;
		} 
		dccp_send_ack(sk);
		return -1;
	}

out_invalid_packet:
	return 1; /* dccp_v4_do_rcv will send a reset, but...
		     FIXME: the reset code should be
			    DCCP_RESET_CODE_PACKET_ERROR */
}

static int dccp_rcv_respond_partopen_state_process(struct sock *sk,
						   struct sk_buff *skb,
						   const struct dccp_hdr *dh,
						   const unsigned len)
{
	int queued = 0;

	switch (dh->dccph_type) {
	case DCCP_PKT_RESET:
		inet_csk_clear_xmit_timer(sk, ICSK_TIME_DACK);
		break;
	case DCCP_PKT_DATAACK:
	case DCCP_PKT_ACK:
		/*
		 * FIXME: we should be reseting the PARTOPEN (DELACK) timer
		 * here but only if we haven't used the DELACK timer for
		 * something else, like sending a delayed ack for a TIMESTAMP
		 * echo, etc, for now were not clearing it, sending an extra
		 * ACK when there is nothing else to do in DELACK is not a big
		 * deal after all.
		 */

		/* Stop the PARTOPEN timer */
		if (sk->sk_state == DCCP_PARTOPEN)
			inet_csk_clear_xmit_timer(sk, ICSK_TIME_DACK);

		dccp_sk(sk)->dccps_osr = DCCP_SKB_CB(skb)->dccpd_seq;
		dccp_set_state(sk, DCCP_OPEN);

		if (dh->dccph_type == DCCP_PKT_DATAACK) {
			dccp_rcv_established(sk, skb, dh, len);
			queued = 1; /* packet was queued
				       (by dccp_rcv_established) */
		}
		break;
	}

	return queued;
}

int dccp_rcv_state_process(struct sock *sk, struct sk_buff *skb,
			   struct dccp_hdr *dh, unsigned len)
{
	struct dccp_sock *dp = dccp_sk(sk);
	const int old_state = sk->sk_state;
	int queued = 0;

	/*
	 *  Step 3: Process LISTEN state
	 *  	(Continuing from dccp_v4_do_rcv and dccp_v6_do_rcv)
	 *
	 *     If S.state == LISTEN,
	 *	  If P.type == Request or P contains a valid Init Cookie
	 *	  	option,
	 *	     * Must scan the packet's options to check for an Init
	 *		Cookie.  Only the Init Cookie is processed here,
	 *		however; other options are processed in Step 8.  This
	 *		scan need only be performed if the endpoint uses Init
	 *		Cookies *
	 *	     * Generate a new socket and switch to that socket *
	 *	     Set S := new socket for this port pair
	 *	     S.state = RESPOND
	 *	     Choose S.ISS (initial seqno) or set from Init Cookie
	 *	     Set S.ISR, S.GSR, S.SWL, S.SWH from packet or Init Cookie
	 *	     Continue with S.state == RESPOND
	 *	     * A Response packet will be generated in Step 11 *
	 *	  Otherwise,
	 *	     Generate Reset(No Connection) unless P.type == Reset
	 *	     Drop packet and return
	 *
	 * NOTE: the check for the packet types is done in
	 *	 dccp_rcv_state_process
	 */
	if (sk->sk_state == DCCP_LISTEN) {
		if (dh->dccph_type == DCCP_PKT_REQUEST) {
			if (dccp_v4_conn_request(sk, skb) < 0)
				return 1;

			/* FIXME: do congestion control initialization */
			goto discard;
		}
		if (dh->dccph_type == DCCP_PKT_RESET)
			goto discard;

		/* Caller (dccp_v4_do_rcv) will send Reset(No Connection)*/
		return 1;
	}

	if (sk->sk_state != DCCP_REQUESTING) {
		if (dccp_check_seqno(sk, skb))
			goto discard;

		/*
		 * Step 8: Process options and mark acknowledgeable
		 */
		if (dccp_parse_options(sk, skb))
			goto discard;

		if (DCCP_SKB_CB(skb)->dccpd_ack_seq !=
		    DCCP_PKT_WITHOUT_ACK_SEQ)
			dccp_event_ack_recv(sk, skb);

		ccid_hc_rx_packet_recv(dp->dccps_hc_rx_ccid, sk, skb);
		ccid_hc_tx_packet_recv(dp->dccps_hc_tx_ccid, sk, skb);

		/*
		 * FIXME: check ECN to see if we should use
		 * DCCP_ACKPKTS_STATE_ECN_MARKED
		 */
		if (dp->dccps_options.dccpo_send_ack_vector) {
			if (dccp_ackpkts_add(dp->dccps_hc_rx_ackpkts,
					     DCCP_SKB_CB(skb)->dccpd_seq,
					     DCCP_ACKPKTS_STATE_RECEIVED))
				goto discard;
			/*
			 * FIXME: this activation is probably wrong, have to
			 * study more TCP delack machinery and how it fits into
			 * DCCP draft, but for now it kinda "works" 8)
			 */
			if ((dp->dccps_hc_rx_ackpkts->dccpap_ack_seqno ==
			     DCCP_MAX_SEQNO + 1) &&
			    !inet_csk_ack_scheduled(sk)) {
				inet_csk_schedule_ack(sk);
				inet_csk_reset_xmit_timer(sk, ICSK_TIME_DACK,
							  TCP_DELACK_MIN,
							  DCCP_RTO_MAX);
			}
		}
	}

	/*
	 *  Step 9: Process Reset
	 *	If P.type == Reset,
	 *		Tear down connection
	 *		S.state := TIMEWAIT
	 *		Set TIMEWAIT timer
	 *		Drop packet and return
	*/
	if (dh->dccph_type == DCCP_PKT_RESET) {
		/*
		 * Queue the equivalent of TCP fin so that dccp_recvmsg
		 * exits the loop
		 */
		dccp_fin(sk, skb);
		dccp_time_wait(sk, DCCP_TIME_WAIT, 0);
		return 0;
		/*
		 *   Step 7: Check for unexpected packet types
		 *      If (S.is_server and P.type == CloseReq)
		 *	    or (S.is_server and P.type == Response)
		 *	    or (S.is_client and P.type == Request)
		 *	    or (S.state == RESPOND and P.type == Data),
		 *	  Send Sync packet acknowledging P.seqno
		 *	  Drop packet and return
		 */
	} else if ((dp->dccps_role != DCCP_ROLE_CLIENT &&
		    (dh->dccph_type == DCCP_PKT_RESPONSE ||
		     dh->dccph_type == DCCP_PKT_CLOSEREQ)) ||
		    (dp->dccps_role == DCCP_ROLE_CLIENT &&
		     dh->dccph_type == DCCP_PKT_REQUEST) ||
		    (sk->sk_state == DCCP_RESPOND &&
		     dh->dccph_type == DCCP_PKT_DATA)) {
		dccp_send_sync(sk, DCCP_SKB_CB(skb)->dccpd_seq);
		goto discard;
	}

	switch (sk->sk_state) {
	case DCCP_CLOSED:
		return 1;

	case DCCP_REQUESTING:
		/* FIXME: do congestion control initialization */

		queued = dccp_rcv_request_sent_state_process(sk, skb, dh, len);
		if (queued >= 0)
			return queued;

		__kfree_skb(skb);
		return 0;

	case DCCP_RESPOND:
	case DCCP_PARTOPEN:
		queued = dccp_rcv_respond_partopen_state_process(sk, skb,
								 dh, len);
		break;
	}

	if (dh->dccph_type == DCCP_PKT_ACK ||
	    dh->dccph_type == DCCP_PKT_DATAACK) {
		switch (old_state) {
		case DCCP_PARTOPEN:
			sk->sk_state_change(sk);
			sk_wake_async(sk, 0, POLL_OUT);
			break;
		}
	}

	if (!queued) { 
discard:
		__kfree_skb(skb);
	}
	return 0;
}
