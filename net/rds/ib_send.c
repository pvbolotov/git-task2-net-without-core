/*
 * Copyright (c) 2006 Oracle.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */
#include <linux/kernel.h>
#include <linux/in.h>
#include <linux/device.h>
#include <linux/dmapool.h>

#include "rds.h"
#include "ib.h"

/*
 * Convert IB-specific error message to RDS error message and call core
 * completion handler.
 */
static void rds_ib_send_complete(struct rds_message *rm,
				 int wc_status,
				 void (*complete)(struct rds_message *rm, int status))
{
	int notify_status;

	switch (wc_status) {
	case IB_WC_WR_FLUSH_ERR:
		return;

	case IB_WC_SUCCESS:
		notify_status = RDS_RDMA_SUCCESS;
		break;

	case IB_WC_REM_ACCESS_ERR:
		notify_status = RDS_RDMA_REMOTE_ERROR;
		break;

	default:
		notify_status = RDS_RDMA_OTHER_ERROR;
		break;
	}
	complete(rm, notify_status);
}

static void rds_ib_send_unmap_rm(struct rds_ib_connection *ic,
			  struct rds_ib_send_work *send,
			  int wc_status)
{
	struct rds_message *rm = send->s_rm;

	rdsdebug("ic %p send %p rm %p\n", ic, send, rm);

	ib_dma_unmap_sg(ic->i_cm_id->device,
			rm->data.op_sg, rm->data.op_nents,
			DMA_TO_DEVICE);

	if (rm->rdma.op_active) {
		struct rm_rdma_op *op = &rm->rdma;

		if (op->op_mapped) {
			ib_dma_unmap_sg(ic->i_cm_id->device,
					op->op_sg, op->op_nents,
					op->op_write ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
			op->op_mapped = 0;
		}

		/* If the user asked for a completion notification on this
		 * message, we can implement three different semantics:
		 *  1.	Notify when we received the ACK on the RDS message
		 *	that was queued with the RDMA. This provides reliable
		 *	notification of RDMA status at the expense of a one-way
		 *	packet delay.
		 *  2.	Notify when the IB stack gives us the completion event for
		 *	the RDMA operation.
		 *  3.	Notify when the IB stack gives us the completion event for
		 *	the accompanying RDS messages.
		 * Here, we implement approach #3. To implement approach #2,
		 * call rds_rdma_send_complete from the cq_handler. To implement #1,
		 * don't call rds_rdma_send_complete at all, and fall back to the notify
		 * handling in the ACK processing code.
		 *
		 * Note: There's no need to explicitly sync any RDMA buffers using
		 * ib_dma_sync_sg_for_cpu - the completion for the RDMA
		 * operation itself unmapped the RDMA buffers, which takes care
		 * of synching.
		 */
		rds_ib_send_complete(rm, wc_status, rds_rdma_send_complete);

		if (rm->rdma.op_write)
			rds_stats_add(s_send_rdma_bytes, rm->rdma.op_bytes);
		else
			rds_stats_add(s_recv_rdma_bytes, rm->rdma.op_bytes);
	}

	if (rm->atomic.op_active) {
		struct rm_atomic_op *op = &rm->atomic;

		/* unmap atomic recvbuf */
		if (op->op_mapped) {
			ib_dma_unmap_sg(ic->i_cm_id->device, op->op_sg, 1,
					DMA_FROM_DEVICE);
			op->op_mapped = 0;
		}

		rds_ib_send_complete(rm, wc_status, rds_atomic_send_complete);

		if (rm->atomic.op_type == RDS_ATOMIC_TYPE_CSWP)
			rds_stats_inc(s_atomic_cswp);
		else
			rds_stats_inc(s_atomic_fadd);
	}

	/* If anyone waited for this message to get flushed out, wake
	 * them up now */
	rds_message_unmapped(rm);

	rds_message_put(rm);
	send->s_rm = NULL;
}

void rds_ib_send_init_ring(struct rds_ib_connection *ic)
{
	struct rds_ib_send_work *send;
	u32 i;

	for (i = 0, send = ic->i_sends; i < ic->i_send_ring.w_nr; i++, send++) {
		struct ib_sge *sge;

		send->s_rm = NULL;
		send->s_op = NULL;

		send->s_wr.wr_id = i;
		send->s_wr.sg_list = send->s_sge;
		send->s_wr.ex.imm_data = 0;

		sge = &send->s_sge[0];
		sge->addr = ic->i_send_hdrs_dma + (i * sizeof(struct rds_header));
		sge->length = sizeof(struct rds_header);
		sge->lkey = ic->i_mr->lkey;

		send->s_sge[1].lkey = ic->i_mr->lkey;
	}
}

void rds_ib_send_clear_ring(struct rds_ib_connection *ic)
{
	struct rds_ib_send_work *send;
	u32 i;

	for (i = 0, send = ic->i_sends; i < ic->i_send_ring.w_nr; i++, send++) {
		if (!send->s_rm || send->s_wr.opcode == 0xdead)
			continue;
		rds_ib_send_unmap_rm(ic, send, IB_WC_WR_FLUSH_ERR);
	}
}

/*
 * The _oldest/_free ring operations here race cleanly with the alloc/unalloc
 * operations performed in the send path.  As the sender allocs and potentially
 * unallocs the next free entry in the ring it doesn't alter which is
 * the next to be freed, which is what this is concerned with.
 */
void rds_ib_send_cq_comp_handler(struct ib_cq *cq, void *context)
{
	struct rds_connection *conn = context;
	struct rds_ib_connection *ic = conn->c_transport_data;
	struct ib_wc wc;
	struct rds_ib_send_work *send;
	u32 completed;
	u32 oldest;
	u32 i = 0;
	int ret;

	rdsdebug("cq %p conn %p\n", cq, conn);
	rds_ib_stats_inc(s_ib_tx_cq_call);
	ret = ib_req_notify_cq(cq, IB_CQ_NEXT_COMP);
	if (ret)
		rdsdebug("ib_req_notify_cq send failed: %d\n", ret);

	while (ib_poll_cq(cq, 1, &wc) > 0) {
		rdsdebug("wc wr_id 0x%llx status %u byte_len %u imm_data %u\n",
			 (unsigned long long)wc.wr_id, wc.status, wc.byte_len,
			 be32_to_cpu(wc.ex.imm_data));
		rds_ib_stats_inc(s_ib_tx_cq_event);

		if (wc.wr_id == RDS_IB_ACK_WR_ID) {
			if (ic->i_ack_queued + HZ/2 < jiffies)
				rds_ib_stats_inc(s_ib_tx_stalled);
			rds_ib_ack_send_complete(ic);
			continue;
		}

		oldest = rds_ib_ring_oldest(&ic->i_send_ring);

		completed = rds_ib_ring_completed(&ic->i_send_ring, wc.wr_id, oldest);

		for (i = 0; i < completed; i++) {
			send = &ic->i_sends[oldest];

			/* In the error case, wc.opcode sometimes contains garbage */
			switch (send->s_wr.opcode) {
			case IB_WR_SEND:
			case IB_WR_RDMA_WRITE:
			case IB_WR_RDMA_READ:
			case IB_WR_ATOMIC_FETCH_AND_ADD:
			case IB_WR_ATOMIC_CMP_AND_SWP:
				if (send->s_rm)
					rds_ib_send_unmap_rm(ic, send, wc.status);
				break;
			default:
				if (printk_ratelimit())
					printk(KERN_NOTICE
						"RDS/IB: %s: unexpected opcode 0x%x in WR!\n",
						__func__, send->s_wr.opcode);
				break;
			}

			send->s_wr.opcode = 0xdead;
			send->s_wr.num_sge = 1;
			if (send->s_queued + HZ/2 < jiffies)
				rds_ib_stats_inc(s_ib_tx_stalled);

			/* If a RDMA operation produced an error, signal this right
			 * away. If we don't, the subsequent SEND that goes with this
			 * RDMA will be canceled with ERR_WFLUSH, and the application
			 * never learn that the RDMA failed. */
			if (unlikely(wc.status == IB_WC_REM_ACCESS_ERR && send->s_op)) {
				struct rds_message *rm;

				rm = rds_send_get_message(conn, send->s_op);
				if (rm) {
					rds_ib_send_unmap_rm(ic, send, wc.status);
					rds_ib_send_complete(rm, wc.status, rds_rdma_send_complete);
					rds_message_put(rm);
				}
			}

			oldest = (oldest + 1) % ic->i_send_ring.w_nr;
		}

		rds_ib_ring_free(&ic->i_send_ring, completed);

		if (test_and_clear_bit(RDS_LL_SEND_FULL, &conn->c_flags) ||
		    test_bit(0, &conn->c_map_queued))
			queue_delayed_work(rds_wq, &conn->c_send_w, 0);

		/* We expect errors as the qp is drained during shutdown */
		if (wc.status != IB_WC_SUCCESS && rds_conn_up(conn)) {
			rds_ib_conn_error(conn,
				"send completion on %pI4 "
				"had status %u, disconnecting and reconnecting\n",
				&conn->c_faddr, wc.status);
		}
	}
}

/*
 * This is the main function for allocating credits when sending
 * messages.
 *
 * Conceptually, we have two counters:
 *  -	send credits: this tells us how many WRs we're allowed
 *	to submit without overruning the reciever's queue. For
 *	each SEND WR we post, we decrement this by one.
 *
 *  -	posted credits: this tells us how many WRs we recently
 *	posted to the receive queue. This value is transferred
 *	to the peer as a "credit update" in a RDS header field.
 *	Every time we transmit credits to the peer, we subtract
 *	the amount of transferred credits from this counter.
 *
 * It is essential that we avoid situations where both sides have
 * exhausted their send credits, and are unable to send new credits
 * to the peer. We achieve this by requiring that we send at least
 * one credit update to the peer before exhausting our credits.
 * When new credits arrive, we subtract one credit that is withheld
 * until we've posted new buffers and are ready to transmit these
 * credits (see rds_ib_send_add_credits below).
 *
 * The RDS send code is essentially single-threaded; rds_send_xmit
 * grabs c_send_lock to ensure exclusive access to the send ring.
 * However, the ACK sending code is independent and can race with
 * message SENDs.
 *
 * In the send path, we need to update the counters for send credits
 * and the counter of posted buffers atomically - when we use the
 * last available credit, we cannot allow another thread to race us
 * and grab the posted credits counter.  Hence, we have to use a
 * spinlock to protect the credit counter, or use atomics.
 *
 * Spinlocks shared between the send and the receive path are bad,
 * because they create unnecessary delays. An early implementation
 * using a spinlock showed a 5% degradation in throughput at some
 * loads.
 *
 * This implementation avoids spinlocks completely, putting both
 * counters into a single atomic, and updating that atomic using
 * atomic_add (in the receive path, when receiving fresh credits),
 * and using atomic_cmpxchg when updating the two counters.
 */
int rds_ib_send_grab_credits(struct rds_ib_connection *ic,
			     u32 wanted, u32 *adv_credits, int need_posted, int max_posted)
{
	unsigned int avail, posted, got = 0, advertise;
	long oldval, newval;

	*adv_credits = 0;
	if (!ic->i_flowctl)
		return wanted;

try_again:
	advertise = 0;
	oldval = newval = atomic_read(&ic->i_credits);
	posted = IB_GET_POST_CREDITS(oldval);
	avail = IB_GET_SEND_CREDITS(oldval);

	rdsdebug("rds_ib_send_grab_credits(%u): credits=%u posted=%u\n",
			wanted, avail, posted);

	/* The last credit must be used to send a credit update. */
	if (avail && !posted)
		avail--;

	if (avail < wanted) {
		struct rds_connection *conn = ic->i_cm_id->context;

		/* Oops, there aren't that many credits left! */
		set_bit(RDS_LL_SEND_FULL, &conn->c_flags);
		got = avail;
	} else {
		/* Sometimes you get what you want, lalala. */
		got = wanted;
	}
	newval -= IB_SET_SEND_CREDITS(got);

	/*
	 * If need_posted is non-zero, then the caller wants
	 * the posted regardless of whether any send credits are
	 * available.
	 */
	if (posted && (got || need_posted)) {
		advertise = min_t(unsigned int, posted, max_posted);
		newval -= IB_SET_POST_CREDITS(advertise);
	}

	/* Finally bill everything */
	if (atomic_cmpxchg(&ic->i_credits, oldval, newval) != oldval)
		goto try_again;

	*adv_credits = advertise;
	return got;
}

void rds_ib_send_add_credits(struct rds_connection *conn, unsigned int credits)
{
	struct rds_ib_connection *ic = conn->c_transport_data;

	if (credits == 0)
		return;

	rdsdebug("rds_ib_send_add_credits(%u): current=%u%s\n",
			credits,
			IB_GET_SEND_CREDITS(atomic_read(&ic->i_credits)),
			test_bit(RDS_LL_SEND_FULL, &conn->c_flags) ? ", ll_send_full" : "");

	atomic_add(IB_SET_SEND_CREDITS(credits), &ic->i_credits);
	if (test_and_clear_bit(RDS_LL_SEND_FULL, &conn->c_flags))
		queue_delayed_work(rds_wq, &conn->c_send_w, 0);

	WARN_ON(IB_GET_SEND_CREDITS(credits) >= 16384);

	rds_ib_stats_inc(s_ib_rx_credit_updates);
}

void rds_ib_advertise_credits(struct rds_connection *conn, unsigned int posted)
{
	struct rds_ib_connection *ic = conn->c_transport_data;

	if (posted == 0)
		return;

	atomic_add(IB_SET_POST_CREDITS(posted), &ic->i_credits);

	/* Decide whether to send an update to the peer now.
	 * If we would send a credit update for every single buffer we
	 * post, we would end up with an ACK storm (ACK arrives,
	 * consumes buffer, we refill the ring, send ACK to remote
	 * advertising the newly posted buffer... ad inf)
	 *
	 * Performance pretty much depends on how often we send
	 * credit updates - too frequent updates mean lots of ACKs.
	 * Too infrequent updates, and the peer will run out of
	 * credits and has to throttle.
	 * For the time being, 16 seems to be a good compromise.
	 */
	if (IB_GET_POST_CREDITS(atomic_read(&ic->i_credits)) >= 16)
		set_bit(IB_ACK_REQUESTED, &ic->i_ack_flags);
}

static inline void rds_ib_set_wr_signal_state(struct rds_ib_connection *ic,
					      struct rds_ib_send_work *send,
					      bool notify)
{
	/*
	 * We want to delay signaling completions just enough to get
	 * the batching benefits but not so much that we create dead time
	 * on the wire.
	 */
	if (ic->i_unsignaled_wrs-- == 0 || notify) {
		ic->i_unsignaled_wrs = rds_ib_sysctl_max_unsig_wrs;
		send->s_wr.send_flags |= IB_SEND_SIGNALED;
	}
}

/*
 * This can be called multiple times for a given message.  The first time
 * we see a message we map its scatterlist into the IB device so that
 * we can provide that mapped address to the IB scatter gather entries
 * in the IB work requests.  We translate the scatterlist into a series
 * of work requests that fragment the message.  These work requests complete
 * in order so we pass ownership of the message to the completion handler
 * once we send the final fragment.
 *
 * The RDS core uses the c_send_lock to only enter this function once
 * per connection.  This makes sure that the tx ring alloc/unalloc pairs
 * don't get out of sync and confuse the ring.
 */
int rds_ib_xmit(struct rds_connection *conn, struct rds_message *rm,
		unsigned int hdr_off, unsigned int sg, unsigned int off)
{
	struct rds_ib_connection *ic = conn->c_transport_data;
	struct ib_device *dev = ic->i_cm_id->device;
	struct rds_ib_send_work *send = NULL;
	struct rds_ib_send_work *first;
	struct rds_ib_send_work *prev;
	struct ib_send_wr *failed_wr;
	struct scatterlist *scat;
	u32 pos;
	u32 i;
	u32 work_alloc;
	u32 credit_alloc = 0;
	u32 posted;
	u32 adv_credits = 0;
	int send_flags = 0;
	int bytes_sent = 0;
	int ret;
	int flow_controlled = 0;

	BUG_ON(off % RDS_FRAG_SIZE);
	BUG_ON(hdr_off != 0 && hdr_off != sizeof(struct rds_header));

	/* Do not send cong updates to IB loopback */
	if (conn->c_loopback
	    && rm->m_inc.i_hdr.h_flags & RDS_FLAG_CONG_BITMAP) {
		rds_cong_map_updated(conn->c_fcong, ~(u64) 0);
		return sizeof(struct rds_header) + RDS_CONG_MAP_BYTES;
	}

	/* FIXME we may overallocate here */
	if (be32_to_cpu(rm->m_inc.i_hdr.h_len) == 0)
		i = 1;
	else
		i = ceil(be32_to_cpu(rm->m_inc.i_hdr.h_len), RDS_FRAG_SIZE);

	work_alloc = rds_ib_ring_alloc(&ic->i_send_ring, i, &pos);
	if (work_alloc == 0) {
		set_bit(RDS_LL_SEND_FULL, &conn->c_flags);
		rds_ib_stats_inc(s_ib_tx_ring_full);
		ret = -ENOMEM;
		goto out;
	}

	if (ic->i_flowctl) {
		credit_alloc = rds_ib_send_grab_credits(ic, work_alloc, &posted, 0, RDS_MAX_ADV_CREDIT);
		adv_credits += posted;
		if (credit_alloc < work_alloc) {
			rds_ib_ring_unalloc(&ic->i_send_ring, work_alloc - credit_alloc);
			work_alloc = credit_alloc;
			flow_controlled = 1;
		}
		if (work_alloc == 0) {
			set_bit(RDS_LL_SEND_FULL, &conn->c_flags);
			rds_ib_stats_inc(s_ib_tx_throttle);
			ret = -ENOMEM;
			goto out;
		}
	}

	/* map the message the first time we see it */
	if (!ic->i_rm) {
		if (rm->data.op_nents) {
			rm->data.op_count = ib_dma_map_sg(dev,
							  rm->data.op_sg,
							  rm->data.op_nents,
							  DMA_TO_DEVICE);
			rdsdebug("ic %p mapping rm %p: %d\n", ic, rm, rm->data.op_count);
			if (rm->data.op_count == 0) {
				rds_ib_stats_inc(s_ib_tx_sg_mapping_failure);
				rds_ib_ring_unalloc(&ic->i_send_ring, work_alloc);
				ret = -ENOMEM; /* XXX ? */
				goto out;
			}
		} else {
			rm->data.op_count = 0;
		}

		rds_message_addref(rm);
		ic->i_rm = rm;

		/* Finalize the header */
		if (test_bit(RDS_MSG_ACK_REQUIRED, &rm->m_flags))
			rm->m_inc.i_hdr.h_flags |= RDS_FLAG_ACK_REQUIRED;
		if (test_bit(RDS_MSG_RETRANSMITTED, &rm->m_flags))
			rm->m_inc.i_hdr.h_flags |= RDS_FLAG_RETRANSMITTED;

		/* If it has a RDMA op, tell the peer we did it. This is
		 * used by the peer to release use-once RDMA MRs. */
		if (rm->rdma.op_active) {
			struct rds_ext_header_rdma ext_hdr;

			ext_hdr.h_rdma_rkey = cpu_to_be32(rm->rdma.op_rkey);
			rds_message_add_extension(&rm->m_inc.i_hdr,
					RDS_EXTHDR_RDMA, &ext_hdr, sizeof(ext_hdr));
		}
		if (rm->m_rdma_cookie) {
			rds_message_add_rdma_dest_extension(&rm->m_inc.i_hdr,
					rds_rdma_cookie_key(rm->m_rdma_cookie),
					rds_rdma_cookie_offset(rm->m_rdma_cookie));
		}

		/* Note - rds_ib_piggyb_ack clears the ACK_REQUIRED bit, so
		 * we should not do this unless we have a chance of at least
		 * sticking the header into the send ring. Which is why we
		 * should call rds_ib_ring_alloc first. */
		rm->m_inc.i_hdr.h_ack = cpu_to_be64(rds_ib_piggyb_ack(ic));
		rds_message_make_checksum(&rm->m_inc.i_hdr);

		/*
		 * Update adv_credits since we reset the ACK_REQUIRED bit.
		 */
		if (ic->i_flowctl) {
			rds_ib_send_grab_credits(ic, 0, &posted, 1, RDS_MAX_ADV_CREDIT - adv_credits);
			adv_credits += posted;
			BUG_ON(adv_credits > 255);
		}
	}

	/* Sometimes you want to put a fence between an RDMA
	 * READ and the following SEND.
	 * We could either do this all the time
	 * or when requested by the user. Right now, we let
	 * the application choose.
	 */
	if (rm->rdma.op_active && rm->rdma.op_fence)
		send_flags = IB_SEND_FENCE;

	/* Each frag gets a header. Msgs may be 0 bytes */
	send = &ic->i_sends[pos];
	first = send;
	prev = NULL;
	scat = &rm->data.op_sg[sg];
	i = 0;
	do {
		unsigned int len = 0;

		/* Set up the header */
		send->s_wr.send_flags = send_flags;
		send->s_wr.opcode = IB_WR_SEND;
		send->s_wr.num_sge = 1;
		send->s_wr.next = NULL;
		send->s_queued = jiffies;
		send->s_op = NULL;

		send->s_sge[0].addr = ic->i_send_hdrs_dma
			+ (pos * sizeof(struct rds_header));
		send->s_sge[0].length = sizeof(struct rds_header);

		memcpy(&ic->i_send_hdrs[pos], &rm->m_inc.i_hdr, sizeof(struct rds_header));

		/* Set up the data, if present */
		if (i < work_alloc
		    && scat != &rm->data.op_sg[rm->data.op_count]) {
			len = min(RDS_FRAG_SIZE, ib_sg_dma_len(dev, scat) - off);
			send->s_wr.num_sge = 2;

			send->s_sge[1].addr = ib_sg_dma_address(dev, scat) + off;
			send->s_sge[1].length = len;

			bytes_sent += len;
			off += len;
			if (off == ib_sg_dma_len(dev, scat)) {
				scat++;
				off = 0;
			}
		}

		rds_ib_set_wr_signal_state(ic, send, 0);

		/*
		 * Always signal the last one if we're stopping due to flow control.
		 */
		if (ic->i_flowctl && flow_controlled && i == (work_alloc-1))
			send->s_wr.send_flags |= IB_SEND_SIGNALED | IB_SEND_SOLICITED;

		rdsdebug("send %p wr %p num_sge %u next %p\n", send,
			 &send->s_wr, send->s_wr.num_sge, send->s_wr.next);

		if (ic->i_flowctl && adv_credits) {
			struct rds_header *hdr = &ic->i_send_hdrs[pos];

			/* add credit and redo the header checksum */
			hdr->h_credit = adv_credits;
			rds_message_make_checksum(hdr);
			adv_credits = 0;
			rds_ib_stats_inc(s_ib_tx_credit_updates);
		}

		if (prev)
			prev->s_wr.next = &send->s_wr;
		prev = send;

		pos = (pos + 1) % ic->i_send_ring.w_nr;
		send = &ic->i_sends[pos];
		i++;

	} while (i < work_alloc
		 && scat != &rm->data.op_sg[rm->data.op_count]);

	/* Account the RDS header in the number of bytes we sent, but just once.
	 * The caller has no concept of fragmentation. */
	if (hdr_off == 0)
		bytes_sent += sizeof(struct rds_header);

	/* if we finished the message then send completion owns it */
	if (scat == &rm->data.op_sg[rm->data.op_count]) {
		prev->s_rm = ic->i_rm;
		prev->s_wr.send_flags |= IB_SEND_SOLICITED;
		ic->i_rm = NULL;
	}

	/* Put back wrs & credits we didn't use */
	if (i < work_alloc) {
		rds_ib_ring_unalloc(&ic->i_send_ring, work_alloc - i);
		work_alloc = i;
	}
	if (ic->i_flowctl && i < credit_alloc)
		rds_ib_send_add_credits(conn, credit_alloc - i);

	/* XXX need to worry about failed_wr and partial sends. */
	failed_wr = &first->s_wr;
	ret = ib_post_send(ic->i_cm_id->qp, &first->s_wr, &failed_wr);
	rdsdebug("ic %p first %p (wr %p) ret %d wr %p\n", ic,
		 first, &first->s_wr, ret, failed_wr);
	BUG_ON(failed_wr != &first->s_wr);
	if (ret) {
		printk(KERN_WARNING "RDS/IB: ib_post_send to %pI4 "
		       "returned %d\n", &conn->c_faddr, ret);
		rds_ib_ring_unalloc(&ic->i_send_ring, work_alloc);
		if (prev->s_rm) {
			ic->i_rm = prev->s_rm;
			prev->s_rm = NULL;
		}

		rds_ib_conn_error(ic->conn, "ib_post_send failed\n");
		goto out;
	}

	ret = bytes_sent;
out:
	BUG_ON(adv_credits);
	return ret;
}

/*
 * Issue atomic operation.
 * A simplified version of the rdma case, we always map 1 SG, and
 * only 8 bytes, for the return value from the atomic operation.
 */
int rds_ib_xmit_atomic(struct rds_connection *conn, struct rds_message *rm)
{
	struct rds_ib_connection *ic = conn->c_transport_data;
	struct rm_atomic_op *op = &rm->atomic;
	struct rds_ib_send_work *send = NULL;
	struct ib_send_wr *failed_wr;
	struct rds_ib_device *rds_ibdev;
	u32 pos;
	u32 work_alloc;
	int ret;

	rds_ibdev = ib_get_client_data(ic->i_cm_id->device, &rds_ib_client);

	work_alloc = rds_ib_ring_alloc(&ic->i_send_ring, 1, &pos);
	if (work_alloc != 1) {
		rds_ib_ring_unalloc(&ic->i_send_ring, work_alloc);
		rds_ib_stats_inc(s_ib_tx_ring_full);
		ret = -ENOMEM;
		goto out;
	}

	/* address of send request in ring */
	send = &ic->i_sends[pos];
	send->s_queued = jiffies;

	if (op->op_type == RDS_ATOMIC_TYPE_CSWP) {
		send->s_wr.opcode = IB_WR_ATOMIC_CMP_AND_SWP;
		send->s_wr.wr.atomic.compare_add = op->op_compare;
		send->s_wr.wr.atomic.swap = op->op_swap_add;
	} else { /* FADD */
		send->s_wr.opcode = IB_WR_ATOMIC_FETCH_AND_ADD;
		send->s_wr.wr.atomic.compare_add = op->op_swap_add;
		send->s_wr.wr.atomic.swap = 0;
	}
	rds_ib_set_wr_signal_state(ic, send, op->op_notify);
	send->s_wr.num_sge = 1;
	send->s_wr.next = NULL;
	send->s_wr.wr.atomic.remote_addr = op->op_remote_addr;
	send->s_wr.wr.atomic.rkey = op->op_rkey;

	/*
	 * If there is no data or rdma ops in the message, then
	 * we must fill in s_rm ourselves, so we properly clean up
	 * on completion.
	 */
	if (!rm->rdma.op_active && !rm->data.op_active)
		send->s_rm = rm;

	/* map 8 byte retval buffer to the device */
	ret = ib_dma_map_sg(ic->i_cm_id->device, op->op_sg, 1, DMA_FROM_DEVICE);
	rdsdebug("ic %p mapping atomic op %p. mapped %d pg\n", ic, op, ret);
	if (ret != 1) {
		rds_ib_ring_unalloc(&ic->i_send_ring, work_alloc);
		rds_ib_stats_inc(s_ib_tx_sg_mapping_failure);
		ret = -ENOMEM; /* XXX ? */
		goto out;
	}

	/* Convert our struct scatterlist to struct ib_sge */
	send->s_sge[0].addr = ib_sg_dma_address(ic->i_cm_id->device, op->op_sg);
	send->s_sge[0].length = ib_sg_dma_len(ic->i_cm_id->device, op->op_sg);
	send->s_sge[0].lkey = ic->i_mr->lkey;

	rdsdebug("rva %Lx rpa %Lx len %u\n", op->op_remote_addr,
		 send->s_sge[0].addr, send->s_sge[0].length);

	failed_wr = &send->s_wr;
	ret = ib_post_send(ic->i_cm_id->qp, &send->s_wr, &failed_wr);
	rdsdebug("ic %p send %p (wr %p) ret %d wr %p\n", ic,
		 send, &send->s_wr, ret, failed_wr);
	BUG_ON(failed_wr != &send->s_wr);
	if (ret) {
		printk(KERN_WARNING "RDS/IB: atomic ib_post_send to %pI4 "
		       "returned %d\n", &conn->c_faddr, ret);
		rds_ib_ring_unalloc(&ic->i_send_ring, work_alloc);
		goto out;
	}

	if (unlikely(failed_wr != &send->s_wr)) {
		printk(KERN_WARNING "RDS/IB: atomic ib_post_send() rc=%d, but failed_wqe updated!\n", ret);
		BUG_ON(failed_wr != &send->s_wr);
	}

out:
	return ret;
}

int rds_ib_xmit_rdma(struct rds_connection *conn, struct rm_rdma_op *op)
{
	struct rds_ib_connection *ic = conn->c_transport_data;
	struct rds_ib_send_work *send = NULL;
	struct rds_ib_send_work *first;
	struct rds_ib_send_work *prev;
	struct ib_send_wr *failed_wr;
	struct rds_ib_device *rds_ibdev;
	struct scatterlist *scat;
	unsigned long len;
	u64 remote_addr = op->op_remote_addr;
	u32 pos;
	u32 work_alloc;
	u32 i;
	u32 j;
	int sent;
	int ret;
	int num_sge;

	rds_ibdev = ib_get_client_data(ic->i_cm_id->device, &rds_ib_client);

	/* map the message the first time we see it */
	if (!op->op_mapped) {
		op->op_count = ib_dma_map_sg(ic->i_cm_id->device,
					     op->op_sg, op->op_nents, (op->op_write) ?
					     DMA_TO_DEVICE : DMA_FROM_DEVICE);
		rdsdebug("ic %p mapping op %p: %d\n", ic, op, op->op_count);
		if (op->op_count == 0) {
			rds_ib_stats_inc(s_ib_tx_sg_mapping_failure);
			ret = -ENOMEM; /* XXX ? */
			goto out;
		}

		op->op_mapped = 1;
	}

	/*
	 * Instead of knowing how to return a partial rdma read/write we insist that there
	 * be enough work requests to send the entire message.
	 */
	i = ceil(op->op_count, rds_ibdev->max_sge);

	work_alloc = rds_ib_ring_alloc(&ic->i_send_ring, i, &pos);
	if (work_alloc != i) {
		rds_ib_ring_unalloc(&ic->i_send_ring, work_alloc);
		rds_ib_stats_inc(s_ib_tx_ring_full);
		ret = -ENOMEM;
		goto out;
	}

	send = &ic->i_sends[pos];
	first = send;
	prev = NULL;
	scat = &op->op_sg[0];
	sent = 0;
	num_sge = op->op_count;

	for (i = 0; i < work_alloc && scat != &op->op_sg[op->op_count]; i++) {
		send->s_wr.send_flags = 0;
		send->s_queued = jiffies;

		rds_ib_set_wr_signal_state(ic, send, op->op_notify);

		send->s_wr.opcode = op->op_write ? IB_WR_RDMA_WRITE : IB_WR_RDMA_READ;
		send->s_wr.wr.rdma.remote_addr = remote_addr;
		send->s_wr.wr.rdma.rkey = op->op_rkey;
		send->s_op = op;

		if (num_sge > rds_ibdev->max_sge) {
			send->s_wr.num_sge = rds_ibdev->max_sge;
			num_sge -= rds_ibdev->max_sge;
		} else {
			send->s_wr.num_sge = num_sge;
		}

		send->s_wr.next = NULL;

		if (prev)
			prev->s_wr.next = &send->s_wr;

		for (j = 0; j < send->s_wr.num_sge && scat != &op->op_sg[op->op_count]; j++) {
			len = ib_sg_dma_len(ic->i_cm_id->device, scat);
			send->s_sge[j].addr =
				 ib_sg_dma_address(ic->i_cm_id->device, scat);
			send->s_sge[j].length = len;
			send->s_sge[j].lkey = ic->i_mr->lkey;

			sent += len;
			rdsdebug("ic %p sent %d remote_addr %llu\n", ic, sent, remote_addr);

			remote_addr += len;
			scat++;
		}

		rdsdebug("send %p wr %p num_sge %u next %p\n", send,
			&send->s_wr, send->s_wr.num_sge, send->s_wr.next);

		prev = send;
		if (++send == &ic->i_sends[ic->i_send_ring.w_nr])
			send = ic->i_sends;
	}

	if (i < work_alloc) {
		rds_ib_ring_unalloc(&ic->i_send_ring, work_alloc - i);
		work_alloc = i;
	}

	failed_wr = &first->s_wr;
	ret = ib_post_send(ic->i_cm_id->qp, &first->s_wr, &failed_wr);
	rdsdebug("ic %p first %p (wr %p) ret %d wr %p\n", ic,
		 first, &first->s_wr, ret, failed_wr);
	BUG_ON(failed_wr != &first->s_wr);
	if (ret) {
		printk(KERN_WARNING "RDS/IB: rdma ib_post_send to %pI4 "
		       "returned %d\n", &conn->c_faddr, ret);
		rds_ib_ring_unalloc(&ic->i_send_ring, work_alloc);
		goto out;
	}

	if (unlikely(failed_wr != &first->s_wr)) {
		printk(KERN_WARNING "RDS/IB: ib_post_send() rc=%d, but failed_wqe updated!\n", ret);
		BUG_ON(failed_wr != &first->s_wr);
	}


out:
	return ret;
}

void rds_ib_xmit_complete(struct rds_connection *conn)
{
	struct rds_ib_connection *ic = conn->c_transport_data;

	/* We may have a pending ACK or window update we were unable
	 * to send previously (due to flow control). Try again. */
	rds_ib_attempt_ack(ic);
}
