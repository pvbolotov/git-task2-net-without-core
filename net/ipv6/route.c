/*
 *	Linux INET6 implementation
 *	FIB front-end.
 *
 *	Authors:
 *	Pedro Roque		<roque@di.fc.ul.pt>	
 *
 *	$Id: route.c,v 1.56 2001/10/31 21:55:55 davem Exp $
 *
 *	This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

/*	Changes:
 *
 *	YOSHIFUJI Hideaki @USAGI
 *		reworked default router selection.
 *		- respect outgoing interface
 *		- select from (probably) reachable routers (i.e.
 *		routers in REACHABLE, STALE, DELAY or PROBE states).
 *		- always select the same router if it is (probably)
 *		reachable.  otherwise, round-robin the list.
 *	Ville Nuorvala
 *		Fixed routing subtrees.
 */

#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/times.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <linux/route.h>
#include <linux/netdevice.h>
#include <linux/in6.h>
#include <linux/init.h>
#include <linux/if_arp.h>

#ifdef 	CONFIG_PROC_FS
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#endif

#include <net/snmp.h>
#include <net/ipv6.h>
#include <net/ip6_fib.h>
#include <net/ip6_route.h>
#include <net/ndisc.h>
#include <net/addrconf.h>
#include <net/tcp.h>
#include <linux/rtnetlink.h>
#include <net/dst.h>
#include <net/xfrm.h>
#include <net/netevent.h>
#include <net/netlink.h>

#include <asm/uaccess.h>

#ifdef CONFIG_SYSCTL
#include <linux/sysctl.h>
#endif

/* Set to 3 to get tracing. */
#define RT6_DEBUG 2

#if RT6_DEBUG >= 3
#define RDBG(x) printk x
#define RT6_TRACE(x...) printk(KERN_DEBUG x)
#else
#define RDBG(x)
#define RT6_TRACE(x...) do { ; } while (0)
#endif

#define CLONE_OFFLINK_ROUTE 0

static int ip6_rt_max_size = 4096;
static int ip6_rt_gc_min_interval = HZ / 2;
static int ip6_rt_gc_timeout = 60*HZ;
int ip6_rt_gc_interval = 30*HZ;
static int ip6_rt_gc_elasticity = 9;
static int ip6_rt_mtu_expires = 10*60*HZ;
static int ip6_rt_min_advmss = IPV6_MIN_MTU - 20 - 40;

static struct rt6_info * ip6_rt_copy(struct rt6_info *ort);
static struct dst_entry	*ip6_dst_check(struct dst_entry *dst, u32 cookie);
static struct dst_entry *ip6_negative_advice(struct dst_entry *);
static void		ip6_dst_destroy(struct dst_entry *);
static void		ip6_dst_ifdown(struct dst_entry *,
				       struct net_device *dev, int how);
static int		 ip6_dst_gc(void);

static int		ip6_pkt_discard(struct sk_buff *skb);
static int		ip6_pkt_discard_out(struct sk_buff *skb);
static void		ip6_link_failure(struct sk_buff *skb);
static void		ip6_rt_update_pmtu(struct dst_entry *dst, u32 mtu);

#ifdef CONFIG_IPV6_ROUTE_INFO
static struct rt6_info *rt6_add_route_info(struct in6_addr *prefix, int prefixlen,
					   struct in6_addr *gwaddr, int ifindex,
					   unsigned pref);
static struct rt6_info *rt6_get_route_info(struct in6_addr *prefix, int prefixlen,
					   struct in6_addr *gwaddr, int ifindex);
#endif

static struct dst_ops ip6_dst_ops = {
	.family			=	AF_INET6,
	.protocol		=	__constant_htons(ETH_P_IPV6),
	.gc			=	ip6_dst_gc,
	.gc_thresh		=	1024,
	.check			=	ip6_dst_check,
	.destroy		=	ip6_dst_destroy,
	.ifdown			=	ip6_dst_ifdown,
	.negative_advice	=	ip6_negative_advice,
	.link_failure		=	ip6_link_failure,
	.update_pmtu		=	ip6_rt_update_pmtu,
	.entry_size		=	sizeof(struct rt6_info),
};

struct rt6_info ip6_null_entry = {
	.u = {
		.dst = {
			.__refcnt	= ATOMIC_INIT(1),
			.__use		= 1,
			.dev		= &loopback_dev,
			.obsolete	= -1,
			.error		= -ENETUNREACH,
			.metrics	= { [RTAX_HOPLIMIT - 1] = 255, },
			.input		= ip6_pkt_discard,
			.output		= ip6_pkt_discard_out,
			.ops		= &ip6_dst_ops,
			.path		= (struct dst_entry*)&ip6_null_entry,
		}
	},
	.rt6i_flags	= (RTF_REJECT | RTF_NONEXTHOP),
	.rt6i_metric	= ~(u32) 0,
	.rt6i_ref	= ATOMIC_INIT(1),
};

#ifdef CONFIG_IPV6_MULTIPLE_TABLES

static int ip6_pkt_prohibit(struct sk_buff *skb);
static int ip6_pkt_prohibit_out(struct sk_buff *skb);
static int ip6_pkt_blk_hole(struct sk_buff *skb);

struct rt6_info ip6_prohibit_entry = {
	.u = {
		.dst = {
			.__refcnt	= ATOMIC_INIT(1),
			.__use		= 1,
			.dev		= &loopback_dev,
			.obsolete	= -1,
			.error		= -EACCES,
			.metrics	= { [RTAX_HOPLIMIT - 1] = 255, },
			.input		= ip6_pkt_prohibit,
			.output		= ip6_pkt_prohibit_out,
			.ops		= &ip6_dst_ops,
			.path		= (struct dst_entry*)&ip6_prohibit_entry,
		}
	},
	.rt6i_flags	= (RTF_REJECT | RTF_NONEXTHOP),
	.rt6i_metric	= ~(u32) 0,
	.rt6i_ref	= ATOMIC_INIT(1),
};

struct rt6_info ip6_blk_hole_entry = {
	.u = {
		.dst = {
			.__refcnt	= ATOMIC_INIT(1),
			.__use		= 1,
			.dev		= &loopback_dev,
			.obsolete	= -1,
			.error		= -EINVAL,
			.metrics	= { [RTAX_HOPLIMIT - 1] = 255, },
			.input		= ip6_pkt_blk_hole,
			.output		= ip6_pkt_blk_hole,
			.ops		= &ip6_dst_ops,
			.path		= (struct dst_entry*)&ip6_blk_hole_entry,
		}
	},
	.rt6i_flags	= (RTF_REJECT | RTF_NONEXTHOP),
	.rt6i_metric	= ~(u32) 0,
	.rt6i_ref	= ATOMIC_INIT(1),
};

#endif

/* allocate dst with ip6_dst_ops */
static __inline__ struct rt6_info *ip6_dst_alloc(void)
{
	return (struct rt6_info *)dst_alloc(&ip6_dst_ops);
}

static void ip6_dst_destroy(struct dst_entry *dst)
{
	struct rt6_info *rt = (struct rt6_info *)dst;
	struct inet6_dev *idev = rt->rt6i_idev;

	if (idev != NULL) {
		rt->rt6i_idev = NULL;
		in6_dev_put(idev);
	}	
}

static void ip6_dst_ifdown(struct dst_entry *dst, struct net_device *dev,
			   int how)
{
	struct rt6_info *rt = (struct rt6_info *)dst;
	struct inet6_dev *idev = rt->rt6i_idev;

	if (dev != &loopback_dev && idev != NULL && idev->dev == dev) {
		struct inet6_dev *loopback_idev = in6_dev_get(&loopback_dev);
		if (loopback_idev != NULL) {
			rt->rt6i_idev = loopback_idev;
			in6_dev_put(idev);
		}
	}
}

static __inline__ int rt6_check_expired(const struct rt6_info *rt)
{
	return (rt->rt6i_flags & RTF_EXPIRES &&
		time_after(jiffies, rt->rt6i_expires));
}

static inline int rt6_need_strict(struct in6_addr *daddr)
{
	return (ipv6_addr_type(daddr) &
		(IPV6_ADDR_MULTICAST | IPV6_ADDR_LINKLOCAL));
}

/*
 *	Route lookup. Any table->tb6_lock is implied.
 */

static __inline__ struct rt6_info *rt6_device_match(struct rt6_info *rt,
						    int oif,
						    int strict)
{
	struct rt6_info *local = NULL;
	struct rt6_info *sprt;

	if (oif) {
		for (sprt = rt; sprt; sprt = sprt->u.next) {
			struct net_device *dev = sprt->rt6i_dev;
			if (dev->ifindex == oif)
				return sprt;
			if (dev->flags & IFF_LOOPBACK) {
				if (sprt->rt6i_idev == NULL ||
				    sprt->rt6i_idev->dev->ifindex != oif) {
					if (strict && oif)
						continue;
					if (local && (!oif || 
						      local->rt6i_idev->dev->ifindex == oif))
						continue;
				}
				local = sprt;
			}
		}

		if (local)
			return local;

		if (strict)
			return &ip6_null_entry;
	}
	return rt;
}

#ifdef CONFIG_IPV6_ROUTER_PREF
static void rt6_probe(struct rt6_info *rt)
{
	struct neighbour *neigh = rt ? rt->rt6i_nexthop : NULL;
	/*
	 * Okay, this does not seem to be appropriate
	 * for now, however, we need to check if it
	 * is really so; aka Router Reachability Probing.
	 *
	 * Router Reachability Probe MUST be rate-limited
	 * to no more than one per minute.
	 */
	if (!neigh || (neigh->nud_state & NUD_VALID))
		return;
	read_lock_bh(&neigh->lock);
	if (!(neigh->nud_state & NUD_VALID) &&
	    time_after(jiffies, neigh->updated + rt->rt6i_idev->cnf.rtr_probe_interval)) {
		struct in6_addr mcaddr;
		struct in6_addr *target;

		neigh->updated = jiffies;
		read_unlock_bh(&neigh->lock);

		target = (struct in6_addr *)&neigh->primary_key;
		addrconf_addr_solict_mult(target, &mcaddr);
		ndisc_send_ns(rt->rt6i_dev, NULL, target, &mcaddr, NULL);
	} else
		read_unlock_bh(&neigh->lock);
}
#else
static inline void rt6_probe(struct rt6_info *rt)
{
	return;
}
#endif

/*
 * Default Router Selection (RFC 2461 6.3.6)
 */
static int inline rt6_check_dev(struct rt6_info *rt, int oif)
{
	struct net_device *dev = rt->rt6i_dev;
	if (!oif || dev->ifindex == oif)
		return 2;
	if ((dev->flags & IFF_LOOPBACK) &&
	    rt->rt6i_idev && rt->rt6i_idev->dev->ifindex == oif)
		return 1;
	return 0;
}

static int inline rt6_check_neigh(struct rt6_info *rt)
{
	struct neighbour *neigh = rt->rt6i_nexthop;
	int m = 0;
	if (rt->rt6i_flags & RTF_NONEXTHOP ||
	    !(rt->rt6i_flags & RTF_GATEWAY))
		m = 1;
	else if (neigh) {
		read_lock_bh(&neigh->lock);
		if (neigh->nud_state & NUD_VALID)
			m = 2;
		else if (!(neigh->nud_state & NUD_FAILED))
			m = 1;
		read_unlock_bh(&neigh->lock);
	}
	return m;
}

static int rt6_score_route(struct rt6_info *rt, int oif,
			   int strict)
{
	int m, n;
		
	m = rt6_check_dev(rt, oif);
	if (!m && (strict & RT6_LOOKUP_F_IFACE))
		return -1;
#ifdef CONFIG_IPV6_ROUTER_PREF
	m |= IPV6_DECODE_PREF(IPV6_EXTRACT_PREF(rt->rt6i_flags)) << 2;
#endif
	n = rt6_check_neigh(rt);
	if (!n && (strict & RT6_LOOKUP_F_REACHABLE))
		return -1;
	return m;
}

static struct rt6_info *rt6_select(struct rt6_info **head, int oif,
				   int strict)
{
	struct rt6_info *match = NULL, *last = NULL;
	struct rt6_info *rt, *rt0 = *head;
	u32 metric;
	int mpri = -1;

	RT6_TRACE("%s(head=%p(*head=%p), oif=%d)\n",
		  __FUNCTION__, head, head ? *head : NULL, oif);

	for (rt = rt0, metric = rt0->rt6i_metric;
	     rt && rt->rt6i_metric == metric && (!last || rt != rt0);
	     rt = rt->u.next) {
		int m;

		if (rt6_check_expired(rt))
			continue;

		last = rt;

		m = rt6_score_route(rt, oif, strict);
		if (m < 0)
			continue;

		if (m > mpri) {
			if (strict & RT6_LOOKUP_F_REACHABLE)
				rt6_probe(match);
			match = rt;
			mpri = m;
		} else if (strict & RT6_LOOKUP_F_REACHABLE) {
			rt6_probe(rt);
		}
	}

	if (!match &&
	    (strict & RT6_LOOKUP_F_REACHABLE) &&
	    last && last != rt0) {
		/* no entries matched; do round-robin */
		static DEFINE_SPINLOCK(lock);
		spin_lock(&lock);
		*head = rt0->u.next;
		rt0->u.next = last->u.next;
		last->u.next = rt0;
		spin_unlock(&lock);
	}

	RT6_TRACE("%s() => %p, score=%d\n",
		  __FUNCTION__, match, mpri);

	return (match ? match : &ip6_null_entry);
}

#ifdef CONFIG_IPV6_ROUTE_INFO
int rt6_route_rcv(struct net_device *dev, u8 *opt, int len,
		  struct in6_addr *gwaddr)
{
	struct route_info *rinfo = (struct route_info *) opt;
	struct in6_addr prefix_buf, *prefix;
	unsigned int pref;
	u32 lifetime;
	struct rt6_info *rt;

	if (len < sizeof(struct route_info)) {
		return -EINVAL;
	}

	/* Sanity check for prefix_len and length */
	if (rinfo->length > 3) {
		return -EINVAL;
	} else if (rinfo->prefix_len > 128) {
		return -EINVAL;
	} else if (rinfo->prefix_len > 64) {
		if (rinfo->length < 2) {
			return -EINVAL;
		}
	} else if (rinfo->prefix_len > 0) {
		if (rinfo->length < 1) {
			return -EINVAL;
		}
	}

	pref = rinfo->route_pref;
	if (pref == ICMPV6_ROUTER_PREF_INVALID)
		pref = ICMPV6_ROUTER_PREF_MEDIUM;

	lifetime = ntohl(rinfo->lifetime);
	if (lifetime == 0xffffffff) {
		/* infinity */
	} else if (lifetime > 0x7fffffff/HZ) {
		/* Avoid arithmetic overflow */
		lifetime = 0x7fffffff/HZ - 1;
	}

	if (rinfo->length == 3)
		prefix = (struct in6_addr *)rinfo->prefix;
	else {
		/* this function is safe */
		ipv6_addr_prefix(&prefix_buf,
				 (struct in6_addr *)rinfo->prefix,
				 rinfo->prefix_len);
		prefix = &prefix_buf;
	}

	rt = rt6_get_route_info(prefix, rinfo->prefix_len, gwaddr, dev->ifindex);

	if (rt && !lifetime) {
		ip6_del_rt(rt);
		rt = NULL;
	}

	if (!rt && lifetime)
		rt = rt6_add_route_info(prefix, rinfo->prefix_len, gwaddr, dev->ifindex,
					pref);
	else if (rt)
		rt->rt6i_flags = RTF_ROUTEINFO |
				 (rt->rt6i_flags & ~RTF_PREF_MASK) | RTF_PREF(pref);

	if (rt) {
		if (lifetime == 0xffffffff) {
			rt->rt6i_flags &= ~RTF_EXPIRES;
		} else {
			rt->rt6i_expires = jiffies + HZ * lifetime;
			rt->rt6i_flags |= RTF_EXPIRES;
		}
		dst_release(&rt->u.dst);
	}
	return 0;
}
#endif

#define BACKTRACK(saddr) \
do { \
	if (rt == &ip6_null_entry) { \
		struct fib6_node *pn; \
		while (1) { \
			if (fn->fn_flags & RTN_TL_ROOT) \
				goto out; \
			pn = fn->parent; \
			if (FIB6_SUBTREE(pn) && FIB6_SUBTREE(pn) != fn) \
				fn = fib6_lookup(pn->subtree, NULL, saddr); \
			else \
				fn = pn; \
			if (fn->fn_flags & RTN_RTINFO) \
				goto restart; \
		} \
	} \
} while(0)

static struct rt6_info *ip6_pol_route_lookup(struct fib6_table *table,
					     struct flowi *fl, int flags)
{
	struct fib6_node *fn;
	struct rt6_info *rt;

	read_lock_bh(&table->tb6_lock);
	fn = fib6_lookup(&table->tb6_root, &fl->fl6_dst, &fl->fl6_src);
restart:
	rt = fn->leaf;
	rt = rt6_device_match(rt, fl->oif, flags);
	BACKTRACK(&fl->fl6_src);
out:
	dst_hold(&rt->u.dst);
	read_unlock_bh(&table->tb6_lock);

	rt->u.dst.lastuse = jiffies;
	rt->u.dst.__use++;

	return rt;

}

struct rt6_info *rt6_lookup(struct in6_addr *daddr, struct in6_addr *saddr,
			    int oif, int strict)
{
	struct flowi fl = {
		.oif = oif,
		.nl_u = {
			.ip6_u = {
				.daddr = *daddr,
			},
		},
	};
	struct dst_entry *dst;
	int flags = strict ? RT6_LOOKUP_F_IFACE : 0;

	if (saddr) {
		memcpy(&fl.fl6_src, saddr, sizeof(*saddr));
		flags |= RT6_LOOKUP_F_HAS_SADDR;
	}

	dst = fib6_rule_lookup(&fl, flags, ip6_pol_route_lookup);
	if (dst->error == 0)
		return (struct rt6_info *) dst;

	dst_release(dst);

	return NULL;
}

/* ip6_ins_rt is called with FREE table->tb6_lock.
   It takes new route entry, the addition fails by any reason the
   route is freed. In any case, if caller does not hold it, it may
   be destroyed.
 */

static int __ip6_ins_rt(struct rt6_info *rt, struct nl_info *info)
{
	int err;
	struct fib6_table *table;

	table = rt->rt6i_table;
	write_lock_bh(&table->tb6_lock);
	err = fib6_add(&table->tb6_root, rt, info);
	write_unlock_bh(&table->tb6_lock);

	return err;
}

int ip6_ins_rt(struct rt6_info *rt)
{
	return __ip6_ins_rt(rt, NULL);
}

static struct rt6_info *rt6_alloc_cow(struct rt6_info *ort, struct in6_addr *daddr,
				      struct in6_addr *saddr)
{
	struct rt6_info *rt;

	/*
	 *	Clone the route.
	 */

	rt = ip6_rt_copy(ort);

	if (rt) {
		if (!(rt->rt6i_flags&RTF_GATEWAY)) {
			if (rt->rt6i_dst.plen != 128 &&
			    ipv6_addr_equal(&rt->rt6i_dst.addr, daddr))
				rt->rt6i_flags |= RTF_ANYCAST;
			ipv6_addr_copy(&rt->rt6i_gateway, daddr);
		}

		ipv6_addr_copy(&rt->rt6i_dst.addr, daddr);
		rt->rt6i_dst.plen = 128;
		rt->rt6i_flags |= RTF_CACHE;
		rt->u.dst.flags |= DST_HOST;

#ifdef CONFIG_IPV6_SUBTREES
		if (rt->rt6i_src.plen && saddr) {
			ipv6_addr_copy(&rt->rt6i_src.addr, saddr);
			rt->rt6i_src.plen = 128;
		}
#endif

		rt->rt6i_nexthop = ndisc_get_neigh(rt->rt6i_dev, &rt->rt6i_gateway);

	}

	return rt;
}

static struct rt6_info *rt6_alloc_clone(struct rt6_info *ort, struct in6_addr *daddr)
{
	struct rt6_info *rt = ip6_rt_copy(ort);
	if (rt) {
		ipv6_addr_copy(&rt->rt6i_dst.addr, daddr);
		rt->rt6i_dst.plen = 128;
		rt->rt6i_flags |= RTF_CACHE;
		rt->u.dst.flags |= DST_HOST;
		rt->rt6i_nexthop = neigh_clone(ort->rt6i_nexthop);
	}
	return rt;
}

static struct rt6_info *ip6_pol_route_input(struct fib6_table *table,
					    struct flowi *fl, int flags)
{
	struct fib6_node *fn;
	struct rt6_info *rt, *nrt;
	int strict = 0;
	int attempts = 3;
	int err;
	int reachable = ipv6_devconf.forwarding ? 0 : RT6_LOOKUP_F_REACHABLE;

	strict |= flags & RT6_LOOKUP_F_IFACE;

relookup:
	read_lock_bh(&table->tb6_lock);

restart_2:
	fn = fib6_lookup(&table->tb6_root, &fl->fl6_dst, &fl->fl6_src);

restart:
	rt = rt6_select(&fn->leaf, fl->iif, strict | reachable);
	BACKTRACK(&fl->fl6_src);
	if (rt == &ip6_null_entry ||
	    rt->rt6i_flags & RTF_CACHE)
		goto out;

	dst_hold(&rt->u.dst);
	read_unlock_bh(&table->tb6_lock);

	if (!rt->rt6i_nexthop && !(rt->rt6i_flags & RTF_NONEXTHOP))
		nrt = rt6_alloc_cow(rt, &fl->fl6_dst, &fl->fl6_src);
	else {
#if CLONE_OFFLINK_ROUTE
		nrt = rt6_alloc_clone(rt, &fl->fl6_dst);
#else
		goto out2;
#endif
	}

	dst_release(&rt->u.dst);
	rt = nrt ? : &ip6_null_entry;

	dst_hold(&rt->u.dst);
	if (nrt) {
		err = ip6_ins_rt(nrt);
		if (!err)
			goto out2;
	}

	if (--attempts <= 0)
		goto out2;

	/*
	 * Race condition! In the gap, when table->tb6_lock was
	 * released someone could insert this route.  Relookup.
	 */
	dst_release(&rt->u.dst);
	goto relookup;

out:
	if (reachable) {
		reachable = 0;
		goto restart_2;
	}
	dst_hold(&rt->u.dst);
	read_unlock_bh(&table->tb6_lock);
out2:
	rt->u.dst.lastuse = jiffies;
	rt->u.dst.__use++;

	return rt;
}

void ip6_route_input(struct sk_buff *skb)
{
	struct ipv6hdr *iph = skb->nh.ipv6h;
	int flags = RT6_LOOKUP_F_HAS_SADDR;
	struct flowi fl = {
		.iif = skb->dev->ifindex,
		.nl_u = {
			.ip6_u = {
				.daddr = iph->daddr,
				.saddr = iph->saddr,
				.flowlabel = (* (__be32 *) iph)&IPV6_FLOWINFO_MASK,
			},
		},
 		.mark = skb->mark,
		.proto = iph->nexthdr,
	};

	if (rt6_need_strict(&iph->daddr))
		flags |= RT6_LOOKUP_F_IFACE;

	skb->dst = fib6_rule_lookup(&fl, flags, ip6_pol_route_input);
}

static struct rt6_info *ip6_pol_route_output(struct fib6_table *table,
					     struct flowi *fl, int flags)
{
	struct fib6_node *fn;
	struct rt6_info *rt, *nrt;
	int strict = 0;
	int attempts = 3;
	int err;
	int reachable = ipv6_devconf.forwarding ? 0 : RT6_LOOKUP_F_REACHABLE;

	strict |= flags & RT6_LOOKUP_F_IFACE;

relookup:
	read_lock_bh(&table->tb6_lock);

restart_2:
	fn = fib6_lookup(&table->tb6_root, &fl->fl6_dst, &fl->fl6_src);

restart:
	rt = rt6_select(&fn->leaf, fl->oif, strict | reachable);
	BACKTRACK(&fl->fl6_src);
	if (rt == &ip6_null_entry ||
	    rt->rt6i_flags & RTF_CACHE)
		goto out;

	dst_hold(&rt->u.dst);
	read_unlock_bh(&table->tb6_lock);

	if (!rt->rt6i_nexthop && !(rt->rt6i_flags & RTF_NONEXTHOP))
		nrt = rt6_alloc_cow(rt, &fl->fl6_dst, &fl->fl6_src);
	else {
#if CLONE_OFFLINK_ROUTE
		nrt = rt6_alloc_clone(rt, &fl->fl6_dst);
#else
		goto out2;
#endif
	}

	dst_release(&rt->u.dst);
	rt = nrt ? : &ip6_null_entry;

	dst_hold(&rt->u.dst);
	if (nrt) {
		err = ip6_ins_rt(nrt);
		if (!err)
			goto out2;
	}

	if (--attempts <= 0)
		goto out2;

	/*
	 * Race condition! In the gap, when table->tb6_lock was
	 * released someone could insert this route.  Relookup.
	 */
	dst_release(&rt->u.dst);
	goto relookup;

out:
	if (reachable) {
		reachable = 0;
		goto restart_2;
	}
	dst_hold(&rt->u.dst);
	read_unlock_bh(&table->tb6_lock);
out2:
	rt->u.dst.lastuse = jiffies;
	rt->u.dst.__use++;
	return rt;
}

struct dst_entry * ip6_route_output(struct sock *sk, struct flowi *fl)
{
	int flags = 0;

	if (rt6_need_strict(&fl->fl6_dst))
		flags |= RT6_LOOKUP_F_IFACE;

	if (!ipv6_addr_any(&fl->fl6_src))
		flags |= RT6_LOOKUP_F_HAS_SADDR;

	return fib6_rule_lookup(fl, flags, ip6_pol_route_output);
}


/*
 *	Destination cache support functions
 */

static struct dst_entry *ip6_dst_check(struct dst_entry *dst, u32 cookie)
{
	struct rt6_info *rt;

	rt = (struct rt6_info *) dst;

	if (rt && rt->rt6i_node && (rt->rt6i_node->fn_sernum == cookie))
		return dst;

	return NULL;
}

static struct dst_entry *ip6_negative_advice(struct dst_entry *dst)
{
	struct rt6_info *rt = (struct rt6_info *) dst;

	if (rt) {
		if (rt->rt6i_flags & RTF_CACHE)
			ip6_del_rt(rt);
		else
			dst_release(dst);
	}
	return NULL;
}

static void ip6_link_failure(struct sk_buff *skb)
{
	struct rt6_info *rt;

	icmpv6_send(skb, ICMPV6_DEST_UNREACH, ICMPV6_ADDR_UNREACH, 0, skb->dev);

	rt = (struct rt6_info *) skb->dst;
	if (rt) {
		if (rt->rt6i_flags&RTF_CACHE) {
			dst_set_expires(&rt->u.dst, 0);
			rt->rt6i_flags |= RTF_EXPIRES;
		} else if (rt->rt6i_node && (rt->rt6i_flags & RTF_DEFAULT))
			rt->rt6i_node->fn_sernum = -1;
	}
}

static void ip6_rt_update_pmtu(struct dst_entry *dst, u32 mtu)
{
	struct rt6_info *rt6 = (struct rt6_info*)dst;

	if (mtu < dst_mtu(dst) && rt6->rt6i_dst.plen == 128) {
		rt6->rt6i_flags |= RTF_MODIFIED;
		if (mtu < IPV6_MIN_MTU) {
			mtu = IPV6_MIN_MTU;
			dst->metrics[RTAX_FEATURES-1] |= RTAX_FEATURE_ALLFRAG;
		}
		dst->metrics[RTAX_MTU-1] = mtu;
		call_netevent_notifiers(NETEVENT_PMTU_UPDATE, dst);
	}
}

static int ipv6_get_mtu(struct net_device *dev);

static inline unsigned int ipv6_advmss(unsigned int mtu)
{
	mtu -= sizeof(struct ipv6hdr) + sizeof(struct tcphdr);

	if (mtu < ip6_rt_min_advmss)
		mtu = ip6_rt_min_advmss;

	/*
	 * Maximal non-jumbo IPv6 payload is IPV6_MAXPLEN and 
	 * corresponding MSS is IPV6_MAXPLEN - tcp_header_size. 
	 * IPV6_MAXPLEN is also valid and means: "any MSS, 
	 * rely only on pmtu discovery"
	 */
	if (mtu > IPV6_MAXPLEN - sizeof(struct tcphdr))
		mtu = IPV6_MAXPLEN;
	return mtu;
}

static struct dst_entry *ndisc_dst_gc_list;
static DEFINE_SPINLOCK(ndisc_lock);

struct dst_entry *ndisc_dst_alloc(struct net_device *dev, 
				  struct neighbour *neigh,
				  struct in6_addr *addr,
				  int (*output)(struct sk_buff *))
{
	struct rt6_info *rt;
	struct inet6_dev *idev = in6_dev_get(dev);

	if (unlikely(idev == NULL))
		return NULL;

	rt = ip6_dst_alloc();
	if (unlikely(rt == NULL)) {
		in6_dev_put(idev);
		goto out;
	}

	dev_hold(dev);
	if (neigh)
		neigh_hold(neigh);
	else
		neigh = ndisc_get_neigh(dev, addr);

	rt->rt6i_dev	  = dev;
	rt->rt6i_idev     = idev;
	rt->rt6i_nexthop  = neigh;
	atomic_set(&rt->u.dst.__refcnt, 1);
	rt->u.dst.metrics[RTAX_HOPLIMIT-1] = 255;
	rt->u.dst.metrics[RTAX_MTU-1] = ipv6_get_mtu(rt->rt6i_dev);
	rt->u.dst.metrics[RTAX_ADVMSS-1] = ipv6_advmss(dst_mtu(&rt->u.dst));
	rt->u.dst.output  = output;

#if 0	/* there's no chance to use these for ndisc */
	rt->u.dst.flags   = ipv6_addr_type(addr) & IPV6_ADDR_UNICAST 
				? DST_HOST 
				: 0;
	ipv6_addr_copy(&rt->rt6i_dst.addr, addr);
	rt->rt6i_dst.plen = 128;
#endif

	spin_lock_bh(&ndisc_lock);
	rt->u.dst.next = ndisc_dst_gc_list;
	ndisc_dst_gc_list = &rt->u.dst;
	spin_unlock_bh(&ndisc_lock);

	fib6_force_start_gc();

out:
	return &rt->u.dst;
}

int ndisc_dst_gc(int *more)
{
	struct dst_entry *dst, *next, **pprev;
	int freed;

	next = NULL;
 	freed = 0;

	spin_lock_bh(&ndisc_lock);
	pprev = &ndisc_dst_gc_list;

	while ((dst = *pprev) != NULL) {
		if (!atomic_read(&dst->__refcnt)) {
			*pprev = dst->next;
			dst_free(dst);
			freed++;
		} else {
			pprev = &dst->next;
			(*more)++;
		}
	}

	spin_unlock_bh(&ndisc_lock);

	return freed;
}

static int ip6_dst_gc(void)
{
	static unsigned expire = 30*HZ;
	static unsigned long last_gc;
	unsigned long now = jiffies;

	if (time_after(last_gc + ip6_rt_gc_min_interval, now) &&
	    atomic_read(&ip6_dst_ops.entries) <= ip6_rt_max_size)
		goto out;

	expire++;
	fib6_run_gc(expire);
	last_gc = now;
	if (atomic_read(&ip6_dst_ops.entries) < ip6_dst_ops.gc_thresh)
		expire = ip6_rt_gc_timeout>>1;

out:
	expire -= expire>>ip6_rt_gc_elasticity;
	return (atomic_read(&ip6_dst_ops.entries) > ip6_rt_max_size);
}

/* Clean host part of a prefix. Not necessary in radix tree,
   but results in cleaner routing tables.

   Remove it only when all the things will work!
 */

static int ipv6_get_mtu(struct net_device *dev)
{
	int mtu = IPV6_MIN_MTU;
	struct inet6_dev *idev;

	idev = in6_dev_get(dev);
	if (idev) {
		mtu = idev->cnf.mtu6;
		in6_dev_put(idev);
	}
	return mtu;
}

int ipv6_get_hoplimit(struct net_device *dev)
{
	int hoplimit = ipv6_devconf.hop_limit;
	struct inet6_dev *idev;

	idev = in6_dev_get(dev);
	if (idev) {
		hoplimit = idev->cnf.hop_limit;
		in6_dev_put(idev);
	}
	return hoplimit;
}

/*
 *
 */

int ip6_route_add(struct fib6_config *cfg)
{
	int err;
	struct rt6_info *rt = NULL;
	struct net_device *dev = NULL;
	struct inet6_dev *idev = NULL;
	struct fib6_table *table;
	int addr_type;

	if (cfg->fc_dst_len > 128 || cfg->fc_src_len > 128)
		return -EINVAL;
#ifndef CONFIG_IPV6_SUBTREES
	if (cfg->fc_src_len)
		return -EINVAL;
#endif
	if (cfg->fc_ifindex) {
		err = -ENODEV;
		dev = dev_get_by_index(cfg->fc_ifindex);
		if (!dev)
			goto out;
		idev = in6_dev_get(dev);
		if (!idev)
			goto out;
	}

	if (cfg->fc_metric == 0)
		cfg->fc_metric = IP6_RT_PRIO_USER;

	table = fib6_new_table(cfg->fc_table);
	if (table == NULL) {
		err = -ENOBUFS;
		goto out;
	}

	rt = ip6_dst_alloc();

	if (rt == NULL) {
		err = -ENOMEM;
		goto out;
	}

	rt->u.dst.obsolete = -1;
	rt->rt6i_expires = jiffies + clock_t_to_jiffies(cfg->fc_expires);

	if (cfg->fc_protocol == RTPROT_UNSPEC)
		cfg->fc_protocol = RTPROT_BOOT;
	rt->rt6i_protocol = cfg->fc_protocol;

	addr_type = ipv6_addr_type(&cfg->fc_dst);

	if (addr_type & IPV6_ADDR_MULTICAST)
		rt->u.dst.input = ip6_mc_input;
	else
		rt->u.dst.input = ip6_forward;

	rt->u.dst.output = ip6_output;

	ipv6_addr_prefix(&rt->rt6i_dst.addr, &cfg->fc_dst, cfg->fc_dst_len);
	rt->rt6i_dst.plen = cfg->fc_dst_len;
	if (rt->rt6i_dst.plen == 128)
	       rt->u.dst.flags = DST_HOST;

#ifdef CONFIG_IPV6_SUBTREES
	ipv6_addr_prefix(&rt->rt6i_src.addr, &cfg->fc_src, cfg->fc_src_len);
	rt->rt6i_src.plen = cfg->fc_src_len;
#endif

	rt->rt6i_metric = cfg->fc_metric;

	/* We cannot add true routes via loopback here,
	   they would result in kernel looping; promote them to reject routes
	 */
	if ((cfg->fc_flags & RTF_REJECT) ||
	    (dev && (dev->flags&IFF_LOOPBACK) && !(addr_type&IPV6_ADDR_LOOPBACK))) {
		/* hold loopback dev/idev if we haven't done so. */
		if (dev != &loopback_dev) {
			if (dev) {
				dev_put(dev);
				in6_dev_put(idev);
			}
			dev = &loopback_dev;
			dev_hold(dev);
			idev = in6_dev_get(dev);
			if (!idev) {
				err = -ENODEV;
				goto out;
			}
		}
		rt->u.dst.output = ip6_pkt_discard_out;
		rt->u.dst.input = ip6_pkt_discard;
		rt->u.dst.error = -ENETUNREACH;
		rt->rt6i_flags = RTF_REJECT|RTF_NONEXTHOP;
		goto install_route;
	}

	if (cfg->fc_flags & RTF_GATEWAY) {
		struct in6_addr *gw_addr;
		int gwa_type;

		gw_addr = &cfg->fc_gateway;
		ipv6_addr_copy(&rt->rt6i_gateway, gw_addr);
		gwa_type = ipv6_addr_type(gw_addr);

		if (gwa_type != (IPV6_ADDR_LINKLOCAL|IPV6_ADDR_UNICAST)) {
			struct rt6_info *grt;

			/* IPv6 strictly inhibits using not link-local
			   addresses as nexthop address.
			   Otherwise, router will not able to send redirects.
			   It is very good, but in some (rare!) circumstances
			   (SIT, PtP, NBMA NOARP links) it is handy to allow
			   some exceptions. --ANK
			 */
			err = -EINVAL;
			if (!(gwa_type&IPV6_ADDR_UNICAST))
				goto out;

			grt = rt6_lookup(gw_addr, NULL, cfg->fc_ifindex, 1);

			err = -EHOSTUNREACH;
			if (grt == NULL)
				goto out;
			if (dev) {
				if (dev != grt->rt6i_dev) {
					dst_release(&grt->u.dst);
					goto out;
				}
			} else {
				dev = grt->rt6i_dev;
				idev = grt->rt6i_idev;
				dev_hold(dev);
				in6_dev_hold(grt->rt6i_idev);
			}
			if (!(grt->rt6i_flags&RTF_GATEWAY))
				err = 0;
			dst_release(&grt->u.dst);

			if (err)
				goto out;
		}
		err = -EINVAL;
		if (dev == NULL || (dev->flags&IFF_LOOPBACK))
			goto out;
	}

	err = -ENODEV;
	if (dev == NULL)
		goto out;

	if (cfg->fc_flags & (RTF_GATEWAY | RTF_NONEXTHOP)) {
		rt->rt6i_nexthop = __neigh_lookup_errno(&nd_tbl, &rt->rt6i_gateway, dev);
		if (IS_ERR(rt->rt6i_nexthop)) {
			err = PTR_ERR(rt->rt6i_nexthop);
			rt->rt6i_nexthop = NULL;
			goto out;
		}
	}

	rt->rt6i_flags = cfg->fc_flags;

install_route:
	if (cfg->fc_mx) {
		struct nlattr *nla;
		int remaining;

		nla_for_each_attr(nla, cfg->fc_mx, cfg->fc_mx_len, remaining) {
			int type = nla->nla_type;

			if (type) {
				if (type > RTAX_MAX) {
					err = -EINVAL;
					goto out;
				}

				rt->u.dst.metrics[type - 1] = nla_get_u32(nla);
			}
		}
	}

	if (rt->u.dst.metrics[RTAX_HOPLIMIT-1] == 0)
		rt->u.dst.metrics[RTAX_HOPLIMIT-1] = -1;
	if (!rt->u.dst.metrics[RTAX_MTU-1])
		rt->u.dst.metrics[RTAX_MTU-1] = ipv6_get_mtu(dev);
	if (!rt->u.dst.metrics[RTAX_ADVMSS-1])
		rt->u.dst.metrics[RTAX_ADVMSS-1] = ipv6_advmss(dst_mtu(&rt->u.dst));
	rt->u.dst.dev = dev;
	rt->rt6i_idev = idev;
	rt->rt6i_table = table;
	return __ip6_ins_rt(rt, &cfg->fc_nlinfo);

out:
	if (dev)
		dev_put(dev);
	if (idev)
		in6_dev_put(idev);
	if (rt)
		dst_free(&rt->u.dst);
	return err;
}

static int __ip6_del_rt(struct rt6_info *rt, struct nl_info *info)
{
	int err;
	struct fib6_table *table;

	if (rt == &ip6_null_entry)
		return -ENOENT;

	table = rt->rt6i_table;
	write_lock_bh(&table->tb6_lock);

	err = fib6_del(rt, info);
	dst_release(&rt->u.dst);

	write_unlock_bh(&table->tb6_lock);

	return err;
}

int ip6_del_rt(struct rt6_info *rt)
{
	return __ip6_del_rt(rt, NULL);
}

static int ip6_route_del(struct fib6_config *cfg)
{
	struct fib6_table *table;
	struct fib6_node *fn;
	struct rt6_info *rt;
	int err = -ESRCH;

	table = fib6_get_table(cfg->fc_table);
	if (table == NULL)
		return err;

	read_lock_bh(&table->tb6_lock);

	fn = fib6_locate(&table->tb6_root,
			 &cfg->fc_dst, cfg->fc_dst_len,
			 &cfg->fc_src, cfg->fc_src_len);
	
	if (fn) {
		for (rt = fn->leaf; rt; rt = rt->u.next) {
			if (cfg->fc_ifindex &&
			    (rt->rt6i_dev == NULL ||
			     rt->rt6i_dev->ifindex != cfg->fc_ifindex))
				continue;
			if (cfg->fc_flags & RTF_GATEWAY &&
			    !ipv6_addr_equal(&cfg->fc_gateway, &rt->rt6i_gateway))
				continue;
			if (cfg->fc_metric && cfg->fc_metric != rt->rt6i_metric)
				continue;
			dst_hold(&rt->u.dst);
			read_unlock_bh(&table->tb6_lock);

			return __ip6_del_rt(rt, &cfg->fc_nlinfo);
		}
	}
	read_unlock_bh(&table->tb6_lock);

	return err;
}

/*
 *	Handle redirects
 */
struct ip6rd_flowi {
	struct flowi fl;
	struct in6_addr gateway;
};

static struct rt6_info *__ip6_route_redirect(struct fib6_table *table,
					     struct flowi *fl,
					     int flags)
{
	struct ip6rd_flowi *rdfl = (struct ip6rd_flowi *)fl;
	struct rt6_info *rt;
	struct fib6_node *fn;

	/*
	 * Get the "current" route for this destination and
	 * check if the redirect has come from approriate router.
	 *
	 * RFC 2461 specifies that redirects should only be
	 * accepted if they come from the nexthop to the target.
	 * Due to the way the routes are chosen, this notion
	 * is a bit fuzzy and one might need to check all possible
	 * routes.
	 */

	read_lock_bh(&table->tb6_lock);
	fn = fib6_lookup(&table->tb6_root, &fl->fl6_dst, &fl->fl6_src);
restart:
	for (rt = fn->leaf; rt; rt = rt->u.next) {
		/*
		 * Current route is on-link; redirect is always invalid.
		 *
		 * Seems, previous statement is not true. It could
		 * be node, which looks for us as on-link (f.e. proxy ndisc)
		 * But then router serving it might decide, that we should
		 * know truth 8)8) --ANK (980726).
		 */
		if (rt6_check_expired(rt))
			continue;
		if (!(rt->rt6i_flags & RTF_GATEWAY))
			continue;
		if (fl->oif != rt->rt6i_dev->ifindex)
			continue;
		if (!ipv6_addr_equal(&rdfl->gateway, &rt->rt6i_gateway))
			continue;
		break;
	}

	if (!rt)
		rt = &ip6_null_entry;
	BACKTRACK(&fl->fl6_src);
out:
	dst_hold(&rt->u.dst);

	read_unlock_bh(&table->tb6_lock);

	return rt;
};

static struct rt6_info *ip6_route_redirect(struct in6_addr *dest,
					   struct in6_addr *src,
					   struct in6_addr *gateway,
					   struct net_device *dev)
{
	int flags = RT6_LOOKUP_F_HAS_SADDR;
	struct ip6rd_flowi rdfl = {
		.fl = {
			.oif = dev->ifindex,
			.nl_u = {
				.ip6_u = {
					.daddr = *dest,
					.saddr = *src,
				},
			},
		},
		.gateway = *gateway,
	};

	if (rt6_need_strict(dest))
		flags |= RT6_LOOKUP_F_IFACE;

	return (struct rt6_info *)fib6_rule_lookup((struct flowi *)&rdfl, flags, __ip6_route_redirect);
}

void rt6_redirect(struct in6_addr *dest, struct in6_addr *src,
		  struct in6_addr *saddr,
		  struct neighbour *neigh, u8 *lladdr, int on_link)
{
	struct rt6_info *rt, *nrt = NULL;
	struct netevent_redirect netevent;

	rt = ip6_route_redirect(dest, src, saddr, neigh->dev);

	if (rt == &ip6_null_entry) {
		if (net_ratelimit())
			printk(KERN_DEBUG "rt6_redirect: source isn't a valid nexthop "
			       "for redirect target\n");
		goto out;
	}

	/*
	 *	We have finally decided to accept it.
	 */

	neigh_update(neigh, lladdr, NUD_STALE, 
		     NEIGH_UPDATE_F_WEAK_OVERRIDE|
		     NEIGH_UPDATE_F_OVERRIDE|
		     (on_link ? 0 : (NEIGH_UPDATE_F_OVERRIDE_ISROUTER|
				     NEIGH_UPDATE_F_ISROUTER))
		     );

	/*
	 * Redirect received -> path was valid.
	 * Look, redirects are sent only in response to data packets,
	 * so that this nexthop apparently is reachable. --ANK
	 */
	dst_confirm(&rt->u.dst);

	/* Duplicate redirect: silently ignore. */
	if (neigh == rt->u.dst.neighbour)
		goto out;

	nrt = ip6_rt_copy(rt);
	if (nrt == NULL)
		goto out;

	nrt->rt6i_flags = RTF_GATEWAY|RTF_UP|RTF_DYNAMIC|RTF_CACHE;
	if (on_link)
		nrt->rt6i_flags &= ~RTF_GATEWAY;

	ipv6_addr_copy(&nrt->rt6i_dst.addr, dest);
	nrt->rt6i_dst.plen = 128;
	nrt->u.dst.flags |= DST_HOST;

	ipv6_addr_copy(&nrt->rt6i_gateway, (struct in6_addr*)neigh->primary_key);
	nrt->rt6i_nexthop = neigh_clone(neigh);
	/* Reset pmtu, it may be better */
	nrt->u.dst.metrics[RTAX_MTU-1] = ipv6_get_mtu(neigh->dev);
	nrt->u.dst.metrics[RTAX_ADVMSS-1] = ipv6_advmss(dst_mtu(&nrt->u.dst));

	if (ip6_ins_rt(nrt))
		goto out;

	netevent.old = &rt->u.dst;
	netevent.new = &nrt->u.dst;
	call_netevent_notifiers(NETEVENT_REDIRECT, &netevent);

	if (rt->rt6i_flags&RTF_CACHE) {
		ip6_del_rt(rt);
		return;
	}

out:
        dst_release(&rt->u.dst);
	return;
}

/*
 *	Handle ICMP "packet too big" messages
 *	i.e. Path MTU discovery
 */

void rt6_pmtu_discovery(struct in6_addr *daddr, struct in6_addr *saddr,
			struct net_device *dev, u32 pmtu)
{
	struct rt6_info *rt, *nrt;
	int allfrag = 0;

	rt = rt6_lookup(daddr, saddr, dev->ifindex, 0);
	if (rt == NULL)
		return;

	if (pmtu >= dst_mtu(&rt->u.dst))
		goto out;

	if (pmtu < IPV6_MIN_MTU) {
		/*
		 * According to RFC2460, PMTU is set to the IPv6 Minimum Link 
		 * MTU (1280) and a fragment header should always be included
		 * after a node receiving Too Big message reporting PMTU is
		 * less than the IPv6 Minimum Link MTU.
		 */
		pmtu = IPV6_MIN_MTU;
		allfrag = 1;
	}

	/* New mtu received -> path was valid.
	   They are sent only in response to data packets,
	   so that this nexthop apparently is reachable. --ANK
	 */
	dst_confirm(&rt->u.dst);

	/* Host route. If it is static, it would be better
	   not to override it, but add new one, so that
	   when cache entry will expire old pmtu
	   would return automatically.
	 */
	if (rt->rt6i_flags & RTF_CACHE) {
		rt->u.dst.metrics[RTAX_MTU-1] = pmtu;
		if (allfrag)
			rt->u.dst.metrics[RTAX_FEATURES-1] |= RTAX_FEATURE_ALLFRAG;
		dst_set_expires(&rt->u.dst, ip6_rt_mtu_expires);
		rt->rt6i_flags |= RTF_MODIFIED|RTF_EXPIRES;
		goto out;
	}

	/* Network route.
	   Two cases are possible:
	   1. It is connected route. Action: COW
	   2. It is gatewayed route or NONEXTHOP route. Action: clone it.
	 */
	if (!rt->rt6i_nexthop && !(rt->rt6i_flags & RTF_NONEXTHOP))
		nrt = rt6_alloc_cow(rt, daddr, saddr);
	else
		nrt = rt6_alloc_clone(rt, daddr);

	if (nrt) {
		nrt->u.dst.metrics[RTAX_MTU-1] = pmtu;
		if (allfrag)
			nrt->u.dst.metrics[RTAX_FEATURES-1] |= RTAX_FEATURE_ALLFRAG;

		/* According to RFC 1981, detecting PMTU increase shouldn't be
		 * happened within 5 mins, the recommended timer is 10 mins.
		 * Here this route expiration time is set to ip6_rt_mtu_expires
		 * which is 10 mins. After 10 mins the decreased pmtu is expired
		 * and detecting PMTU increase will be automatically happened.
		 */
		dst_set_expires(&nrt->u.dst, ip6_rt_mtu_expires);
		nrt->rt6i_flags |= RTF_DYNAMIC|RTF_EXPIRES;

		ip6_ins_rt(nrt);
	}
out:
	dst_release(&rt->u.dst);
}

/*
 *	Misc support functions
 */

static struct rt6_info * ip6_rt_copy(struct rt6_info *ort)
{
	struct rt6_info *rt = ip6_dst_alloc();

	if (rt) {
		rt->u.dst.input = ort->u.dst.input;
		rt->u.dst.output = ort->u.dst.output;

		memcpy(rt->u.dst.metrics, ort->u.dst.metrics, RTAX_MAX*sizeof(u32));
		rt->u.dst.error = ort->u.dst.error;
		rt->u.dst.dev = ort->u.dst.dev;
		if (rt->u.dst.dev)
			dev_hold(rt->u.dst.dev);
		rt->rt6i_idev = ort->rt6i_idev;
		if (rt->rt6i_idev)
			in6_dev_hold(rt->rt6i_idev);
		rt->u.dst.lastuse = jiffies;
		rt->rt6i_expires = 0;

		ipv6_addr_copy(&rt->rt6i_gateway, &ort->rt6i_gateway);
		rt->rt6i_flags = ort->rt6i_flags & ~RTF_EXPIRES;
		rt->rt6i_metric = 0;

		memcpy(&rt->rt6i_dst, &ort->rt6i_dst, sizeof(struct rt6key));
#ifdef CONFIG_IPV6_SUBTREES
		memcpy(&rt->rt6i_src, &ort->rt6i_src, sizeof(struct rt6key));
#endif
		rt->rt6i_table = ort->rt6i_table;
	}
	return rt;
}

#ifdef CONFIG_IPV6_ROUTE_INFO
static struct rt6_info *rt6_get_route_info(struct in6_addr *prefix, int prefixlen,
					   struct in6_addr *gwaddr, int ifindex)
{
	struct fib6_node *fn;
	struct rt6_info *rt = NULL;
	struct fib6_table *table;

	table = fib6_get_table(RT6_TABLE_INFO);
	if (table == NULL)
		return NULL;

	write_lock_bh(&table->tb6_lock);
	fn = fib6_locate(&table->tb6_root, prefix ,prefixlen, NULL, 0);
	if (!fn)
		goto out;

	for (rt = fn->leaf; rt; rt = rt->u.next) {
		if (rt->rt6i_dev->ifindex != ifindex)
			continue;
		if ((rt->rt6i_flags & (RTF_ROUTEINFO|RTF_GATEWAY)) != (RTF_ROUTEINFO|RTF_GATEWAY))
			continue;
		if (!ipv6_addr_equal(&rt->rt6i_gateway, gwaddr))
			continue;
		dst_hold(&rt->u.dst);
		break;
	}
out:
	write_unlock_bh(&table->tb6_lock);
	return rt;
}

static struct rt6_info *rt6_add_route_info(struct in6_addr *prefix, int prefixlen,
					   struct in6_addr *gwaddr, int ifindex,
					   unsigned pref)
{
	struct fib6_config cfg = {
		.fc_table	= RT6_TABLE_INFO,
		.fc_metric	= 1024,
		.fc_ifindex	= ifindex,
		.fc_dst_len	= prefixlen,
		.fc_flags	= RTF_GATEWAY | RTF_ADDRCONF | RTF_ROUTEINFO |
				  RTF_UP | RTF_PREF(pref),
	};

	ipv6_addr_copy(&cfg.fc_dst, prefix);
	ipv6_addr_copy(&cfg.fc_gateway, gwaddr);

	/* We should treat it as a default route if prefix length is 0. */
	if (!prefixlen)
		cfg.fc_flags |= RTF_DEFAULT;

	ip6_route_add(&cfg);

	return rt6_get_route_info(prefix, prefixlen, gwaddr, ifindex);
}
#endif

struct rt6_info *rt6_get_dflt_router(struct in6_addr *addr, struct net_device *dev)
{	
	struct rt6_info *rt;
	struct fib6_table *table;

	table = fib6_get_table(RT6_TABLE_DFLT);
	if (table == NULL)
		return NULL;

	write_lock_bh(&table->tb6_lock);
	for (rt = table->tb6_root.leaf; rt; rt=rt->u.next) {
		if (dev == rt->rt6i_dev &&
		    ((rt->rt6i_flags & (RTF_ADDRCONF | RTF_DEFAULT)) == (RTF_ADDRCONF | RTF_DEFAULT)) &&
		    ipv6_addr_equal(&rt->rt6i_gateway, addr))
			break;
	}
	if (rt)
		dst_hold(&rt->u.dst);
	write_unlock_bh(&table->tb6_lock);
	return rt;
}

struct rt6_info *rt6_add_dflt_router(struct in6_addr *gwaddr,
				     struct net_device *dev,
				     unsigned int pref)
{
	struct fib6_config cfg = {
		.fc_table	= RT6_TABLE_DFLT,
		.fc_metric	= 1024,
		.fc_ifindex	= dev->ifindex,
		.fc_flags	= RTF_GATEWAY | RTF_ADDRCONF | RTF_DEFAULT |
				  RTF_UP | RTF_EXPIRES | RTF_PREF(pref),
	};

	ipv6_addr_copy(&cfg.fc_gateway, gwaddr);

	ip6_route_add(&cfg);

	return rt6_get_dflt_router(gwaddr, dev);
}

void rt6_purge_dflt_routers(void)
{
	struct rt6_info *rt;
	struct fib6_table *table;

	/* NOTE: Keep consistent with rt6_get_dflt_router */
	table = fib6_get_table(RT6_TABLE_DFLT);
	if (table == NULL)
		return;

restart:
	read_lock_bh(&table->tb6_lock);
	for (rt = table->tb6_root.leaf; rt; rt = rt->u.next) {
		if (rt->rt6i_flags & (RTF_DEFAULT | RTF_ADDRCONF)) {
			dst_hold(&rt->u.dst);
			read_unlock_bh(&table->tb6_lock);
			ip6_del_rt(rt);
			goto restart;
		}
	}
	read_unlock_bh(&table->tb6_lock);
}

static void rtmsg_to_fib6_config(struct in6_rtmsg *rtmsg,
				 struct fib6_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));

	cfg->fc_table = RT6_TABLE_MAIN;
	cfg->fc_ifindex = rtmsg->rtmsg_ifindex;
	cfg->fc_metric = rtmsg->rtmsg_metric;
	cfg->fc_expires = rtmsg->rtmsg_info;
	cfg->fc_dst_len = rtmsg->rtmsg_dst_len;
	cfg->fc_src_len = rtmsg->rtmsg_src_len;
	cfg->fc_flags = rtmsg->rtmsg_flags;

	ipv6_addr_copy(&cfg->fc_dst, &rtmsg->rtmsg_dst);
	ipv6_addr_copy(&cfg->fc_src, &rtmsg->rtmsg_src);
	ipv6_addr_copy(&cfg->fc_gateway, &rtmsg->rtmsg_gateway);
}

int ipv6_route_ioctl(unsigned int cmd, void __user *arg)
{
	struct fib6_config cfg;
	struct in6_rtmsg rtmsg;
	int err;

	switch(cmd) {
	case SIOCADDRT:		/* Add a route */
	case SIOCDELRT:		/* Delete a route */
		if (!capable(CAP_NET_ADMIN))
			return -EPERM;
		err = copy_from_user(&rtmsg, arg,
				     sizeof(struct in6_rtmsg));
		if (err)
			return -EFAULT;

		rtmsg_to_fib6_config(&rtmsg, &cfg);

		rtnl_lock();
		switch (cmd) {
		case SIOCADDRT:
			err = ip6_route_add(&cfg);
			break;
		case SIOCDELRT:
			err = ip6_route_del(&cfg);
			break;
		default:
			err = -EINVAL;
		}
		rtnl_unlock();

		return err;
	};

	return -EINVAL;
}

/*
 *	Drop the packet on the floor
 */

static inline int ip6_pkt_drop(struct sk_buff *skb, int code)
{
	int type = ipv6_addr_type(&skb->nh.ipv6h->daddr);
	if (type == IPV6_ADDR_ANY || type == IPV6_ADDR_RESERVED)
		IP6_INC_STATS(ip6_dst_idev(skb->dst), IPSTATS_MIB_INADDRERRORS);

	IP6_INC_STATS(ip6_dst_idev(skb->dst), IPSTATS_MIB_OUTNOROUTES);
	icmpv6_send(skb, ICMPV6_DEST_UNREACH, code, 0, skb->dev);
	kfree_skb(skb);
	return 0;
}

static int ip6_pkt_discard(struct sk_buff *skb)
{
	return ip6_pkt_drop(skb, ICMPV6_NOROUTE);
}

static int ip6_pkt_discard_out(struct sk_buff *skb)
{
	skb->dev = skb->dst->dev;
	return ip6_pkt_discard(skb);
}

#ifdef CONFIG_IPV6_MULTIPLE_TABLES

static int ip6_pkt_prohibit(struct sk_buff *skb)
{
	return ip6_pkt_drop(skb, ICMPV6_ADM_PROHIBITED);
}

static int ip6_pkt_prohibit_out(struct sk_buff *skb)
{
	skb->dev = skb->dst->dev;
	return ip6_pkt_prohibit(skb);
}

static int ip6_pkt_blk_hole(struct sk_buff *skb)
{
	kfree_skb(skb);
	return 0;
}

#endif

/*
 *	Allocate a dst for local (unicast / anycast) address.
 */

struct rt6_info *addrconf_dst_alloc(struct inet6_dev *idev,
				    const struct in6_addr *addr,
				    int anycast)
{
	struct rt6_info *rt = ip6_dst_alloc();

	if (rt == NULL)
		return ERR_PTR(-ENOMEM);

	dev_hold(&loopback_dev);
	in6_dev_hold(idev);

	rt->u.dst.flags = DST_HOST;
	rt->u.dst.input = ip6_input;
	rt->u.dst.output = ip6_output;
	rt->rt6i_dev = &loopback_dev;
	rt->rt6i_idev = idev;
	rt->u.dst.metrics[RTAX_MTU-1] = ipv6_get_mtu(rt->rt6i_dev);
	rt->u.dst.metrics[RTAX_ADVMSS-1] = ipv6_advmss(dst_mtu(&rt->u.dst));
	rt->u.dst.metrics[RTAX_HOPLIMIT-1] = -1;
	rt->u.dst.obsolete = -1;

	rt->rt6i_flags = RTF_UP | RTF_NONEXTHOP;
	if (anycast)
		rt->rt6i_flags |= RTF_ANYCAST;
	else
		rt->rt6i_flags |= RTF_LOCAL;
	rt->rt6i_nexthop = ndisc_get_neigh(rt->rt6i_dev, &rt->rt6i_gateway);
	if (rt->rt6i_nexthop == NULL) {
		dst_free(&rt->u.dst);
		return ERR_PTR(-ENOMEM);
	}

	ipv6_addr_copy(&rt->rt6i_dst.addr, addr);
	rt->rt6i_dst.plen = 128;
	rt->rt6i_table = fib6_get_table(RT6_TABLE_LOCAL);

	atomic_set(&rt->u.dst.__refcnt, 1);

	return rt;
}

static int fib6_ifdown(struct rt6_info *rt, void *arg)
{
	if (((void*)rt->rt6i_dev == arg || arg == NULL) &&
	    rt != &ip6_null_entry) {
		RT6_TRACE("deleted by ifdown %p\n", rt);
		return -1;
	}
	return 0;
}

void rt6_ifdown(struct net_device *dev)
{
	fib6_clean_all(fib6_ifdown, 0, dev);
}

struct rt6_mtu_change_arg
{
	struct net_device *dev;
	unsigned mtu;
};

static int rt6_mtu_change_route(struct rt6_info *rt, void *p_arg)
{
	struct rt6_mtu_change_arg *arg = (struct rt6_mtu_change_arg *) p_arg;
	struct inet6_dev *idev;

	/* In IPv6 pmtu discovery is not optional,
	   so that RTAX_MTU lock cannot disable it.
	   We still use this lock to block changes
	   caused by addrconf/ndisc.
	*/

	idev = __in6_dev_get(arg->dev);
	if (idev == NULL)
		return 0;

	/* For administrative MTU increase, there is no way to discover
	   IPv6 PMTU increase, so PMTU increase should be updated here.
	   Since RFC 1981 doesn't include administrative MTU increase
	   update PMTU increase is a MUST. (i.e. jumbo frame)
	 */
	/*
	   If new MTU is less than route PMTU, this new MTU will be the
	   lowest MTU in the path, update the route PMTU to reflect PMTU
	   decreases; if new MTU is greater than route PMTU, and the
	   old MTU is the lowest MTU in the path, update the route PMTU
	   to reflect the increase. In this case if the other nodes' MTU
	   also have the lowest MTU, TOO BIG MESSAGE will be lead to
	   PMTU discouvery.
	 */
	if (rt->rt6i_dev == arg->dev &&
	    !dst_metric_locked(&rt->u.dst, RTAX_MTU) &&
            (dst_mtu(&rt->u.dst) > arg->mtu ||
             (dst_mtu(&rt->u.dst) < arg->mtu &&
	      dst_mtu(&rt->u.dst) == idev->cnf.mtu6)))
		rt->u.dst.metrics[RTAX_MTU-1] = arg->mtu;
	rt->u.dst.metrics[RTAX_ADVMSS-1] = ipv6_advmss(arg->mtu);
	return 0;
}

void rt6_mtu_change(struct net_device *dev, unsigned mtu)
{
	struct rt6_mtu_change_arg arg = {
		.dev = dev,
		.mtu = mtu,
	};

	fib6_clean_all(rt6_mtu_change_route, 0, &arg);
}

static struct nla_policy rtm_ipv6_policy[RTA_MAX+1] __read_mostly = {
	[RTA_GATEWAY]           = { .len = sizeof(struct in6_addr) },
	[RTA_OIF]               = { .type = NLA_U32 },
	[RTA_IIF]		= { .type = NLA_U32 },
	[RTA_PRIORITY]          = { .type = NLA_U32 },
	[RTA_METRICS]           = { .type = NLA_NESTED },
};

static int rtm_to_fib6_config(struct sk_buff *skb, struct nlmsghdr *nlh,
			      struct fib6_config *cfg)
{
	struct rtmsg *rtm;
	struct nlattr *tb[RTA_MAX+1];
	int err;

	err = nlmsg_parse(nlh, sizeof(*rtm), tb, RTA_MAX, rtm_ipv6_policy);
	if (err < 0)
		goto errout;

	err = -EINVAL;
	rtm = nlmsg_data(nlh);
	memset(cfg, 0, sizeof(*cfg));

	cfg->fc_table = rtm->rtm_table;
	cfg->fc_dst_len = rtm->rtm_dst_len;
	cfg->fc_src_len = rtm->rtm_src_len;
	cfg->fc_flags = RTF_UP;
	cfg->fc_protocol = rtm->rtm_protocol;

	if (rtm->rtm_type == RTN_UNREACHABLE)
		cfg->fc_flags |= RTF_REJECT;

	cfg->fc_nlinfo.pid = NETLINK_CB(skb).pid;
	cfg->fc_nlinfo.nlh = nlh;

	if (tb[RTA_GATEWAY]) {
		nla_memcpy(&cfg->fc_gateway, tb[RTA_GATEWAY], 16);
		cfg->fc_flags |= RTF_GATEWAY;
	}

	if (tb[RTA_DST]) {
		int plen = (rtm->rtm_dst_len + 7) >> 3;

		if (nla_len(tb[RTA_DST]) < plen)
			goto errout;

		nla_memcpy(&cfg->fc_dst, tb[RTA_DST], plen);
	}

	if (tb[RTA_SRC]) {
		int plen = (rtm->rtm_src_len + 7) >> 3;

		if (nla_len(tb[RTA_SRC]) < plen)
			goto errout;

		nla_memcpy(&cfg->fc_src, tb[RTA_SRC], plen);
	}

	if (tb[RTA_OIF])
		cfg->fc_ifindex = nla_get_u32(tb[RTA_OIF]);

	if (tb[RTA_PRIORITY])
		cfg->fc_metric = nla_get_u32(tb[RTA_PRIORITY]);

	if (tb[RTA_METRICS]) {
		cfg->fc_mx = nla_data(tb[RTA_METRICS]);
		cfg->fc_mx_len = nla_len(tb[RTA_METRICS]);
	}

	if (tb[RTA_TABLE])
		cfg->fc_table = nla_get_u32(tb[RTA_TABLE]);

	err = 0;
errout:
	return err;
}

int inet6_rtm_delroute(struct sk_buff *skb, struct nlmsghdr* nlh, void *arg)
{
	struct fib6_config cfg;
	int err;

	err = rtm_to_fib6_config(skb, nlh, &cfg);
	if (err < 0)
		return err;

	return ip6_route_del(&cfg);
}

int inet6_rtm_newroute(struct sk_buff *skb, struct nlmsghdr* nlh, void *arg)
{
	struct fib6_config cfg;
	int err;

	err = rtm_to_fib6_config(skb, nlh, &cfg);
	if (err < 0)
		return err;

	return ip6_route_add(&cfg);
}

static inline size_t rt6_nlmsg_size(void)
{
	return NLMSG_ALIGN(sizeof(struct rtmsg))
	       + nla_total_size(16) /* RTA_SRC */
	       + nla_total_size(16) /* RTA_DST */
	       + nla_total_size(16) /* RTA_GATEWAY */
	       + nla_total_size(16) /* RTA_PREFSRC */
	       + nla_total_size(4) /* RTA_TABLE */
	       + nla_total_size(4) /* RTA_IIF */
	       + nla_total_size(4) /* RTA_OIF */
	       + nla_total_size(4) /* RTA_PRIORITY */
	       + nla_total_size(sizeof(struct rta_cacheinfo));
}

static int rt6_fill_node(struct sk_buff *skb, struct rt6_info *rt,
			 struct in6_addr *dst, struct in6_addr *src,
			 int iif, int type, u32 pid, u32 seq,
			 int prefix, unsigned int flags)
{
	struct rtmsg *rtm;
	struct nlmsghdr *nlh;
	struct rta_cacheinfo ci;
	u32 table;

	if (prefix) {	/* user wants prefix routes only */
		if (!(rt->rt6i_flags & RTF_PREFIX_RT)) {
			/* success since this is not a prefix route */
			return 1;
		}
	}

	nlh = nlmsg_put(skb, pid, seq, type, sizeof(*rtm), flags);
	if (nlh == NULL)
		return -ENOBUFS;

	rtm = nlmsg_data(nlh);
	rtm->rtm_family = AF_INET6;
	rtm->rtm_dst_len = rt->rt6i_dst.plen;
	rtm->rtm_src_len = rt->rt6i_src.plen;
	rtm->rtm_tos = 0;
	if (rt->rt6i_table)
		table = rt->rt6i_table->tb6_id;
	else
		table = RT6_TABLE_UNSPEC;
	rtm->rtm_table = table;
	NLA_PUT_U32(skb, RTA_TABLE, table);
	if (rt->rt6i_flags&RTF_REJECT)
		rtm->rtm_type = RTN_UNREACHABLE;
	else if (rt->rt6i_dev && (rt->rt6i_dev->flags&IFF_LOOPBACK))
		rtm->rtm_type = RTN_LOCAL;
	else
		rtm->rtm_type = RTN_UNICAST;
	rtm->rtm_flags = 0;
	rtm->rtm_scope = RT_SCOPE_UNIVERSE;
	rtm->rtm_protocol = rt->rt6i_protocol;
	if (rt->rt6i_flags&RTF_DYNAMIC)
		rtm->rtm_protocol = RTPROT_REDIRECT;
	else if (rt->rt6i_flags & RTF_ADDRCONF)
		rtm->rtm_protocol = RTPROT_KERNEL;
	else if (rt->rt6i_flags&RTF_DEFAULT)
		rtm->rtm_protocol = RTPROT_RA;

	if (rt->rt6i_flags&RTF_CACHE)
		rtm->rtm_flags |= RTM_F_CLONED;

	if (dst) {
		NLA_PUT(skb, RTA_DST, 16, dst);
	        rtm->rtm_dst_len = 128;
	} else if (rtm->rtm_dst_len)
		NLA_PUT(skb, RTA_DST, 16, &rt->rt6i_dst.addr);
#ifdef CONFIG_IPV6_SUBTREES
	if (src) {
		NLA_PUT(skb, RTA_SRC, 16, src);
	        rtm->rtm_src_len = 128;
	} else if (rtm->rtm_src_len)
		NLA_PUT(skb, RTA_SRC, 16, &rt->rt6i_src.addr);
#endif
	if (iif)
		NLA_PUT_U32(skb, RTA_IIF, iif);
	else if (dst) {
		struct in6_addr saddr_buf;
		if (ipv6_get_saddr(&rt->u.dst, dst, &saddr_buf) == 0)
			NLA_PUT(skb, RTA_PREFSRC, 16, &saddr_buf);
	}

	if (rtnetlink_put_metrics(skb, rt->u.dst.metrics) < 0)
		goto nla_put_failure;

	if (rt->u.dst.neighbour)
		NLA_PUT(skb, RTA_GATEWAY, 16, &rt->u.dst.neighbour->primary_key);

	if (rt->u.dst.dev)
		NLA_PUT_U32(skb, RTA_OIF, rt->rt6i_dev->ifindex);

	NLA_PUT_U32(skb, RTA_PRIORITY, rt->rt6i_metric);
	ci.rta_lastuse = jiffies_to_clock_t(jiffies - rt->u.dst.lastuse);
	if (rt->rt6i_expires)
		ci.rta_expires = jiffies_to_clock_t(rt->rt6i_expires - jiffies);
	else
		ci.rta_expires = 0;
	ci.rta_used = rt->u.dst.__use;
	ci.rta_clntref = atomic_read(&rt->u.dst.__refcnt);
	ci.rta_error = rt->u.dst.error;
	ci.rta_id = 0;
	ci.rta_ts = 0;
	ci.rta_tsage = 0;
	NLA_PUT(skb, RTA_CACHEINFO, sizeof(ci), &ci);

	return nlmsg_end(skb, nlh);

nla_put_failure:
	return nlmsg_cancel(skb, nlh);
}

int rt6_dump_route(struct rt6_info *rt, void *p_arg)
{
	struct rt6_rtnl_dump_arg *arg = (struct rt6_rtnl_dump_arg *) p_arg;
	int prefix;

	if (nlmsg_len(arg->cb->nlh) >= sizeof(struct rtmsg)) {
		struct rtmsg *rtm = nlmsg_data(arg->cb->nlh);
		prefix = (rtm->rtm_flags & RTM_F_PREFIX) != 0;
	} else
		prefix = 0;

	return rt6_fill_node(arg->skb, rt, NULL, NULL, 0, RTM_NEWROUTE,
		     NETLINK_CB(arg->cb->skb).pid, arg->cb->nlh->nlmsg_seq,
		     prefix, NLM_F_MULTI);
}

int inet6_rtm_getroute(struct sk_buff *in_skb, struct nlmsghdr* nlh, void *arg)
{
	struct nlattr *tb[RTA_MAX+1];
	struct rt6_info *rt;
	struct sk_buff *skb;
	struct rtmsg *rtm;
	struct flowi fl;
	int err, iif = 0;

	err = nlmsg_parse(nlh, sizeof(*rtm), tb, RTA_MAX, rtm_ipv6_policy);
	if (err < 0)
		goto errout;

	err = -EINVAL;
	memset(&fl, 0, sizeof(fl));

	if (tb[RTA_SRC]) {
		if (nla_len(tb[RTA_SRC]) < sizeof(struct in6_addr))
			goto errout;

		ipv6_addr_copy(&fl.fl6_src, nla_data(tb[RTA_SRC]));
	}

	if (tb[RTA_DST]) {
		if (nla_len(tb[RTA_DST]) < sizeof(struct in6_addr))
			goto errout;

		ipv6_addr_copy(&fl.fl6_dst, nla_data(tb[RTA_DST]));
	}

	if (tb[RTA_IIF])
		iif = nla_get_u32(tb[RTA_IIF]);

	if (tb[RTA_OIF])
		fl.oif = nla_get_u32(tb[RTA_OIF]);

	if (iif) {
		struct net_device *dev;
		dev = __dev_get_by_index(iif);
		if (!dev) {
			err = -ENODEV;
			goto errout;
		}
	}

	skb = alloc_skb(NLMSG_GOODSIZE, GFP_KERNEL);
	if (skb == NULL) {
		err = -ENOBUFS;
		goto errout;
	}

	/* Reserve room for dummy headers, this skb can pass
	   through good chunk of routing engine.
	 */
	skb->mac.raw = skb->data;
	skb_reserve(skb, MAX_HEADER + sizeof(struct ipv6hdr));

	rt = (struct rt6_info*) ip6_route_output(NULL, &fl);
	skb->dst = &rt->u.dst;

	err = rt6_fill_node(skb, rt, &fl.fl6_dst, &fl.fl6_src, iif,
			    RTM_NEWROUTE, NETLINK_CB(in_skb).pid,
			    nlh->nlmsg_seq, 0, 0);
	if (err < 0) {
		kfree_skb(skb);
		goto errout;
	}

	err = rtnl_unicast(skb, NETLINK_CB(in_skb).pid);
errout:
	return err;
}

void inet6_rt_notify(int event, struct rt6_info *rt, struct nl_info *info)
{
	struct sk_buff *skb;
	u32 pid = 0, seq = 0;
	struct nlmsghdr *nlh = NULL;
	int err = -ENOBUFS;

	if (info) {
		pid = info->pid;
		nlh = info->nlh;
		if (nlh)
			seq = nlh->nlmsg_seq;
	}

	skb = nlmsg_new(rt6_nlmsg_size(), gfp_any());
	if (skb == NULL)
		goto errout;

	err = rt6_fill_node(skb, rt, NULL, NULL, 0, event, pid, seq, 0, 0);
	/* failure implies BUG in rt6_nlmsg_size() */
	BUG_ON(err < 0);

	err = rtnl_notify(skb, pid, RTNLGRP_IPV6_ROUTE, nlh, gfp_any());
errout:
	if (err < 0)
		rtnl_set_sk_err(RTNLGRP_IPV6_ROUTE, err);
}

/*
 *	/proc
 */

#ifdef CONFIG_PROC_FS

#define RT6_INFO_LEN (32 + 4 + 32 + 4 + 32 + 40 + 5 + 1)

struct rt6_proc_arg
{
	char *buffer;
	int offset;
	int length;
	int skip;
	int len;
};

static int rt6_info_route(struct rt6_info *rt, void *p_arg)
{
	struct rt6_proc_arg *arg = (struct rt6_proc_arg *) p_arg;

	if (arg->skip < arg->offset / RT6_INFO_LEN) {
		arg->skip++;
		return 0;
	}

	if (arg->len >= arg->length)
		return 0;

	arg->len += sprintf(arg->buffer + arg->len,
			    NIP6_SEQFMT " %02x ",
			    NIP6(rt->rt6i_dst.addr),
			    rt->rt6i_dst.plen);

#ifdef CONFIG_IPV6_SUBTREES
	arg->len += sprintf(arg->buffer + arg->len,
			    NIP6_SEQFMT " %02x ",
			    NIP6(rt->rt6i_src.addr),
			    rt->rt6i_src.plen);
#else
	arg->len += sprintf(arg->buffer + arg->len,
			    "00000000000000000000000000000000 00 ");
#endif

	if (rt->rt6i_nexthop) {
		arg->len += sprintf(arg->buffer + arg->len,
				    NIP6_SEQFMT,
				    NIP6(*((struct in6_addr *)rt->rt6i_nexthop->primary_key)));
	} else {
		arg->len += sprintf(arg->buffer + arg->len,
				    "00000000000000000000000000000000");
	}
	arg->len += sprintf(arg->buffer + arg->len,
			    " %08x %08x %08x %08x %8s\n",
			    rt->rt6i_metric, atomic_read(&rt->u.dst.__refcnt),
			    rt->u.dst.__use, rt->rt6i_flags, 
			    rt->rt6i_dev ? rt->rt6i_dev->name : "");
	return 0;
}

static int rt6_proc_info(char *buffer, char **start, off_t offset, int length)
{
	struct rt6_proc_arg arg = {
		.buffer = buffer,
		.offset = offset,
		.length = length,
	};

	fib6_clean_all(rt6_info_route, 0, &arg);

	*start = buffer;
	if (offset)
		*start += offset % RT6_INFO_LEN;

	arg.len -= offset % RT6_INFO_LEN;

	if (arg.len > length)
		arg.len = length;
	if (arg.len < 0)
		arg.len = 0;

	return arg.len;
}

static int rt6_stats_seq_show(struct seq_file *seq, void *v)
{
	seq_printf(seq, "%04x %04x %04x %04x %04x %04x %04x\n",
		      rt6_stats.fib_nodes, rt6_stats.fib_route_nodes,
		      rt6_stats.fib_rt_alloc, rt6_stats.fib_rt_entries,
		      rt6_stats.fib_rt_cache,
		      atomic_read(&ip6_dst_ops.entries),
		      rt6_stats.fib_discarded_routes);

	return 0;
}

static int rt6_stats_seq_open(struct inode *inode, struct file *file)
{
	return single_open(file, rt6_stats_seq_show, NULL);
}

static struct file_operations rt6_stats_seq_fops = {
	.owner	 = THIS_MODULE,
	.open	 = rt6_stats_seq_open,
	.read	 = seq_read,
	.llseek	 = seq_lseek,
	.release = single_release,
};
#endif	/* CONFIG_PROC_FS */

#ifdef CONFIG_SYSCTL

static int flush_delay;

static
int ipv6_sysctl_rtcache_flush(ctl_table *ctl, int write, struct file * filp,
			      void __user *buffer, size_t *lenp, loff_t *ppos)
{
	if (write) {
		proc_dointvec(ctl, write, filp, buffer, lenp, ppos);
		fib6_run_gc(flush_delay <= 0 ? ~0UL : (unsigned long)flush_delay);
		return 0;
	} else
		return -EINVAL;
}

ctl_table ipv6_route_table[] = {
        {
		.ctl_name	=	NET_IPV6_ROUTE_FLUSH, 
		.procname	=	"flush",
         	.data		=	&flush_delay,
		.maxlen		=	sizeof(int),
		.mode		=	0200,
         	.proc_handler	=	&ipv6_sysctl_rtcache_flush
	},
	{
		.ctl_name	=	NET_IPV6_ROUTE_GC_THRESH,
		.procname	=	"gc_thresh",
         	.data		=	&ip6_dst_ops.gc_thresh,
		.maxlen		=	sizeof(int),
		.mode		=	0644,
         	.proc_handler	=	&proc_dointvec,
	},
	{
		.ctl_name	=	NET_IPV6_ROUTE_MAX_SIZE,
		.procname	=	"max_size",
         	.data		=	&ip6_rt_max_size,
		.maxlen		=	sizeof(int),
		.mode		=	0644,
         	.proc_handler	=	&proc_dointvec,
	},
	{
		.ctl_name	=	NET_IPV6_ROUTE_GC_MIN_INTERVAL,
		.procname	=	"gc_min_interval",
         	.data		=	&ip6_rt_gc_min_interval,
		.maxlen		=	sizeof(int),
		.mode		=	0644,
         	.proc_handler	=	&proc_dointvec_jiffies,
		.strategy	=	&sysctl_jiffies,
	},
	{
		.ctl_name	=	NET_IPV6_ROUTE_GC_TIMEOUT,
		.procname	=	"gc_timeout",
         	.data		=	&ip6_rt_gc_timeout,
		.maxlen		=	sizeof(int),
		.mode		=	0644,
         	.proc_handler	=	&proc_dointvec_jiffies,
		.strategy	=	&sysctl_jiffies,
	},
	{
		.ctl_name	=	NET_IPV6_ROUTE_GC_INTERVAL,
		.procname	=	"gc_interval",
         	.data		=	&ip6_rt_gc_interval,
		.maxlen		=	sizeof(int),
		.mode		=	0644,
         	.proc_handler	=	&proc_dointvec_jiffies,
		.strategy	=	&sysctl_jiffies,
	},
	{
		.ctl_name	=	NET_IPV6_ROUTE_GC_ELASTICITY,
		.procname	=	"gc_elasticity",
         	.data		=	&ip6_rt_gc_elasticity,
		.maxlen		=	sizeof(int),
		.mode		=	0644,
         	.proc_handler	=	&proc_dointvec_jiffies,
		.strategy	=	&sysctl_jiffies,
	},
	{
		.ctl_name	=	NET_IPV6_ROUTE_MTU_EXPIRES,
		.procname	=	"mtu_expires",
         	.data		=	&ip6_rt_mtu_expires,
		.maxlen		=	sizeof(int),
		.mode		=	0644,
         	.proc_handler	=	&proc_dointvec_jiffies,
		.strategy	=	&sysctl_jiffies,
	},
	{
		.ctl_name	=	NET_IPV6_ROUTE_MIN_ADVMSS,
		.procname	=	"min_adv_mss",
         	.data		=	&ip6_rt_min_advmss,
		.maxlen		=	sizeof(int),
		.mode		=	0644,
         	.proc_handler	=	&proc_dointvec_jiffies,
		.strategy	=	&sysctl_jiffies,
	},
	{
		.ctl_name	=	NET_IPV6_ROUTE_GC_MIN_INTERVAL_MS,
		.procname	=	"gc_min_interval_ms",
         	.data		=	&ip6_rt_gc_min_interval,
		.maxlen		=	sizeof(int),
		.mode		=	0644,
         	.proc_handler	=	&proc_dointvec_ms_jiffies,
		.strategy	=	&sysctl_ms_jiffies,
	},
	{ .ctl_name = 0 }
};

#endif

void __init ip6_route_init(void)
{
	struct proc_dir_entry *p;

	ip6_dst_ops.kmem_cachep =
		kmem_cache_create("ip6_dst_cache", sizeof(struct rt6_info), 0,
				  SLAB_HWCACHE_ALIGN|SLAB_PANIC, NULL, NULL);
	fib6_init();
#ifdef 	CONFIG_PROC_FS
	p = proc_net_create("ipv6_route", 0, rt6_proc_info);
	if (p)
		p->owner = THIS_MODULE;

	proc_net_fops_create("rt6_stats", S_IRUGO, &rt6_stats_seq_fops);
#endif
#ifdef CONFIG_XFRM
	xfrm6_init();
#endif
#ifdef CONFIG_IPV6_MULTIPLE_TABLES
	fib6_rules_init();
#endif
}

void ip6_route_cleanup(void)
{
#ifdef CONFIG_IPV6_MULTIPLE_TABLES
	fib6_rules_cleanup();
#endif
#ifdef CONFIG_PROC_FS
	proc_net_remove("ipv6_route");
	proc_net_remove("rt6_stats");
#endif
#ifdef CONFIG_XFRM
	xfrm6_fini();
#endif
	rt6_ifdown(NULL);
	fib6_gc_cleanup();
	kmem_cache_destroy(ip6_dst_ops.kmem_cachep);
}
