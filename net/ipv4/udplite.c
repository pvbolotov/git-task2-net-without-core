/*
 *  UDPLITE     An implementation of the UDP-Lite protocol (RFC 3828).
 *
 *  Version:    $Id: udplite.c,v 1.25 2006/10/19 07:22:36 gerrit Exp $
 *
 *  Authors:    Gerrit Renker       <gerrit@erg.abdn.ac.uk>
 *
 *  Changes:
 *  Fixes:
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#include "udp_impl.h"
DEFINE_SNMP_STAT(struct udp_mib, udplite_statistics)	__read_mostly;

struct hlist_head 	udplite_hash[UDP_HTABLE_SIZE];

static int udplite_rcv(struct sk_buff *skb)
{
	return __udp4_lib_rcv(skb, udplite_hash, IPPROTO_UDPLITE);
}

static void udplite_err(struct sk_buff *skb, u32 info)
{
	__udp4_lib_err(skb, info, udplite_hash);
}

static	struct net_protocol udplite_protocol = {
	.handler	= udplite_rcv,
	.err_handler	= udplite_err,
	.no_policy	= 1,
};

DEFINE_PROTO_INUSE(udplite)

struct proto 	udplite_prot = {
	.name		   = "UDP-Lite",
	.owner		   = THIS_MODULE,
	.close		   = udp_lib_close,
	.connect	   = ip4_datagram_connect,
	.disconnect	   = udp_disconnect,
	.ioctl		   = udp_ioctl,
	.init		   = udplite_sk_init,
	.destroy	   = udp_destroy_sock,
	.setsockopt	   = udp_setsockopt,
	.getsockopt	   = udp_getsockopt,
	.sendmsg	   = udp_sendmsg,
	.recvmsg	   = udp_recvmsg,
	.sendpage	   = udp_sendpage,
	.backlog_rcv	   = udp_queue_rcv_skb,
	.hash		   = udp_lib_hash,
	.unhash		   = udp_lib_unhash,
	.get_port	   = udp_v4_get_port,
	.obj_size	   = sizeof(struct udp_sock),
	.h.udp_hash	   = udplite_hash,
#ifdef CONFIG_COMPAT
	.compat_setsockopt = compat_udp_setsockopt,
	.compat_getsockopt = compat_udp_getsockopt,
#endif
	REF_PROTO_INUSE(udplite)
};

static struct inet_protosw udplite4_protosw = {
	.type		=  SOCK_DGRAM,
	.protocol	=  IPPROTO_UDPLITE,
	.prot		=  &udplite_prot,
	.ops		=  &inet_dgram_ops,
	.capability	= -1,
	.no_check	=  0,		/* must checksum (RFC 3828) */
	.flags		=  INET_PROTOSW_PERMANENT,
};

#ifdef CONFIG_PROC_FS
static struct file_operations udplite4_seq_fops;
static struct udp_seq_afinfo udplite4_seq_afinfo = {
	.owner		= THIS_MODULE,
	.name		= "udplite",
	.family		= AF_INET,
	.hashtable	= udplite_hash,
	.seq_show	= udp4_seq_show,
	.seq_fops	= &udplite4_seq_fops,
};
#endif

void __init udplite4_register(void)
{
	if (proto_register(&udplite_prot, 1))
		goto out_register_err;

	if (inet_add_protocol(&udplite_protocol, IPPROTO_UDPLITE) < 0)
		goto out_unregister_proto;

	inet_register_protosw(&udplite4_protosw);

#ifdef CONFIG_PROC_FS
	if (udp_proc_register(&init_net, &udplite4_seq_afinfo))
		printk(KERN_ERR "%s: Cannot register /proc!\n", __func__);
#endif
	return;

out_unregister_proto:
	proto_unregister(&udplite_prot);
out_register_err:
	printk(KERN_CRIT "%s: Cannot add UDP-Lite protocol.\n", __func__);
}

EXPORT_SYMBOL(udplite_hash);
EXPORT_SYMBOL(udplite_prot);
