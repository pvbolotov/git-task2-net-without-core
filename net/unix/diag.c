#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/sock_diag.h>
#include <linux/unix_diag.h>
#include <linux/skbuff.h>
#include <linux/module.h>
#include <net/netlink.h>
#include <net/af_unix.h>
#include <net/tcp_states.h>

#define UNIX_DIAG_PUT(skb, attrtype, attrlen) \
	RTA_DATA(__RTA_PUT(skb, attrtype, attrlen))

static int sk_diag_dump_name(struct sock *sk, struct sk_buff *nlskb)
{
	struct unix_address *addr = unix_sk(sk)->addr;
	char *s;

	if (addr) {
		s = UNIX_DIAG_PUT(nlskb, UNIX_DIAG_NAME, addr->len - sizeof(short));
		memcpy(s, addr->name->sun_path, addr->len - sizeof(short));
	}

	return 0;

rtattr_failure:
	return -EMSGSIZE;
}

static int sk_diag_dump_vfs(struct sock *sk, struct sk_buff *nlskb)
{
	struct dentry *dentry = unix_sk(sk)->dentry;
	struct unix_diag_vfs *uv;

	if (dentry) {
		uv = UNIX_DIAG_PUT(nlskb, UNIX_DIAG_VFS, sizeof(*uv));
		uv->udiag_vfs_ino = dentry->d_inode->i_ino;
		uv->udiag_vfs_dev = dentry->d_sb->s_dev;
	}

	return 0;

rtattr_failure:
	return -EMSGSIZE;
}

static int sk_diag_dump_peer(struct sock *sk, struct sk_buff *nlskb)
{
	struct sock *peer;
	int ino;

	peer = unix_peer_get(sk);
	if (peer) {
		unix_state_lock(peer);
		ino = sock_i_ino(peer);
		unix_state_unlock(peer);
		sock_put(peer);

		RTA_PUT_U32(nlskb, UNIX_DIAG_PEER, ino);
	}

	return 0;
rtattr_failure:
	return -EMSGSIZE;
}

static int sk_diag_dump_icons(struct sock *sk, struct sk_buff *nlskb)
{
	struct sk_buff *skb;
	u32 *buf;
	int i;

	if (sk->sk_state == TCP_LISTEN) {
		spin_lock(&sk->sk_receive_queue.lock);
		buf = UNIX_DIAG_PUT(nlskb, UNIX_DIAG_ICONS,
				sk->sk_receive_queue.qlen * sizeof(u32));
		i = 0;
		skb_queue_walk(&sk->sk_receive_queue, skb) {
			struct sock *req, *peer;

			req = skb->sk;
			/*
			 * The state lock is outer for the same sk's
			 * queue lock. With the other's queue locked it's
			 * OK to lock the state.
			 */
			unix_state_lock_nested(req);
			peer = unix_sk(req)->peer;
			buf[i++] = (peer ? sock_i_ino(peer) : 0);
			unix_state_unlock(req);
		}
		spin_unlock(&sk->sk_receive_queue.lock);
	}

	return 0;

rtattr_failure:
	spin_unlock(&sk->sk_receive_queue.lock);
	return -EMSGSIZE;
}

static int sk_diag_show_rqlen(struct sock *sk, struct sk_buff *nlskb)
{
	RTA_PUT_U32(nlskb, UNIX_DIAG_RQLEN, sk->sk_receive_queue.qlen);
	return 0;

rtattr_failure:
	return -EMSGSIZE;
}

static int sk_diag_fill(struct sock *sk, struct sk_buff *skb, struct unix_diag_req *req,
		u32 pid, u32 seq, u32 flags, int sk_ino)
{
	unsigned char *b = skb_tail_pointer(skb);
	struct nlmsghdr *nlh;
	struct unix_diag_msg *rep;

	nlh = NLMSG_PUT(skb, pid, seq, SOCK_DIAG_BY_FAMILY, sizeof(*rep));
	nlh->nlmsg_flags = flags;

	rep = NLMSG_DATA(nlh);

	rep->udiag_family = AF_UNIX;
	rep->udiag_type = sk->sk_type;
	rep->udiag_state = sk->sk_state;
	rep->udiag_ino = sk_ino;
	sock_diag_save_cookie(sk, rep->udiag_cookie);

	if ((req->udiag_show & UDIAG_SHOW_NAME) &&
			sk_diag_dump_name(sk, skb))
		goto nlmsg_failure;

	if ((req->udiag_show & UDIAG_SHOW_VFS) &&
			sk_diag_dump_vfs(sk, skb))
		goto nlmsg_failure;

	if ((req->udiag_show & UDIAG_SHOW_PEER) &&
			sk_diag_dump_peer(sk, skb))
		goto nlmsg_failure;

	if ((req->udiag_show & UDIAG_SHOW_ICONS) &&
			sk_diag_dump_icons(sk, skb))
		goto nlmsg_failure;

	if ((req->udiag_show & UDIAG_SHOW_RQLEN) &&
			sk_diag_show_rqlen(sk, skb))
		goto nlmsg_failure;

	nlh->nlmsg_len = skb_tail_pointer(skb) - b;
	return skb->len;

nlmsg_failure:
	nlmsg_trim(skb, b);
	return -EMSGSIZE;
}

static int sk_diag_dump(struct sock *sk, struct sk_buff *skb, struct unix_diag_req *req,
		u32 pid, u32 seq, u32 flags)
{
	int sk_ino;

	unix_state_lock(sk);
	sk_ino = sock_i_ino(sk);
	unix_state_unlock(sk);

	if (!sk_ino)
		return 0;

	return sk_diag_fill(sk, skb, req, pid, seq, flags, sk_ino);
}

static int unix_diag_dump(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct unix_diag_req *req;
	int num, s_num, slot, s_slot;

	req = NLMSG_DATA(cb->nlh);

	s_slot = cb->args[0];
	num = s_num = cb->args[1];

	spin_lock(&unix_table_lock);
	for (slot = s_slot; slot <= UNIX_HASH_SIZE; s_num = 0, slot++) {
		struct sock *sk;
		struct hlist_node *node;

		num = 0;
		sk_for_each(sk, node, &unix_socket_table[slot]) {
			if (num < s_num)
				goto next;
			if (!(req->udiag_states & (1 << sk->sk_state)))
				goto next;
			if (sk_diag_dump(sk, skb, req,
						NETLINK_CB(cb->skb).pid,
						cb->nlh->nlmsg_seq,
						NLM_F_MULTI) < 0)
				goto done;
next:
			num++;
		}
	}
done:
	spin_unlock(&unix_table_lock);
	cb->args[0] = slot;
	cb->args[1] = num;

	return skb->len;
}

static struct sock *unix_lookup_by_ino(int ino)
{
	int i;
	struct sock *sk;

	spin_lock(&unix_table_lock);
	for (i = 0; i <= UNIX_HASH_SIZE; i++) {
		struct hlist_node *node;

		sk_for_each(sk, node, &unix_socket_table[i])
			if (ino == sock_i_ino(sk)) {
				sock_hold(sk);
				spin_unlock(&unix_table_lock);

				return sk;
			}
	}

	spin_unlock(&unix_table_lock);
	return NULL;
}

static int unix_diag_get_exact(struct sk_buff *in_skb,
			       const struct nlmsghdr *nlh,
			       struct unix_diag_req *req)
{
	int err = -EINVAL;
	struct sock *sk;
	struct sk_buff *rep;
	unsigned int extra_len;

	if (req->udiag_ino == 0)
		goto out_nosk;

	sk = unix_lookup_by_ino(req->udiag_ino);
	err = -ENOENT;
	if (sk == NULL)
		goto out_nosk;

	err = sock_diag_check_cookie(sk, req->udiag_cookie);
	if (err)
		goto out;

	extra_len = 256;
again:
	err = -ENOMEM;
	rep = alloc_skb(NLMSG_SPACE((sizeof(struct unix_diag_msg) + extra_len)),
			GFP_KERNEL);
	if (!rep)
		goto out;

	err = sk_diag_fill(sk, rep, req, NETLINK_CB(in_skb).pid,
			   nlh->nlmsg_seq, 0, req->udiag_ino);
	if (err < 0) {
		kfree_skb(rep);
		extra_len += 256;
		if (extra_len >= PAGE_SIZE)
			goto out;

		goto again;
	}
	err = netlink_unicast(sock_diag_nlsk, rep, NETLINK_CB(in_skb).pid,
			      MSG_DONTWAIT);
	if (err > 0)
		err = 0;
out:
	if (sk)
		sock_put(sk);
out_nosk:
	return err;
}

static int unix_diag_handler_dump(struct sk_buff *skb, struct nlmsghdr *h)
{
	int hdrlen = sizeof(struct unix_diag_req);

	if (nlmsg_len(h) < hdrlen)
		return -EINVAL;

	if (h->nlmsg_flags & NLM_F_DUMP)
		return netlink_dump_start(sock_diag_nlsk, skb, h,
					  unix_diag_dump, NULL, 0);
	else
		return unix_diag_get_exact(skb, h, (struct unix_diag_req *)NLMSG_DATA(h));
}

static struct sock_diag_handler unix_diag_handler = {
	.family = AF_UNIX,
	.dump = unix_diag_handler_dump,
};

static int __init unix_diag_init(void)
{
	return sock_diag_register(&unix_diag_handler);
}

static void __exit unix_diag_exit(void)
{
	sock_diag_unregister(&unix_diag_handler);
}

module_init(unix_diag_init);
module_exit(unix_diag_exit);
MODULE_LICENSE("GPL");
MODULE_ALIAS_NET_PF_PROTO_TYPE(PF_NETLINK, NETLINK_SOCK_DIAG, 1 /* AF_LOCAL */);
