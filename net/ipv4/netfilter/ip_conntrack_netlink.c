/* Connection tracking via netlink socket. Allows for user space
 * protocol helpers and general trouble making from userspace.
 *
 * (C) 2001 by Jay Schulist <jschlst@samba.org>
 * (C) 2002-2005 by Harald Welte <laforge@gnumonks.org>
 * (C) 2003 by Patrick Mchardy <kaber@trash.net>
 * (C) 2005 by Pablo Neira Ayuso <pablo@eurodev.net>
 *
 * I've reworked this stuff to use attributes instead of conntrack 
 * structures. 5.44 am. I need more tea. --pablo 05/07/11.
 *
 * Initial connection tracking via netlink development funded and 
 * generally made possible by Network Robots, Inc. (www.networkrobots.com)
 *
 * Further development of this code funded by Astaro AG (http://www.astaro.com)
 *
 * This software may be used and distributed according to the terms
 * of the GNU General Public License, incorporated herein by reference.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/timer.h>
#include <linux/skbuff.h>
#include <linux/errno.h>
#include <linux/netlink.h>
#include <linux/spinlock.h>
#include <linux/notifier.h>
#include <linux/rtnetlink.h>

#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter_ipv4/ip_tables.h>
#include <linux/netfilter_ipv4/ip_conntrack.h>
#include <linux/netfilter_ipv4/ip_conntrack_core.h>
#include <linux/netfilter_ipv4/ip_conntrack_helper.h>
#include <linux/netfilter_ipv4/ip_conntrack_protocol.h>
#include <linux/netfilter_ipv4/ip_nat_protocol.h>

#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nfnetlink_conntrack.h>

MODULE_LICENSE("GPL");

static char __initdata version[] = "0.90";

#if 0
#define DEBUGP printk
#else
#define DEBUGP(format, args...)
#endif


static inline int
ctnetlink_dump_tuples_proto(struct sk_buff *skb, 
			    const struct ip_conntrack_tuple *tuple)
{
	struct ip_conntrack_protocol *proto;

	NFA_PUT(skb, CTA_PROTO_NUM, sizeof(u_int8_t), &tuple->dst.protonum);

	proto = ip_conntrack_proto_find_get(tuple->dst.protonum);
	if (proto && proto->tuple_to_nfattr)
		return proto->tuple_to_nfattr(skb, tuple);

	return 0;

nfattr_failure:
	return -1;
}

static inline int
ctnetlink_dump_tuples(struct sk_buff *skb, 
		      const struct ip_conntrack_tuple *tuple)
{
	struct nfattr *nest_parms;
	
	nest_parms = NFA_NEST(skb, CTA_TUPLE_IP);
	NFA_PUT(skb, CTA_IP_V4_SRC, sizeof(u_int32_t), &tuple->src.ip);
	NFA_PUT(skb, CTA_IP_V4_DST, sizeof(u_int32_t), &tuple->dst.ip);
	NFA_NEST_END(skb, nest_parms);

	nest_parms = NFA_NEST(skb, CTA_TUPLE_PROTO);
	ctnetlink_dump_tuples_proto(skb, tuple);
	NFA_NEST_END(skb, nest_parms);

	return 0;

nfattr_failure:
	return -1;
}

static inline int
ctnetlink_dump_status(struct sk_buff *skb, const struct ip_conntrack *ct)
{
	u_int32_t status = htonl((u_int32_t) ct->status);
	NFA_PUT(skb, CTA_STATUS, sizeof(status), &status);
	return 0;

nfattr_failure:
	return -1;
}

static inline int
ctnetlink_dump_timeout(struct sk_buff *skb, const struct ip_conntrack *ct)
{
	long timeout_l = ct->timeout.expires - jiffies;
	u_int32_t timeout;

	if (timeout_l < 0)
		timeout = 0;
	else
		timeout = htonl(timeout_l / HZ);
	
	NFA_PUT(skb, CTA_TIMEOUT, sizeof(timeout), &timeout);
	return 0;

nfattr_failure:
	return -1;
}

static inline int
ctnetlink_dump_protoinfo(struct sk_buff *skb, const struct ip_conntrack *ct)
{
	struct ip_conntrack_protocol *proto = ip_conntrack_proto_find_get(ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.dst.protonum);

	struct nfattr *nest_proto;
	int ret;
	
	if (!proto || !proto->to_nfattr)
		return 0;
	
	nest_proto = NFA_NEST(skb, CTA_PROTOINFO);

	ret = proto->to_nfattr(skb, nest_proto, ct);

	ip_conntrack_proto_put(proto);

	NFA_NEST_END(skb, nest_proto);

	return ret;

nfattr_failure:
	return -1;
}

static inline int
ctnetlink_dump_helpinfo(struct sk_buff *skb, const struct ip_conntrack *ct)
{
	struct nfattr *nest_helper;

	if (!ct->helper)
		return 0;
		
	nest_helper = NFA_NEST(skb, CTA_HELP);
	NFA_PUT(skb, CTA_HELP_NAME, CTA_HELP_MAXNAMESIZE, &ct->helper->name);

	if (ct->helper->to_nfattr)
		ct->helper->to_nfattr(skb, ct);

	NFA_NEST_END(skb, nest_helper);

	return 0;

nfattr_failure:
	return -1;
}

#ifdef CONFIG_IP_NF_CT_ACCT
static inline int
ctnetlink_dump_counters(struct sk_buff *skb, const struct ip_conntrack *ct,
			enum ip_conntrack_dir dir)
{
	enum ctattr_type type = dir ? CTA_COUNTERS_REPLY: CTA_COUNTERS_ORIG;
	struct nfattr *nest_count = NFA_NEST(skb, type);
	u_int64_t tmp;

	tmp = cpu_to_be64(ct->counters[dir].packets);
	NFA_PUT(skb, CTA_COUNTERS_PACKETS, sizeof(u_int64_t), &tmp);

	tmp = cpu_to_be64(ct->counters[dir].bytes);
	NFA_PUT(skb, CTA_COUNTERS_BYTES, sizeof(u_int64_t), &tmp);

	NFA_NEST_END(skb, nest_count);

	return 0;

nfattr_failure:
	return -1;
}
#else
#define ctnetlink_dump_counters(a, b, c) (0)
#endif

#ifdef CONFIG_IP_NF_CONNTRACK_MARK
static inline int
ctnetlink_dump_mark(struct sk_buff *skb, const struct ip_conntrack *ct)
{
	u_int32_t mark = htonl(ct->mark);

	NFA_PUT(skb, CTA_MARK, sizeof(u_int32_t), &mark);
	return 0;

nfattr_failure:
	return -1;
}
#else
#define ctnetlink_dump_mark(a, b) (0)
#endif

static inline int
ctnetlink_dump_id(struct sk_buff *skb, const struct ip_conntrack *ct)
{
	u_int32_t id = htonl(ct->id);
	NFA_PUT(skb, CTA_ID, sizeof(u_int32_t), &id);
	return 0;

nfattr_failure:
	return -1;
}

static inline int
ctnetlink_dump_use(struct sk_buff *skb, const struct ip_conntrack *ct)
{
	unsigned int use = htonl(atomic_read(&ct->ct_general.use));
	
	NFA_PUT(skb, CTA_USE, sizeof(u_int32_t), &use);
	return 0;

nfattr_failure:
	return -1;
}

#define tuple(ct, dir) (&(ct)->tuplehash[dir].tuple)

static int
ctnetlink_fill_info(struct sk_buff *skb, u32 pid, u32 seq,
		    int event, int nowait, 
		    const struct ip_conntrack *ct)
{
	struct nlmsghdr *nlh;
	struct nfgenmsg *nfmsg;
	struct nfattr *nest_parms;
	unsigned char *b;

	b = skb->tail;

	event |= NFNL_SUBSYS_CTNETLINK << 8;
	nlh    = NLMSG_PUT(skb, pid, seq, event, sizeof(struct nfgenmsg));
	nfmsg  = NLMSG_DATA(nlh);

	nlh->nlmsg_flags    = (nowait && pid) ? NLM_F_MULTI : 0;
	nfmsg->nfgen_family = AF_INET;
	nfmsg->version      = NFNETLINK_V0;
	nfmsg->res_id	    = 0;

	nest_parms = NFA_NEST(skb, CTA_TUPLE_ORIG);
	if (ctnetlink_dump_tuples(skb, tuple(ct, IP_CT_DIR_ORIGINAL)) < 0)
		goto nfattr_failure;
	NFA_NEST_END(skb, nest_parms);
	
	nest_parms = NFA_NEST(skb, CTA_TUPLE_REPLY);
	if (ctnetlink_dump_tuples(skb, tuple(ct, IP_CT_DIR_REPLY)) < 0)
		goto nfattr_failure;
	NFA_NEST_END(skb, nest_parms);

	if (ctnetlink_dump_status(skb, ct) < 0 ||
	    ctnetlink_dump_timeout(skb, ct) < 0 ||
	    ctnetlink_dump_counters(skb, ct, IP_CT_DIR_ORIGINAL) < 0 ||
	    ctnetlink_dump_counters(skb, ct, IP_CT_DIR_REPLY) < 0 ||
	    ctnetlink_dump_protoinfo(skb, ct) < 0 ||
	    ctnetlink_dump_helpinfo(skb, ct) < 0 ||
	    ctnetlink_dump_mark(skb, ct) < 0 ||
	    ctnetlink_dump_id(skb, ct) < 0 ||
	    ctnetlink_dump_use(skb, ct) < 0)
		goto nfattr_failure;

	nlh->nlmsg_len = skb->tail - b;
	return skb->len;

nlmsg_failure:
nfattr_failure:
	skb_trim(skb, b - skb->data);
	return -1;
}

#ifdef CONFIG_IP_NF_CONNTRACK_EVENTS
static int ctnetlink_conntrack_event(struct notifier_block *this,
                                     unsigned long events, void *ptr)
{
	struct nlmsghdr *nlh;
	struct nfgenmsg *nfmsg;
	struct nfattr *nest_parms;
	struct ip_conntrack *ct = (struct ip_conntrack *)ptr;
	struct sk_buff *skb;
	unsigned int type;
	unsigned char *b;
	unsigned int flags = 0, groups;

	/* ignore our fake conntrack entry */
	if (ct == &ip_conntrack_untracked)
		return NOTIFY_DONE;

	if (events & IPCT_DESTROY) {
		type = IPCTNL_MSG_CT_DELETE;
		groups = NF_NETLINK_CONNTRACK_DESTROY;
		goto alloc_skb;
	}
	if (events & (IPCT_NEW | IPCT_RELATED)) {
		type = IPCTNL_MSG_CT_NEW;
		flags = NLM_F_CREATE|NLM_F_EXCL;
		/* dump everything */
		events = ~0UL;
		groups = NF_NETLINK_CONNTRACK_NEW;
		goto alloc_skb;
	}
	if (events & (IPCT_STATUS |
		      IPCT_PROTOINFO |
		      IPCT_HELPER |
		      IPCT_HELPINFO |
		      IPCT_NATINFO)) {
		type = IPCTNL_MSG_CT_NEW;
		groups = NF_NETLINK_CONNTRACK_UPDATE;
		goto alloc_skb;
	} 
	
	return NOTIFY_DONE;

alloc_skb:
  /* FIXME: Check if there are any listeners before, don't hurt performance */
	
	skb = alloc_skb(NLMSG_GOODSIZE, GFP_ATOMIC);
	if (!skb)
		return NOTIFY_DONE;

	b = skb->tail;

	type |= NFNL_SUBSYS_CTNETLINK << 8;
	nlh   = NLMSG_PUT(skb, 0, 0, type, sizeof(struct nfgenmsg));
	nfmsg = NLMSG_DATA(nlh);

	nlh->nlmsg_flags    = flags;
	nfmsg->nfgen_family = AF_INET;
	nfmsg->version	= NFNETLINK_V0;
	nfmsg->res_id	= 0;

	nest_parms = NFA_NEST(skb, CTA_TUPLE_ORIG);
	if (ctnetlink_dump_tuples(skb, tuple(ct, IP_CT_DIR_ORIGINAL)) < 0)
		goto nfattr_failure;
	NFA_NEST_END(skb, nest_parms);
	
	nest_parms = NFA_NEST(skb, CTA_TUPLE_REPLY);
	if (ctnetlink_dump_tuples(skb, tuple(ct, IP_CT_DIR_REPLY)) < 0)
		goto nfattr_failure;
	NFA_NEST_END(skb, nest_parms);
	
	/* NAT stuff is now a status flag */
	if ((events & IPCT_STATUS || events & IPCT_NATINFO)
	    && ctnetlink_dump_status(skb, ct) < 0)
		goto nfattr_failure;
	if (events & IPCT_REFRESH
	    && ctnetlink_dump_timeout(skb, ct) < 0)
		goto nfattr_failure;
	if (events & IPCT_PROTOINFO
	    && ctnetlink_dump_protoinfo(skb, ct) < 0)
		goto nfattr_failure;
	if (events & IPCT_HELPINFO
	    && ctnetlink_dump_helpinfo(skb, ct) < 0)
		goto nfattr_failure;

	if (ctnetlink_dump_counters(skb, ct, IP_CT_DIR_ORIGINAL) < 0 ||
	    ctnetlink_dump_counters(skb, ct, IP_CT_DIR_REPLY) < 0)
		goto nfattr_failure;

	nlh->nlmsg_len = skb->tail - b;
	nfnetlink_send(skb, 0, groups, 0);
	return NOTIFY_DONE;

nlmsg_failure:
nfattr_failure:
	kfree_skb(skb);
	return NOTIFY_DONE;
}
#endif /* CONFIG_IP_NF_CONNTRACK_EVENTS */

static int ctnetlink_done(struct netlink_callback *cb)
{
	DEBUGP("entered %s\n", __FUNCTION__);
	return 0;
}

static int
ctnetlink_dump_table(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct ip_conntrack *ct = NULL;
	struct ip_conntrack_tuple_hash *h;
	struct list_head *i;
	u_int32_t *id = (u_int32_t *) &cb->args[1];

	DEBUGP("entered %s, last bucket=%lu id=%u\n", __FUNCTION__, 
			cb->args[0], *id);

	read_lock_bh(&ip_conntrack_lock);
	for (; cb->args[0] < ip_conntrack_htable_size; cb->args[0]++, *id = 0) {
		list_for_each(i, &ip_conntrack_hash[cb->args[0]]) {
			h = (struct ip_conntrack_tuple_hash *) i;
			if (DIRECTION(h) != IP_CT_DIR_ORIGINAL)
				continue;
			ct = tuplehash_to_ctrack(h);
			if (ct->id <= *id)
				continue;
			if (ctnetlink_fill_info(skb, NETLINK_CB(cb->skb).pid,
		                        	cb->nlh->nlmsg_seq,
						IPCTNL_MSG_CT_NEW,
						1, ct) < 0)
				goto out;
			*id = ct->id;
		}
	}
out:	
	read_unlock_bh(&ip_conntrack_lock);

	DEBUGP("leaving, last bucket=%lu id=%u\n", cb->args[0], *id);

	return skb->len;
}

#ifdef CONFIG_IP_NF_CT_ACCT
static int
ctnetlink_dump_table_w(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct ip_conntrack *ct = NULL;
	struct ip_conntrack_tuple_hash *h;
	struct list_head *i;
	u_int32_t *id = (u_int32_t *) &cb->args[1];

	DEBUGP("entered %s, last bucket=%u id=%u\n", __FUNCTION__, 
			cb->args[0], *id);

	write_lock_bh(&ip_conntrack_lock);
	for (; cb->args[0] < ip_conntrack_htable_size; cb->args[0]++, *id = 0) {
		list_for_each(i, &ip_conntrack_hash[cb->args[0]]) {
			h = (struct ip_conntrack_tuple_hash *) i;
			if (DIRECTION(h) != IP_CT_DIR_ORIGINAL)
				continue;
			ct = tuplehash_to_ctrack(h);
			if (ct->id <= *id)
				continue;
			if (ctnetlink_fill_info(skb, NETLINK_CB(cb->skb).pid,
		                        	cb->nlh->nlmsg_seq,
						IPCTNL_MSG_CT_NEW,
						1, ct) < 0)
				goto out;
			*id = ct->id;

			memset(&ct->counters, 0, sizeof(ct->counters));
		}
	}
out:	
	write_unlock_bh(&ip_conntrack_lock);

	DEBUGP("leaving, last bucket=%lu id=%u\n", cb->args[0], *id);

	return skb->len;
}
#endif

static const int cta_min_ip[CTA_IP_MAX] = {
	[CTA_IP_V4_SRC-1]	= sizeof(u_int32_t),
	[CTA_IP_V4_DST-1]	= sizeof(u_int32_t),
};

static inline int
ctnetlink_parse_tuple_ip(struct nfattr *attr, struct ip_conntrack_tuple *tuple)
{
	struct nfattr *tb[CTA_IP_MAX];

	DEBUGP("entered %s\n", __FUNCTION__);

	
	if (nfattr_parse_nested(tb, CTA_IP_MAX, attr) < 0)
		goto nfattr_failure;

	if (nfattr_bad_size(tb, CTA_IP_MAX, cta_min_ip))
		return -EINVAL;

	if (!tb[CTA_IP_V4_SRC-1])
		return -EINVAL;
	tuple->src.ip = *(u_int32_t *)NFA_DATA(tb[CTA_IP_V4_SRC-1]);

	if (!tb[CTA_IP_V4_DST-1])
		return -EINVAL;
	tuple->dst.ip = *(u_int32_t *)NFA_DATA(tb[CTA_IP_V4_DST-1]);

	DEBUGP("leaving\n");

	return 0;

nfattr_failure:
	return -1;
}

static const int cta_min_proto[CTA_PROTO_MAX] = {
	[CTA_PROTO_NUM-1]	= sizeof(u_int16_t),
	[CTA_PROTO_SRC_PORT-1]	= sizeof(u_int16_t),
	[CTA_PROTO_DST_PORT-1]	= sizeof(u_int16_t),
	[CTA_PROTO_ICMP_TYPE-1]	= sizeof(u_int8_t),
	[CTA_PROTO_ICMP_CODE-1]	= sizeof(u_int8_t),
	[CTA_PROTO_ICMP_ID-1]	= sizeof(u_int16_t),
};

static inline int
ctnetlink_parse_tuple_proto(struct nfattr *attr, 
			    struct ip_conntrack_tuple *tuple)
{
	struct nfattr *tb[CTA_PROTO_MAX];
	struct ip_conntrack_protocol *proto;
	int ret = 0;

	DEBUGP("entered %s\n", __FUNCTION__);

	if (nfattr_parse_nested(tb, CTA_PROTO_MAX, attr) < 0)
		goto nfattr_failure;

	if (nfattr_bad_size(tb, CTA_PROTO_MAX, cta_min_proto))
		return -EINVAL;

	if (!tb[CTA_PROTO_NUM-1])
		return -EINVAL;
	tuple->dst.protonum = *(u_int16_t *)NFA_DATA(tb[CTA_PROTO_NUM-1]);

	proto = ip_conntrack_proto_find_get(tuple->dst.protonum);

	if (likely(proto && proto->nfattr_to_tuple)) {
		ret = proto->nfattr_to_tuple(tb, tuple);
		ip_conntrack_proto_put(proto);
	}
	
	return ret;

nfattr_failure:
	return -1;
}

static inline int
ctnetlink_parse_tuple(struct nfattr *cda[], struct ip_conntrack_tuple *tuple,
		      enum ctattr_tuple type)
{
	struct nfattr *tb[CTA_TUPLE_MAX];
	int err;

	DEBUGP("entered %s\n", __FUNCTION__);

	memset(tuple, 0, sizeof(*tuple));

	if (nfattr_parse_nested(tb, CTA_TUPLE_MAX, cda[type-1]) < 0)
		goto nfattr_failure;

	if (!tb[CTA_TUPLE_IP-1])
		return -EINVAL;

	err = ctnetlink_parse_tuple_ip(tb[CTA_TUPLE_IP-1], tuple);
	if (err < 0)
		return err;

	if (!tb[CTA_TUPLE_PROTO-1])
		return -EINVAL;

	err = ctnetlink_parse_tuple_proto(tb[CTA_TUPLE_PROTO-1], tuple);
	if (err < 0)
		return err;

	/* orig and expect tuples get DIR_ORIGINAL */
	if (type == CTA_TUPLE_REPLY)
		tuple->dst.dir = IP_CT_DIR_REPLY;
	else
		tuple->dst.dir = IP_CT_DIR_ORIGINAL;

	DUMP_TUPLE(tuple);

	DEBUGP("leaving\n");

	return 0;

nfattr_failure:
	return -1;
}

#ifdef CONFIG_IP_NF_NAT_NEEDED
static const int cta_min_protonat[CTA_PROTONAT_MAX] = {
	[CTA_PROTONAT_PORT_MIN-1]	= sizeof(u_int16_t),
	[CTA_PROTONAT_PORT_MAX-1]	= sizeof(u_int16_t),
};

static int ctnetlink_parse_nat_proto(struct nfattr *attr,
				     const struct ip_conntrack *ct,
				     struct ip_nat_range *range)
{
	struct nfattr *tb[CTA_PROTONAT_MAX];
	struct ip_nat_protocol *npt;

	DEBUGP("entered %s\n", __FUNCTION__);

	if (nfattr_parse_nested(tb, CTA_PROTONAT_MAX, attr) < 0)
		goto nfattr_failure;

	if (nfattr_bad_size(tb, CTA_PROTONAT_MAX, cta_min_protonat))
		goto nfattr_failure;

	npt = ip_nat_proto_find_get(ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple.dst.protonum);
	if (!npt)
		return 0;

	if (!npt->nfattr_to_range) {
		ip_nat_proto_put(npt);
		return 0;
	}

	/* nfattr_to_range returns 1 if it parsed, 0 if not, neg. on error */
	if (npt->nfattr_to_range(tb, range) > 0)
		range->flags |= IP_NAT_RANGE_PROTO_SPECIFIED;

	ip_nat_proto_put(npt);

	DEBUGP("leaving\n");
	return 0;

nfattr_failure:
	return -1;
}

static inline int
ctnetlink_parse_nat(struct nfattr *cda[],
		    const struct ip_conntrack *ct, struct ip_nat_range *range)
{
	struct nfattr *tb[CTA_NAT_MAX];
	int err;

	DEBUGP("entered %s\n", __FUNCTION__);

	memset(range, 0, sizeof(*range));
	
	if (nfattr_parse_nested(tb, CTA_NAT_MAX, cda[CTA_NAT-1]) < 0)
		goto nfattr_failure;

	if (tb[CTA_NAT_MINIP-1])
		range->min_ip = *(u_int32_t *)NFA_DATA(tb[CTA_NAT_MINIP-1]);

	if (!tb[CTA_NAT_MAXIP-1])
		range->max_ip = range->min_ip;
	else
		range->max_ip = *(u_int32_t *)NFA_DATA(tb[CTA_NAT_MAXIP-1]);

	if (range->min_ip)
		range->flags |= IP_NAT_RANGE_MAP_IPS;

	if (!tb[CTA_NAT_PROTO-1])
		return 0;

	err = ctnetlink_parse_nat_proto(tb[CTA_NAT_PROTO-1], ct, range);
	if (err < 0)
		return err;

	DEBUGP("leaving\n");
	return 0;

nfattr_failure:
	return -1;
}
#endif

static inline int
ctnetlink_parse_help(struct nfattr *attr, char **helper_name)
{
	struct nfattr *tb[CTA_HELP_MAX];

	DEBUGP("entered %s\n", __FUNCTION__);

	if (nfattr_parse_nested(tb, CTA_HELP_MAX, attr) < 0)
		goto nfattr_failure;

	if (!tb[CTA_HELP_NAME-1])
		return -EINVAL;

	*helper_name = NFA_DATA(tb[CTA_HELP_NAME-1]);

	return 0;

nfattr_failure:
	return -1;
}

static int
ctnetlink_del_conntrack(struct sock *ctnl, struct sk_buff *skb, 
			struct nlmsghdr *nlh, struct nfattr *cda[], int *errp)
{
	struct ip_conntrack_tuple_hash *h;
	struct ip_conntrack_tuple tuple;
	struct ip_conntrack *ct;
	int err = 0;

	DEBUGP("entered %s\n", __FUNCTION__);

	if (cda[CTA_TUPLE_ORIG-1])
		err = ctnetlink_parse_tuple(cda, &tuple, CTA_TUPLE_ORIG);
	else if (cda[CTA_TUPLE_REPLY-1])
		err = ctnetlink_parse_tuple(cda, &tuple, CTA_TUPLE_REPLY);
	else {
		/* Flush the whole table */
		ip_conntrack_flush();
		return 0;
	}

	if (err < 0)
		return err;

	h = ip_conntrack_find_get(&tuple, NULL);
	if (!h) {
		DEBUGP("tuple not found in conntrack hash\n");
		return -ENOENT;
	}

	ct = tuplehash_to_ctrack(h);
	
	if (cda[CTA_ID-1]) {
		u_int32_t id = ntohl(*(u_int32_t *)NFA_DATA(cda[CTA_ID-1]));
		if (ct->id != id) {
			ip_conntrack_put(ct);
			return -ENOENT;
		}
	}	
	if (del_timer(&ct->timeout)) {
		ip_conntrack_put(ct);
		ct->timeout.function((unsigned long)ct);
		return 0;
	}
	ip_conntrack_put(ct);
	DEBUGP("leaving\n");

	return 0;
}

static int
ctnetlink_get_conntrack(struct sock *ctnl, struct sk_buff *skb, 
			struct nlmsghdr *nlh, struct nfattr *cda[], int *errp)
{
	struct ip_conntrack_tuple_hash *h;
	struct ip_conntrack_tuple tuple;
	struct ip_conntrack *ct;
	struct sk_buff *skb2 = NULL;
	int err = 0;

	DEBUGP("entered %s\n", __FUNCTION__);

	if (nlh->nlmsg_flags & NLM_F_DUMP) {
		struct nfgenmsg *msg = NLMSG_DATA(nlh);
		u32 rlen;

		if (msg->nfgen_family != AF_INET)
			return -EAFNOSUPPORT;

		if (NFNL_MSG_TYPE(nlh->nlmsg_type) ==
					IPCTNL_MSG_CT_GET_CTRZERO) {
#ifdef CONFIG_IP_NF_CT_ACCT
			if ((*errp = netlink_dump_start(ctnl, skb, nlh,
						ctnetlink_dump_table_w,
						ctnetlink_done)) != 0)
				return -EINVAL;
#else
			return -ENOTSUPP;
#endif
		} else {
			if ((*errp = netlink_dump_start(ctnl, skb, nlh,
		      		                        ctnetlink_dump_table,
		                                	ctnetlink_done)) != 0)
			return -EINVAL;
		}

		rlen = NLMSG_ALIGN(nlh->nlmsg_len);
		if (rlen > skb->len)
			rlen = skb->len;
		skb_pull(skb, rlen);
		return 0;
	}

	if (cda[CTA_TUPLE_ORIG-1])
		err = ctnetlink_parse_tuple(cda, &tuple, CTA_TUPLE_ORIG);
	else if (cda[CTA_TUPLE_REPLY-1])
		err = ctnetlink_parse_tuple(cda, &tuple, CTA_TUPLE_REPLY);
	else
		return -EINVAL;

	if (err < 0)
		return err;

	h = ip_conntrack_find_get(&tuple, NULL);
	if (!h) {
		DEBUGP("tuple not found in conntrack hash");
		return -ENOENT;
	}
	DEBUGP("tuple found\n");
	ct = tuplehash_to_ctrack(h);

	err = -ENOMEM;
	skb2 = alloc_skb(NLMSG_GOODSIZE, GFP_ATOMIC);
	if (!skb2) {
		ip_conntrack_put(ct);
		return -ENOMEM;
	}
	NETLINK_CB(skb2).dst_pid = NETLINK_CB(skb).pid;

	err = ctnetlink_fill_info(skb2, NETLINK_CB(skb).pid, nlh->nlmsg_seq, 
				  IPCTNL_MSG_CT_NEW, 1, ct);
	ip_conntrack_put(ct);
	if (err <= 0)
		goto out;

	err = netlink_unicast(ctnl, skb2, NETLINK_CB(skb).pid, MSG_DONTWAIT);
	if (err < 0)
		goto out;

	DEBUGP("leaving\n");
	return 0;

out:
	if (skb2)
		kfree_skb(skb2);
	return -1;
}

static inline int
ctnetlink_change_status(struct ip_conntrack *ct, struct nfattr *cda[])
{
	unsigned long d, status = *(u_int32_t *)NFA_DATA(cda[CTA_STATUS-1]);
	d = ct->status ^ status;

	if (d & (IPS_EXPECTED|IPS_CONFIRMED|IPS_DYING))
		/* unchangeable */
		return -EINVAL;
	
	if (d & IPS_SEEN_REPLY && !(status & IPS_SEEN_REPLY))
		/* SEEN_REPLY bit can only be set */
		return -EINVAL;

	
	if (d & IPS_ASSURED && !(status & IPS_ASSURED))
		/* ASSURED bit can only be set */
		return -EINVAL;

	if (cda[CTA_NAT-1]) {
#ifndef CONFIG_IP_NF_NAT_NEEDED
		return -EINVAL;
#else
		unsigned int hooknum;
		struct ip_nat_range range;

		if (ctnetlink_parse_nat(cda, ct, &range) < 0)
			return -EINVAL;

		DEBUGP("NAT: %u.%u.%u.%u-%u.%u.%u.%u:%u-%u\n", 
		       NIPQUAD(range.min_ip), NIPQUAD(range.max_ip),
		       htons(range.min.all), htons(range.max.all));
		
		/* This is tricky but it works. ip_nat_setup_info needs the
		 * hook number as parameter, so let's do the correct 
		 * conversion and run away */
		if (status & IPS_SRC_NAT_DONE)
			hooknum = NF_IP_POST_ROUTING; /* IP_NAT_MANIP_SRC */
		else if (status & IPS_DST_NAT_DONE)
			hooknum = NF_IP_PRE_ROUTING;  /* IP_NAT_MANIP_DST */
		else 
			return -EINVAL; /* Missing NAT flags */

		DEBUGP("NAT status: %lu\n", 
		       status & (IPS_NAT_MASK | IPS_NAT_DONE_MASK));
		
		if (ip_nat_initialized(ct, hooknum))
			return -EEXIST;
		ip_nat_setup_info(ct, &range, hooknum);

                DEBUGP("NAT status after setup_info: %lu\n",
                       ct->status & (IPS_NAT_MASK | IPS_NAT_DONE_MASK));
#endif
	}

	/* Be careful here, modifying NAT bits can screw up things,
	 * so don't let users modify them directly if they don't pass
	 * ip_nat_range. */
	ct->status |= status & ~(IPS_NAT_DONE_MASK | IPS_NAT_MASK);
	return 0;
}


static inline int
ctnetlink_change_helper(struct ip_conntrack *ct, struct nfattr *cda[])
{
	struct ip_conntrack_helper *helper;
	char *helpname;
	int err;

	DEBUGP("entered %s\n", __FUNCTION__);

	/* don't change helper of sibling connections */
	if (ct->master)
		return -EINVAL;

	err = ctnetlink_parse_help(cda[CTA_HELP-1], &helpname);
	if (err < 0)
		return err;

	helper = __ip_conntrack_helper_find_byname(helpname);
	if (!helper) {
		if (!strcmp(helpname, ""))
			helper = NULL;
		else
			return -EINVAL;
	}

	if (ct->helper) {
		if (!helper) {
			/* we had a helper before ... */
			ip_ct_remove_expectations(ct);
			ct->helper = NULL;
		} else {
			/* need to zero data of old helper */
			memset(&ct->help, 0, sizeof(ct->help));
		}
	}
	
	ct->helper = helper;

	return 0;
}

static inline int
ctnetlink_change_timeout(struct ip_conntrack *ct, struct nfattr *cda[])
{
	u_int32_t timeout = ntohl(*(u_int32_t *)NFA_DATA(cda[CTA_TIMEOUT-1]));
	
	if (!del_timer(&ct->timeout))
		return -ETIME;

	ct->timeout.expires = jiffies + timeout * HZ;
	add_timer(&ct->timeout);

	return 0;
}

static int
ctnetlink_change_conntrack(struct ip_conntrack *ct, struct nfattr *cda[])
{
	int err;

	DEBUGP("entered %s\n", __FUNCTION__);

	if (cda[CTA_HELP-1]) {
		err = ctnetlink_change_helper(ct, cda);
		if (err < 0)
			return err;
	}

	if (cda[CTA_TIMEOUT-1]) {
		err = ctnetlink_change_timeout(ct, cda);
		if (err < 0)
			return err;
	}

	if (cda[CTA_STATUS-1]) {
		err = ctnetlink_change_status(ct, cda);
		if (err < 0)
			return err;
	}

	DEBUGP("all done\n");
	return 0;
}

static int
ctnetlink_create_conntrack(struct nfattr *cda[], 
			   struct ip_conntrack_tuple *otuple,
			   struct ip_conntrack_tuple *rtuple)
{
	struct ip_conntrack *ct;
	int err = -EINVAL;

	DEBUGP("entered %s\n", __FUNCTION__);

	ct = ip_conntrack_alloc(otuple, rtuple);
	if (ct == NULL || IS_ERR(ct))
		return -ENOMEM;	

	if (!cda[CTA_TIMEOUT-1])
		goto err;
	ct->timeout.expires = ntohl(*(u_int32_t *)NFA_DATA(cda[CTA_TIMEOUT-1]));

	ct->timeout.expires = jiffies + ct->timeout.expires * HZ;
	ct->status |= IPS_CONFIRMED;

	err = ctnetlink_change_status(ct, cda);
	if (err < 0)
		goto err;

	ct->helper = ip_conntrack_helper_find_get(rtuple);

	add_timer(&ct->timeout);
	ip_conntrack_hash_insert(ct);

	if (ct->helper)
		ip_conntrack_helper_put(ct->helper);

	DEBUGP("conntrack with id %u inserted\n", ct->id);
	return 0;

err:	
	ip_conntrack_free(ct);
	return err;
}

static int 
ctnetlink_new_conntrack(struct sock *ctnl, struct sk_buff *skb, 
			struct nlmsghdr *nlh, struct nfattr *cda[], int *errp)
{
	struct ip_conntrack_tuple otuple, rtuple;
	struct ip_conntrack_tuple_hash *h = NULL;
	int err = 0;

	DEBUGP("entered %s\n", __FUNCTION__);

	if (cda[CTA_TUPLE_ORIG-1]) {
		err = ctnetlink_parse_tuple(cda, &otuple, CTA_TUPLE_ORIG);
		if (err < 0)
			return err;
	}

	if (cda[CTA_TUPLE_REPLY-1]) {
		err = ctnetlink_parse_tuple(cda, &rtuple, CTA_TUPLE_REPLY);
		if (err < 0)
			return err;
	}

	write_lock_bh(&ip_conntrack_lock);
	if (cda[CTA_TUPLE_ORIG-1])
		h = __ip_conntrack_find(&otuple, NULL);
	else if (cda[CTA_TUPLE_REPLY-1])
		h = __ip_conntrack_find(&rtuple, NULL);

	if (h == NULL) {
		write_unlock_bh(&ip_conntrack_lock);
		DEBUGP("no such conntrack, create new\n");
		err = -ENOENT;
		if (nlh->nlmsg_flags & NLM_F_CREATE)
			err = ctnetlink_create_conntrack(cda, &otuple, &rtuple);
		return err;
	}
	/* implicit 'else' */

	/* we only allow nat config for new conntracks */
	if (cda[CTA_NAT-1]) {
		err = -EINVAL;
		goto out_unlock;
	}

	/* We manipulate the conntrack inside the global conntrack table lock,
	 * so there's no need to increase the refcount */
	DEBUGP("conntrack found\n");
	err = -EEXIST;
	if (!(nlh->nlmsg_flags & NLM_F_EXCL))
		err = ctnetlink_change_conntrack(tuplehash_to_ctrack(h), cda);

out_unlock:
	write_unlock_bh(&ip_conntrack_lock);
	return err;
}

/*********************************************************************** 
 * EXPECT 
 ***********************************************************************/ 

static inline int
ctnetlink_exp_dump_tuple(struct sk_buff *skb,
			 const struct ip_conntrack_tuple *tuple,
			 enum ctattr_expect type)
{
	struct nfattr *nest_parms = NFA_NEST(skb, type);
	
	if (ctnetlink_dump_tuples(skb, tuple) < 0)
		goto nfattr_failure;

	NFA_NEST_END(skb, nest_parms);

	return 0;

nfattr_failure:
	return -1;
}			

static inline int
ctnetlink_exp_dump_expect(struct sk_buff *skb,
                          const struct ip_conntrack_expect *exp)
{
	struct ip_conntrack *master = exp->master;
	u_int32_t timeout = htonl((exp->timeout.expires - jiffies) / HZ);
	u_int32_t id = htonl(exp->id);

	if (ctnetlink_exp_dump_tuple(skb, &exp->tuple, CTA_EXPECT_TUPLE) < 0)
		goto nfattr_failure;
	if (ctnetlink_exp_dump_tuple(skb, &exp->mask, CTA_EXPECT_MASK) < 0)
		goto nfattr_failure;
	if (ctnetlink_exp_dump_tuple(skb,
				 &master->tuplehash[IP_CT_DIR_ORIGINAL].tuple,
				 CTA_EXPECT_MASTER) < 0)
		goto nfattr_failure;
	
	NFA_PUT(skb, CTA_EXPECT_TIMEOUT, sizeof(timeout), &timeout);
	NFA_PUT(skb, CTA_EXPECT_ID, sizeof(u_int32_t), &id);

	return 0;
	
nfattr_failure:
	return -1;
}

static int
ctnetlink_exp_fill_info(struct sk_buff *skb, u32 pid, u32 seq,
		    int event, 
		    int nowait, 
		    const struct ip_conntrack_expect *exp)
{
	struct nlmsghdr *nlh;
	struct nfgenmsg *nfmsg;
	unsigned char *b;

	b = skb->tail;

	event |= NFNL_SUBSYS_CTNETLINK_EXP << 8;
	nlh    = NLMSG_PUT(skb, pid, seq, event, sizeof(struct nfgenmsg));
	nfmsg  = NLMSG_DATA(nlh);

	nlh->nlmsg_flags    = (nowait && pid) ? NLM_F_MULTI : 0;
	nfmsg->nfgen_family = AF_INET;
	nfmsg->version	    = NFNETLINK_V0;
	nfmsg->res_id	    = 0;

	if (ctnetlink_exp_dump_expect(skb, exp) < 0)
		goto nfattr_failure;

	nlh->nlmsg_len = skb->tail - b;
	return skb->len;

nlmsg_failure:
nfattr_failure:
	skb_trim(skb, b - skb->data);
	return -1;
}

#ifdef CONFIG_IP_NF_CONNTRACK_EVENTS
static int ctnetlink_expect_event(struct notifier_block *this,
				  unsigned long events, void *ptr)
{
	struct nlmsghdr *nlh;
	struct nfgenmsg *nfmsg;
	struct ip_conntrack_expect *exp = (struct ip_conntrack_expect *)ptr;
	struct sk_buff *skb;
	unsigned int type;
	unsigned char *b;
	int flags = 0;
	u16 proto;

	if (events & IPEXP_NEW) {
		type = IPCTNL_MSG_EXP_NEW;
		flags = NLM_F_CREATE|NLM_F_EXCL;
	} else
		return NOTIFY_DONE;

	skb = alloc_skb(NLMSG_GOODSIZE, GFP_ATOMIC);
	if (!skb)
		return NOTIFY_DONE;

	b = skb->tail;

	type |= NFNL_SUBSYS_CTNETLINK << 8;
	nlh   = NLMSG_PUT(skb, 0, 0, type, sizeof(struct nfgenmsg));
	nfmsg = NLMSG_DATA(nlh);

	nlh->nlmsg_flags    = flags;
	nfmsg->nfgen_family = AF_INET;
	nfmsg->version	    = NFNETLINK_V0;
	nfmsg->res_id	    = 0;

	if (ctnetlink_exp_dump_expect(skb, exp) < 0)
		goto nfattr_failure;

	nlh->nlmsg_len = skb->tail - b;
	proto = exp->tuple.dst.protonum;
	nfnetlink_send(skb, 0, NF_NETLINK_CONNTRACK_EXP_NEW, 0);
	return NOTIFY_DONE;

nlmsg_failure:
nfattr_failure:
	kfree_skb(skb);
	return NOTIFY_DONE;
}
#endif

static int
ctnetlink_exp_dump_table(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct ip_conntrack_expect *exp = NULL;
	struct list_head *i;
	u_int32_t *id = (u_int32_t *) &cb->args[0];

	DEBUGP("entered %s, last id=%llu\n", __FUNCTION__, *id);

	read_lock_bh(&ip_conntrack_lock);
	list_for_each(i, &ip_conntrack_expect_list) {
		exp = (struct ip_conntrack_expect *) i;
		if (exp->id <= *id)
			continue;
		if (ctnetlink_exp_fill_info(skb, NETLINK_CB(cb->skb).pid,
					    cb->nlh->nlmsg_seq,
					    IPCTNL_MSG_EXP_NEW,
					    1, exp) < 0)
			goto out;
		*id = exp->id;
	}
out:	
	read_unlock_bh(&ip_conntrack_lock);

	DEBUGP("leaving, last id=%llu\n", *id);

	return skb->len;
}

static int
ctnetlink_get_expect(struct sock *ctnl, struct sk_buff *skb, 
		     struct nlmsghdr *nlh, struct nfattr *cda[], int *errp)
{
	struct ip_conntrack_tuple tuple;
	struct ip_conntrack_expect *exp;
	struct sk_buff *skb2;
	int err = 0;

	DEBUGP("entered %s\n", __FUNCTION__);

	if (nlh->nlmsg_flags & NLM_F_DUMP) {
		struct nfgenmsg *msg = NLMSG_DATA(nlh);
		u32 rlen;

		if (msg->nfgen_family != AF_INET)
			return -EAFNOSUPPORT;

		if ((*errp = netlink_dump_start(ctnl, skb, nlh,
		    				ctnetlink_exp_dump_table,
						ctnetlink_done)) != 0)
			return -EINVAL;
		rlen = NLMSG_ALIGN(nlh->nlmsg_len);
		if (rlen > skb->len)
			rlen = skb->len;
		skb_pull(skb, rlen);
		return 0;
	}

	if (cda[CTA_EXPECT_MASTER-1])
		err = ctnetlink_parse_tuple(cda, &tuple, CTA_EXPECT_MASTER);
	else
		return -EINVAL;

	if (err < 0)
		return err;

	exp = ip_conntrack_expect_find_get(&tuple);
	if (!exp)
		return -ENOENT;

	err = -ENOMEM;
	skb2 = alloc_skb(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!skb2)
		goto out;
	NETLINK_CB(skb2).dst_pid = NETLINK_CB(skb).pid;
	
	err = ctnetlink_exp_fill_info(skb2, NETLINK_CB(skb).pid, 
				      nlh->nlmsg_seq, IPCTNL_MSG_EXP_NEW,
				      1, exp);
	if (err <= 0)
		goto out;

	ip_conntrack_expect_put(exp);

	err = netlink_unicast(ctnl, skb2, NETLINK_CB(skb).pid, MSG_DONTWAIT);
	if (err < 0)
		goto free;

	return err;

out:
	ip_conntrack_expect_put(exp);
free:
	if (skb2)
		kfree_skb(skb2);
	return err;
}

static int
ctnetlink_del_expect(struct sock *ctnl, struct sk_buff *skb, 
		     struct nlmsghdr *nlh, struct nfattr *cda[], int *errp)
{
	struct ip_conntrack_expect *exp, *tmp;
	struct ip_conntrack_tuple tuple;
	struct ip_conntrack_helper *h;
	int err;

	if (cda[CTA_EXPECT_TUPLE-1]) {
		/* delete a single expect by tuple */
		err = ctnetlink_parse_tuple(cda, &tuple, CTA_EXPECT_TUPLE);
		if (err < 0)
			return err;

		/* bump usage count to 2 */
		exp = ip_conntrack_expect_find_get(&tuple);
		if (!exp)
			return -ENOENT;

		if (cda[CTA_EXPECT_ID-1]) {
			u_int32_t id = 
				*(u_int32_t *)NFA_DATA(cda[CTA_EXPECT_ID-1]);
			if (exp->id != ntohl(id)) {
				ip_conntrack_expect_put(exp);
				return -ENOENT;
			}
		}

		/* after list removal, usage count == 1 */
		ip_conntrack_unexpect_related(exp);
		/* have to put what we 'get' above. 
		 * after this line usage count == 0 */
		ip_conntrack_expect_put(exp);
	} else if (cda[CTA_EXPECT_HELP_NAME-1]) {
		char *name = NFA_DATA(cda[CTA_EXPECT_HELP_NAME-1]);

		/* delete all expectations for this helper */
		write_lock_bh(&ip_conntrack_lock);
		h = __ip_conntrack_helper_find_byname(name);
		if (!h) {
			write_unlock_bh(&ip_conntrack_lock);
			return -EINVAL;
		}
		list_for_each_entry_safe(exp, tmp, &ip_conntrack_expect_list,
					 list) {
			if (exp->master->helper == h 
			    && del_timer(&exp->timeout))
				__ip_ct_expect_unlink_destroy(exp);
		}
		write_unlock(&ip_conntrack_lock);
	} else {
		/* This basically means we have to flush everything*/
		write_lock_bh(&ip_conntrack_lock);
		list_for_each_entry_safe(exp, tmp, &ip_conntrack_expect_list,
					 list) {
			if (del_timer(&exp->timeout))
				__ip_ct_expect_unlink_destroy(exp);
		}
		write_unlock_bh(&ip_conntrack_lock);
	}

	return 0;
}
static int
ctnetlink_change_expect(struct ip_conntrack_expect *x, struct nfattr *cda[])
{
	return -EOPNOTSUPP;
}

static int
ctnetlink_create_expect(struct nfattr *cda[])
{
	struct ip_conntrack_tuple tuple, mask, master_tuple;
	struct ip_conntrack_tuple_hash *h = NULL;
	struct ip_conntrack_expect *exp;
	struct ip_conntrack *ct;
	int err = 0;

	DEBUGP("entered %s\n", __FUNCTION__);

	/* caller guarantees that those three CTA_EXPECT_* exist */
	err = ctnetlink_parse_tuple(cda, &tuple, CTA_EXPECT_TUPLE);
	if (err < 0)
		return err;
	err = ctnetlink_parse_tuple(cda, &mask, CTA_EXPECT_MASK);
	if (err < 0)
		return err;
	err = ctnetlink_parse_tuple(cda, &master_tuple, CTA_EXPECT_MASTER);
	if (err < 0)
		return err;

	/* Look for master conntrack of this expectation */
	h = ip_conntrack_find_get(&master_tuple, NULL);
	if (!h)
		return -ENOENT;
	ct = tuplehash_to_ctrack(h);

	if (!ct->helper) {
		/* such conntrack hasn't got any helper, abort */
		err = -EINVAL;
		goto out;
	}

	exp = ip_conntrack_expect_alloc(ct);
	if (!exp) {
		err = -ENOMEM;
		goto out;
	}
	
	exp->expectfn = NULL;
	exp->master = ct;
	memcpy(&exp->tuple, &tuple, sizeof(struct ip_conntrack_tuple));
	memcpy(&exp->mask, &mask, sizeof(struct ip_conntrack_tuple));

	err = ip_conntrack_expect_related(exp);
	ip_conntrack_expect_put(exp);

out:	
	ip_conntrack_put(tuplehash_to_ctrack(h));
	return err;
}

static int
ctnetlink_new_expect(struct sock *ctnl, struct sk_buff *skb,
		     struct nlmsghdr *nlh, struct nfattr *cda[], int *errp)
{
	struct ip_conntrack_tuple tuple;
	struct ip_conntrack_expect *exp;
	int err = 0;

	DEBUGP("entered %s\n", __FUNCTION__);	

	if (!cda[CTA_EXPECT_TUPLE-1]
	    || !cda[CTA_EXPECT_MASK-1]
	    || !cda[CTA_EXPECT_MASTER-1])
		return -EINVAL;

	err = ctnetlink_parse_tuple(cda, &tuple, CTA_EXPECT_TUPLE);
	if (err < 0)
		return err;

	write_lock_bh(&ip_conntrack_lock);
	exp = __ip_conntrack_expect_find(&tuple);

	if (!exp) {
		write_unlock_bh(&ip_conntrack_lock);
		err = -ENOENT;
		if (nlh->nlmsg_flags & NLM_F_CREATE)
			err = ctnetlink_create_expect(cda);
		return err;
	}

	err = -EEXIST;
	if (!(nlh->nlmsg_flags & NLM_F_EXCL))
		err = ctnetlink_change_expect(exp, cda);
	write_unlock_bh(&ip_conntrack_lock);

	DEBUGP("leaving\n");
	
	return err;
}

#ifdef CONFIG_IP_NF_CONNTRACK_EVENTS
static struct notifier_block ctnl_notifier = {
	.notifier_call	= ctnetlink_conntrack_event,
};

static struct notifier_block ctnl_notifier_exp = {
	.notifier_call	= ctnetlink_expect_event,
};
#endif

static struct nfnl_callback ctnl_cb[IPCTNL_MSG_MAX] = {
	[IPCTNL_MSG_CT_NEW]		= { .call = ctnetlink_new_conntrack,
					    .attr_count = CTA_MAX,
					    .cap_required = CAP_NET_ADMIN },
	[IPCTNL_MSG_CT_GET] 		= { .call = ctnetlink_get_conntrack,
					    .attr_count = CTA_MAX,
					    .cap_required = CAP_NET_ADMIN },
	[IPCTNL_MSG_CT_DELETE]  	= { .call = ctnetlink_del_conntrack,
					    .attr_count = CTA_MAX,
					    .cap_required = CAP_NET_ADMIN },
	[IPCTNL_MSG_CT_GET_CTRZERO] 	= { .call = ctnetlink_get_conntrack,
					    .attr_count = CTA_MAX,
					    .cap_required = CAP_NET_ADMIN },
};

static struct nfnl_callback ctnl_exp_cb[IPCTNL_MSG_EXP_MAX] = {
	[IPCTNL_MSG_EXP_GET]		= { .call = ctnetlink_get_expect,
					    .attr_count = CTA_EXPECT_MAX,
					    .cap_required = CAP_NET_ADMIN },
	[IPCTNL_MSG_EXP_NEW]		= { .call = ctnetlink_new_expect,
					    .attr_count = CTA_EXPECT_MAX,
					    .cap_required = CAP_NET_ADMIN },
	[IPCTNL_MSG_EXP_DELETE]		= { .call = ctnetlink_del_expect,
					    .attr_count = CTA_EXPECT_MAX,
					    .cap_required = CAP_NET_ADMIN },
};

static struct nfnetlink_subsystem ctnl_subsys = {
	.name				= "conntrack",
	.subsys_id			= NFNL_SUBSYS_CTNETLINK,
	.cb_count			= IPCTNL_MSG_MAX,
	.cb				= ctnl_cb,
};

static struct nfnetlink_subsystem ctnl_exp_subsys = {
	.name				= "conntrack_expect",
	.subsys_id			= NFNL_SUBSYS_CTNETLINK_EXP,
	.cb_count			= IPCTNL_MSG_EXP_MAX,
	.cb				= ctnl_exp_cb,
};

static int __init ctnetlink_init(void)
{
	int ret;

	printk("ctnetlink v%s: registering with nfnetlink.\n", version);
	ret = nfnetlink_subsys_register(&ctnl_subsys);
	if (ret < 0) {
		printk("ctnetlink_init: cannot register with nfnetlink.\n");
		goto err_out;
	}

	ret = nfnetlink_subsys_register(&ctnl_exp_subsys);
	if (ret < 0) {
		printk("ctnetlink_init: cannot register exp with nfnetlink.\n");
		goto err_unreg_subsys;
	}

#ifdef CONFIG_IP_NF_CONNTRACK_EVENTS
	ret = ip_conntrack_register_notifier(&ctnl_notifier);
	if (ret < 0) {
		printk("ctnetlink_init: cannot register notifier.\n");
		goto err_unreg_exp_subsys;
	}

	ret = ip_conntrack_expect_register_notifier(&ctnl_notifier_exp);
	if (ret < 0) {
		printk("ctnetlink_init: cannot expect register notifier.\n");
		goto err_unreg_notifier;
	}
#endif

	return 0;

#ifdef CONFIG_IP_NF_CONNTRACK_EVENTS
err_unreg_notifier:
	ip_conntrack_unregister_notifier(&ctnl_notifier);
err_unreg_exp_subsys:
	nfnetlink_subsys_unregister(&ctnl_exp_subsys);
#endif
err_unreg_subsys:
	nfnetlink_subsys_unregister(&ctnl_subsys);
err_out:
	return ret;
}

static void __exit ctnetlink_exit(void)
{
	printk("ctnetlink: unregistering from nfnetlink.\n");

#ifdef CONFIG_IP_NF_CONNTRACK_EVENTS
	ip_conntrack_unregister_notifier(&ctnl_notifier_exp);
	ip_conntrack_unregister_notifier(&ctnl_notifier);
#endif

	nfnetlink_subsys_unregister(&ctnl_exp_subsys);
	nfnetlink_subsys_unregister(&ctnl_subsys);
	return;
}

module_init(ctnetlink_init);
module_exit(ctnetlink_exit);
