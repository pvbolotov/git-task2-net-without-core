/* 
 * xfrm_policy.c
 *
 * Changes:
 *	Mitsuru KANDA @USAGI
 * 	Kazunori MIYAZAWA @USAGI
 * 	Kunihiro Ishiguro <kunihiro@ipinfusion.com>
 * 		IPv6 support
 * 	Kazunori MIYAZAWA @USAGI
 * 	YOSHIFUJI Hideaki
 * 		Split up af-specific portion
 *	Derek Atkins <derek@ihtfp.com>		Add the post_input processor
 *
 */

#include <linux/slab.h>
#include <linux/kmod.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/notifier.h>
#include <linux/netdevice.h>
#include <linux/netfilter.h>
#include <linux/module.h>
#include <net/xfrm.h>
#include <net/ip.h>

DEFINE_MUTEX(xfrm_cfg_mutex);
EXPORT_SYMBOL(xfrm_cfg_mutex);

static DEFINE_RWLOCK(xfrm_policy_lock);

struct xfrm_policy *xfrm_policy_list[XFRM_POLICY_MAX*2];
EXPORT_SYMBOL(xfrm_policy_list);
#ifdef CONFIG_XFRM_SUB_POLICY
struct xfrm_policy *xfrm_policy_list_sub[XFRM_POLICY_MAX*2];
EXPORT_SYMBOL(xfrm_policy_list_sub);

#define XFRM_POLICY_LISTS(type) \
	((type == XFRM_POLICY_TYPE_SUB) ? xfrm_policy_list_sub : \
	 xfrm_policy_list)
#define XFRM_POLICY_LISTHEAD(type, dir) \
	((type == XFRM_POLICY_TYPE_SUB) ? xfrm_policy_list_sub[dir] : \
	 xfrm_policy_list[dir])
#define XFRM_POLICY_LISTHEADP(type, dir) \
	((type == XFRM_POLICY_TYPE_SUB) ? &xfrm_policy_list_sub[dir] : \
	 &xfrm_policy_list[dir])
#else
#define XFRM_POLICY_LISTS(type)              xfrm_policy_list
#define XFRM_POLICY_LISTHEAD(type, dif)      xfrm_policy_list[dir]
#define XFRM_POLICY_LISTHEADP(type, dif)     &xfrm_policy_list[dir]
#endif

static DEFINE_RWLOCK(xfrm_policy_afinfo_lock);
static struct xfrm_policy_afinfo *xfrm_policy_afinfo[NPROTO];

static kmem_cache_t *xfrm_dst_cache __read_mostly;

static struct work_struct xfrm_policy_gc_work;
static struct list_head xfrm_policy_gc_list =
	LIST_HEAD_INIT(xfrm_policy_gc_list);
static DEFINE_SPINLOCK(xfrm_policy_gc_lock);

static struct xfrm_policy_afinfo *xfrm_policy_get_afinfo(unsigned short family);
static void xfrm_policy_put_afinfo(struct xfrm_policy_afinfo *afinfo);
static struct xfrm_policy_afinfo *xfrm_policy_lock_afinfo(unsigned int family);
static void xfrm_policy_unlock_afinfo(struct xfrm_policy_afinfo *afinfo);

int xfrm_register_type(struct xfrm_type *type, unsigned short family)
{
	struct xfrm_policy_afinfo *afinfo = xfrm_policy_lock_afinfo(family);
	struct xfrm_type **typemap;
	int err = 0;

	if (unlikely(afinfo == NULL))
		return -EAFNOSUPPORT;
	typemap = afinfo->type_map;

	if (likely(typemap[type->proto] == NULL))
		typemap[type->proto] = type;
	else
		err = -EEXIST;
	xfrm_policy_unlock_afinfo(afinfo);
	return err;
}
EXPORT_SYMBOL(xfrm_register_type);

int xfrm_unregister_type(struct xfrm_type *type, unsigned short family)
{
	struct xfrm_policy_afinfo *afinfo = xfrm_policy_lock_afinfo(family);
	struct xfrm_type **typemap;
	int err = 0;

	if (unlikely(afinfo == NULL))
		return -EAFNOSUPPORT;
	typemap = afinfo->type_map;

	if (unlikely(typemap[type->proto] != type))
		err = -ENOENT;
	else
		typemap[type->proto] = NULL;
	xfrm_policy_unlock_afinfo(afinfo);
	return err;
}
EXPORT_SYMBOL(xfrm_unregister_type);

struct xfrm_type *xfrm_get_type(u8 proto, unsigned short family)
{
	struct xfrm_policy_afinfo *afinfo;
	struct xfrm_type **typemap;
	struct xfrm_type *type;
	int modload_attempted = 0;

retry:
	afinfo = xfrm_policy_get_afinfo(family);
	if (unlikely(afinfo == NULL))
		return NULL;
	typemap = afinfo->type_map;

	type = typemap[proto];
	if (unlikely(type && !try_module_get(type->owner)))
		type = NULL;
	if (!type && !modload_attempted) {
		xfrm_policy_put_afinfo(afinfo);
		request_module("xfrm-type-%d-%d",
			       (int) family, (int) proto);
		modload_attempted = 1;
		goto retry;
	}

	xfrm_policy_put_afinfo(afinfo);
	return type;
}

int xfrm_dst_lookup(struct xfrm_dst **dst, struct flowi *fl, 
		    unsigned short family)
{
	struct xfrm_policy_afinfo *afinfo = xfrm_policy_get_afinfo(family);
	int err = 0;

	if (unlikely(afinfo == NULL))
		return -EAFNOSUPPORT;

	if (likely(afinfo->dst_lookup != NULL))
		err = afinfo->dst_lookup(dst, fl);
	else
		err = -EINVAL;
	xfrm_policy_put_afinfo(afinfo);
	return err;
}
EXPORT_SYMBOL(xfrm_dst_lookup);

void xfrm_put_type(struct xfrm_type *type)
{
	module_put(type->owner);
}

int xfrm_register_mode(struct xfrm_mode *mode, int family)
{
	struct xfrm_policy_afinfo *afinfo;
	struct xfrm_mode **modemap;
	int err;

	if (unlikely(mode->encap >= XFRM_MODE_MAX))
		return -EINVAL;

	afinfo = xfrm_policy_lock_afinfo(family);
	if (unlikely(afinfo == NULL))
		return -EAFNOSUPPORT;

	err = -EEXIST;
	modemap = afinfo->mode_map;
	if (likely(modemap[mode->encap] == NULL)) {
		modemap[mode->encap] = mode;
		err = 0;
	}

	xfrm_policy_unlock_afinfo(afinfo);
	return err;
}
EXPORT_SYMBOL(xfrm_register_mode);

int xfrm_unregister_mode(struct xfrm_mode *mode, int family)
{
	struct xfrm_policy_afinfo *afinfo;
	struct xfrm_mode **modemap;
	int err;

	if (unlikely(mode->encap >= XFRM_MODE_MAX))
		return -EINVAL;

	afinfo = xfrm_policy_lock_afinfo(family);
	if (unlikely(afinfo == NULL))
		return -EAFNOSUPPORT;

	err = -ENOENT;
	modemap = afinfo->mode_map;
	if (likely(modemap[mode->encap] == mode)) {
		modemap[mode->encap] = NULL;
		err = 0;
	}

	xfrm_policy_unlock_afinfo(afinfo);
	return err;
}
EXPORT_SYMBOL(xfrm_unregister_mode);

struct xfrm_mode *xfrm_get_mode(unsigned int encap, int family)
{
	struct xfrm_policy_afinfo *afinfo;
	struct xfrm_mode *mode;
	int modload_attempted = 0;

	if (unlikely(encap >= XFRM_MODE_MAX))
		return NULL;

retry:
	afinfo = xfrm_policy_get_afinfo(family);
	if (unlikely(afinfo == NULL))
		return NULL;

	mode = afinfo->mode_map[encap];
	if (unlikely(mode && !try_module_get(mode->owner)))
		mode = NULL;
	if (!mode && !modload_attempted) {
		xfrm_policy_put_afinfo(afinfo);
		request_module("xfrm-mode-%d-%d", family, encap);
		modload_attempted = 1;
		goto retry;
	}

	xfrm_policy_put_afinfo(afinfo);
	return mode;
}

void xfrm_put_mode(struct xfrm_mode *mode)
{
	module_put(mode->owner);
}

static inline unsigned long make_jiffies(long secs)
{
	if (secs >= (MAX_SCHEDULE_TIMEOUT-1)/HZ)
		return MAX_SCHEDULE_TIMEOUT-1;
	else
	        return secs*HZ;
}

static void xfrm_policy_timer(unsigned long data)
{
	struct xfrm_policy *xp = (struct xfrm_policy*)data;
	unsigned long now = (unsigned long)xtime.tv_sec;
	long next = LONG_MAX;
	int warn = 0;
	int dir;

	read_lock(&xp->lock);

	if (xp->dead)
		goto out;

	dir = xfrm_policy_id2dir(xp->index);

	if (xp->lft.hard_add_expires_seconds) {
		long tmo = xp->lft.hard_add_expires_seconds +
			xp->curlft.add_time - now;
		if (tmo <= 0)
			goto expired;
		if (tmo < next)
			next = tmo;
	}
	if (xp->lft.hard_use_expires_seconds) {
		long tmo = xp->lft.hard_use_expires_seconds +
			(xp->curlft.use_time ? : xp->curlft.add_time) - now;
		if (tmo <= 0)
			goto expired;
		if (tmo < next)
			next = tmo;
	}
	if (xp->lft.soft_add_expires_seconds) {
		long tmo = xp->lft.soft_add_expires_seconds +
			xp->curlft.add_time - now;
		if (tmo <= 0) {
			warn = 1;
			tmo = XFRM_KM_TIMEOUT;
		}
		if (tmo < next)
			next = tmo;
	}
	if (xp->lft.soft_use_expires_seconds) {
		long tmo = xp->lft.soft_use_expires_seconds +
			(xp->curlft.use_time ? : xp->curlft.add_time) - now;
		if (tmo <= 0) {
			warn = 1;
			tmo = XFRM_KM_TIMEOUT;
		}
		if (tmo < next)
			next = tmo;
	}

	if (warn)
		km_policy_expired(xp, dir, 0, 0);
	if (next != LONG_MAX &&
	    !mod_timer(&xp->timer, jiffies + make_jiffies(next)))
		xfrm_pol_hold(xp);

out:
	read_unlock(&xp->lock);
	xfrm_pol_put(xp);
	return;

expired:
	read_unlock(&xp->lock);
	if (!xfrm_policy_delete(xp, dir))
		km_policy_expired(xp, dir, 1, 0);
	xfrm_pol_put(xp);
}


/* Allocate xfrm_policy. Not used here, it is supposed to be used by pfkeyv2
 * SPD calls.
 */

struct xfrm_policy *xfrm_policy_alloc(gfp_t gfp)
{
	struct xfrm_policy *policy;

	policy = kzalloc(sizeof(struct xfrm_policy), gfp);

	if (policy) {
		atomic_set(&policy->refcnt, 1);
		rwlock_init(&policy->lock);
		init_timer(&policy->timer);
		policy->timer.data = (unsigned long)policy;
		policy->timer.function = xfrm_policy_timer;
	}
	return policy;
}
EXPORT_SYMBOL(xfrm_policy_alloc);

/* Destroy xfrm_policy: descendant resources must be released to this moment. */

void __xfrm_policy_destroy(struct xfrm_policy *policy)
{
	BUG_ON(!policy->dead);

	BUG_ON(policy->bundles);

	if (del_timer(&policy->timer))
		BUG();

	security_xfrm_policy_free(policy);
	kfree(policy);
}
EXPORT_SYMBOL(__xfrm_policy_destroy);

static void xfrm_policy_gc_kill(struct xfrm_policy *policy)
{
	struct dst_entry *dst;

	while ((dst = policy->bundles) != NULL) {
		policy->bundles = dst->next;
		dst_free(dst);
	}

	if (del_timer(&policy->timer))
		atomic_dec(&policy->refcnt);

	if (atomic_read(&policy->refcnt) > 1)
		flow_cache_flush();

	xfrm_pol_put(policy);
}

static void xfrm_policy_gc_task(void *data)
{
	struct xfrm_policy *policy;
	struct list_head *entry, *tmp;
	struct list_head gc_list = LIST_HEAD_INIT(gc_list);

	spin_lock_bh(&xfrm_policy_gc_lock);
	list_splice_init(&xfrm_policy_gc_list, &gc_list);
	spin_unlock_bh(&xfrm_policy_gc_lock);

	list_for_each_safe(entry, tmp, &gc_list) {
		policy = list_entry(entry, struct xfrm_policy, list);
		xfrm_policy_gc_kill(policy);
	}
}

/* Rule must be locked. Release descentant resources, announce
 * entry dead. The rule must be unlinked from lists to the moment.
 */

static void xfrm_policy_kill(struct xfrm_policy *policy)
{
	int dead;

	write_lock_bh(&policy->lock);
	dead = policy->dead;
	policy->dead = 1;
	write_unlock_bh(&policy->lock);

	if (unlikely(dead)) {
		WARN_ON(1);
		return;
	}

	spin_lock(&xfrm_policy_gc_lock);
	list_add(&policy->list, &xfrm_policy_gc_list);
	spin_unlock(&xfrm_policy_gc_lock);

	schedule_work(&xfrm_policy_gc_work);
}

/* Generate new index... KAME seems to generate them ordered by cost
 * of an absolute inpredictability of ordering of rules. This will not pass. */
static u32 xfrm_gen_index(u8 type, int dir)
{
	u32 idx;
	struct xfrm_policy *p;
	static u32 idx_generator;

	for (;;) {
		idx = (idx_generator | dir);
		idx_generator += 8;
		if (idx == 0)
			idx = 8;
		for (p = XFRM_POLICY_LISTHEAD(type, dir); p; p = p->next) {
			if (p->index == idx)
				break;
		}
		if (!p)
			return idx;
	}
}

int xfrm_policy_insert(int dir, struct xfrm_policy *policy, int excl)
{
	struct xfrm_policy *pol, **p;
	struct xfrm_policy *delpol = NULL;
	struct xfrm_policy **newpos = NULL;
	struct dst_entry *gc_list;

	write_lock_bh(&xfrm_policy_lock);
	for (p = XFRM_POLICY_LISTHEADP(policy->type, dir); (pol=*p)!=NULL;) {
		if (!delpol && memcmp(&policy->selector, &pol->selector, sizeof(pol->selector)) == 0 &&
		    xfrm_sec_ctx_match(pol->security, policy->security)) {
			if (excl) {
				write_unlock_bh(&xfrm_policy_lock);
				return -EEXIST;
			}
			*p = pol->next;
			delpol = pol;
			if (policy->priority > pol->priority)
				continue;
		} else if (policy->priority >= pol->priority) {
			p = &pol->next;
			continue;
		}
		if (!newpos)
			newpos = p;
		if (delpol)
			break;
		p = &pol->next;
	}
	if (newpos)
		p = newpos;
	xfrm_pol_hold(policy);
	policy->next = *p;
	*p = policy;
	atomic_inc(&flow_cache_genid);
	policy->index = delpol ? delpol->index : xfrm_gen_index(policy->type, dir);
	policy->curlft.add_time = (unsigned long)xtime.tv_sec;
	policy->curlft.use_time = 0;
	if (!mod_timer(&policy->timer, jiffies + HZ))
		xfrm_pol_hold(policy);
	write_unlock_bh(&xfrm_policy_lock);

	if (delpol)
		xfrm_policy_kill(delpol);

	read_lock_bh(&xfrm_policy_lock);
	gc_list = NULL;
	for (policy = policy->next; policy; policy = policy->next) {
		struct dst_entry *dst;

		write_lock(&policy->lock);
		dst = policy->bundles;
		if (dst) {
			struct dst_entry *tail = dst;
			while (tail->next)
				tail = tail->next;
			tail->next = gc_list;
			gc_list = dst;

			policy->bundles = NULL;
		}
		write_unlock(&policy->lock);
	}
	read_unlock_bh(&xfrm_policy_lock);

	while (gc_list) {
		struct dst_entry *dst = gc_list;

		gc_list = dst->next;
		dst_free(dst);
	}

	return 0;
}
EXPORT_SYMBOL(xfrm_policy_insert);

struct xfrm_policy *xfrm_policy_bysel_ctx(u8 type, int dir,
					  struct xfrm_selector *sel,
					  struct xfrm_sec_ctx *ctx, int delete)
{
	struct xfrm_policy *pol, **p;

	write_lock_bh(&xfrm_policy_lock);
	for (p = XFRM_POLICY_LISTHEADP(type, dir); (pol=*p)!=NULL; p = &pol->next) {
		if ((memcmp(sel, &pol->selector, sizeof(*sel)) == 0) &&
		    (xfrm_sec_ctx_match(ctx, pol->security))) {
			xfrm_pol_hold(pol);
			if (delete)
				*p = pol->next;
			break;
		}
	}
	write_unlock_bh(&xfrm_policy_lock);

	if (pol && delete) {
		atomic_inc(&flow_cache_genid);
		xfrm_policy_kill(pol);
	}
	return pol;
}
EXPORT_SYMBOL(xfrm_policy_bysel_ctx);

struct xfrm_policy *xfrm_policy_byid(u8 type, int dir, u32 id, int delete)
{
	struct xfrm_policy *pol, **p;

	write_lock_bh(&xfrm_policy_lock);
	for (p = XFRM_POLICY_LISTHEADP(type, dir); (pol=*p)!=NULL; p = &pol->next) {
		if (pol->index == id) {
			xfrm_pol_hold(pol);
			if (delete)
				*p = pol->next;
			break;
		}
	}
	write_unlock_bh(&xfrm_policy_lock);

	if (pol && delete) {
		atomic_inc(&flow_cache_genid);
		xfrm_policy_kill(pol);
	}
	return pol;
}
EXPORT_SYMBOL(xfrm_policy_byid);

void xfrm_policy_flush(u8 type)
{
	struct xfrm_policy *xp;
	struct xfrm_policy **p_list = XFRM_POLICY_LISTS(type);
	int dir;

	write_lock_bh(&xfrm_policy_lock);
	for (dir = 0; dir < XFRM_POLICY_MAX; dir++) {
		while ((xp = p_list[dir]) != NULL) {
			p_list[dir] = xp->next;
			write_unlock_bh(&xfrm_policy_lock);

			xfrm_policy_kill(xp);

			write_lock_bh(&xfrm_policy_lock);
		}
	}
	atomic_inc(&flow_cache_genid);
	write_unlock_bh(&xfrm_policy_lock);
}
EXPORT_SYMBOL(xfrm_policy_flush);

int xfrm_policy_walk(u8 type, int (*func)(struct xfrm_policy *, int, int, void*),
		     void *data)
{
	struct xfrm_policy *xp;
	int dir;
	int count = 0;
	int error = 0;

	read_lock_bh(&xfrm_policy_lock);
	for (dir = 0; dir < 2*XFRM_POLICY_MAX; dir++) {
		for (xp = XFRM_POLICY_LISTHEAD(type, dir); xp; xp = xp->next)
			count++;
	}

	if (count == 0) {
		error = -ENOENT;
		goto out;
	}

	for (dir = 0; dir < 2*XFRM_POLICY_MAX; dir++) {
		for (xp = XFRM_POLICY_LISTHEAD(type, dir); xp; xp = xp->next) {
			error = func(xp, dir%XFRM_POLICY_MAX, --count, data);
			if (error)
				goto out;
		}
	}

out:
	read_unlock_bh(&xfrm_policy_lock);
	return error;
}
EXPORT_SYMBOL(xfrm_policy_walk);

/* Find policy to apply to this flow. */

static struct xfrm_policy *xfrm_policy_lookup_bytype(u8 type, struct flowi *fl,
						     u16 family, u8 dir)
{
	struct xfrm_policy *pol;

	read_lock_bh(&xfrm_policy_lock);
	for (pol = XFRM_POLICY_LISTHEAD(type, dir); pol; pol = pol->next) {
		struct xfrm_selector *sel = &pol->selector;
		int match;

		if (pol->family != family)
			continue;

		match = xfrm_selector_match(sel, fl, family);

		if (match) {
 			if (!security_xfrm_policy_lookup(pol, fl->secid, dir)) {
				xfrm_pol_hold(pol);
				break;
			}
		}
	}
	read_unlock_bh(&xfrm_policy_lock);

	return pol;
}

static void xfrm_policy_lookup(struct flowi *fl, u16 family, u8 dir,
			       void **objp, atomic_t **obj_refp)
{
	struct xfrm_policy *pol;

#ifdef CONFIG_XFRM_SUB_POLICY
	pol = xfrm_policy_lookup_bytype(XFRM_POLICY_TYPE_SUB, fl, family, dir);
	if (pol)
		goto end;
#endif
	pol = xfrm_policy_lookup_bytype(XFRM_POLICY_TYPE_MAIN, fl, family, dir);

#ifdef CONFIG_XFRM_SUB_POLICY
 end:
#endif
	if ((*objp = (void *) pol) != NULL)
		*obj_refp = &pol->refcnt;
}

static inline int policy_to_flow_dir(int dir)
{
	if (XFRM_POLICY_IN == FLOW_DIR_IN &&
 	    XFRM_POLICY_OUT == FLOW_DIR_OUT &&
 	    XFRM_POLICY_FWD == FLOW_DIR_FWD)
 		return dir;
 	switch (dir) {
 	default:
 	case XFRM_POLICY_IN:
 		return FLOW_DIR_IN;
 	case XFRM_POLICY_OUT:
 		return FLOW_DIR_OUT;
 	case XFRM_POLICY_FWD:
 		return FLOW_DIR_FWD;
	};
}

static struct xfrm_policy *xfrm_sk_policy_lookup(struct sock *sk, int dir, struct flowi *fl)
{
	struct xfrm_policy *pol;

	read_lock_bh(&xfrm_policy_lock);
	if ((pol = sk->sk_policy[dir]) != NULL) {
 		int match = xfrm_selector_match(&pol->selector, fl,
						sk->sk_family);
 		int err = 0;

		if (match)
		  err = security_xfrm_policy_lookup(pol, fl->secid, policy_to_flow_dir(dir));

 		if (match && !err)
			xfrm_pol_hold(pol);
		else
			pol = NULL;
	}
	read_unlock_bh(&xfrm_policy_lock);
	return pol;
}

static void __xfrm_policy_link(struct xfrm_policy *pol, int dir)
{
	struct xfrm_policy **p_list = XFRM_POLICY_LISTS(pol->type);

	pol->next = p_list[dir];
	p_list[dir] = pol;
	xfrm_pol_hold(pol);
}

static struct xfrm_policy *__xfrm_policy_unlink(struct xfrm_policy *pol,
						int dir)
{
	struct xfrm_policy **polp;

	for (polp = XFRM_POLICY_LISTHEADP(pol->type, dir);
	     *polp != NULL; polp = &(*polp)->next) {
		if (*polp == pol) {
			*polp = pol->next;
			return pol;
		}
	}
	return NULL;
}

int xfrm_policy_delete(struct xfrm_policy *pol, int dir)
{
	write_lock_bh(&xfrm_policy_lock);
	pol = __xfrm_policy_unlink(pol, dir);
	write_unlock_bh(&xfrm_policy_lock);
	if (pol) {
		if (dir < XFRM_POLICY_MAX)
			atomic_inc(&flow_cache_genid);
		xfrm_policy_kill(pol);
		return 0;
	}
	return -ENOENT;
}
EXPORT_SYMBOL(xfrm_policy_delete);

int xfrm_sk_policy_insert(struct sock *sk, int dir, struct xfrm_policy *pol)
{
	struct xfrm_policy *old_pol;

#ifdef CONFIG_XFRM_SUB_POLICY
	if (pol && pol->type != XFRM_POLICY_TYPE_MAIN)
		return -EINVAL;
#endif

	write_lock_bh(&xfrm_policy_lock);
	old_pol = sk->sk_policy[dir];
	sk->sk_policy[dir] = pol;
	if (pol) {
		pol->curlft.add_time = (unsigned long)xtime.tv_sec;
		pol->index = xfrm_gen_index(pol->type, XFRM_POLICY_MAX+dir);
		__xfrm_policy_link(pol, XFRM_POLICY_MAX+dir);
	}
	if (old_pol)
		__xfrm_policy_unlink(old_pol, XFRM_POLICY_MAX+dir);
	write_unlock_bh(&xfrm_policy_lock);

	if (old_pol) {
		xfrm_policy_kill(old_pol);
	}
	return 0;
}

static struct xfrm_policy *clone_policy(struct xfrm_policy *old, int dir)
{
	struct xfrm_policy *newp = xfrm_policy_alloc(GFP_ATOMIC);

	if (newp) {
		newp->selector = old->selector;
		if (security_xfrm_policy_clone(old, newp)) {
			kfree(newp);
			return NULL;  /* ENOMEM */
		}
		newp->lft = old->lft;
		newp->curlft = old->curlft;
		newp->action = old->action;
		newp->flags = old->flags;
		newp->xfrm_nr = old->xfrm_nr;
		newp->index = old->index;
		newp->type = old->type;
		memcpy(newp->xfrm_vec, old->xfrm_vec,
		       newp->xfrm_nr*sizeof(struct xfrm_tmpl));
		write_lock_bh(&xfrm_policy_lock);
		__xfrm_policy_link(newp, XFRM_POLICY_MAX+dir);
		write_unlock_bh(&xfrm_policy_lock);
		xfrm_pol_put(newp);
	}
	return newp;
}

int __xfrm_sk_clone_policy(struct sock *sk)
{
	struct xfrm_policy *p0 = sk->sk_policy[0],
			   *p1 = sk->sk_policy[1];

	sk->sk_policy[0] = sk->sk_policy[1] = NULL;
	if (p0 && (sk->sk_policy[0] = clone_policy(p0, 0)) == NULL)
		return -ENOMEM;
	if (p1 && (sk->sk_policy[1] = clone_policy(p1, 1)) == NULL)
		return -ENOMEM;
	return 0;
}

/* Resolve list of templates for the flow, given policy. */

static int
xfrm_tmpl_resolve_one(struct xfrm_policy *policy, struct flowi *fl,
		      struct xfrm_state **xfrm,
		      unsigned short family)
{
	int nx;
	int i, error;
	xfrm_address_t *daddr = xfrm_flowi_daddr(fl, family);
	xfrm_address_t *saddr = xfrm_flowi_saddr(fl, family);

	for (nx=0, i = 0; i < policy->xfrm_nr; i++) {
		struct xfrm_state *x;
		xfrm_address_t *remote = daddr;
		xfrm_address_t *local  = saddr;
		struct xfrm_tmpl *tmpl = &policy->xfrm_vec[i];

		if (tmpl->mode == XFRM_MODE_TUNNEL) {
			remote = &tmpl->id.daddr;
			local = &tmpl->saddr;
		}

		x = xfrm_state_find(remote, local, fl, tmpl, policy, &error, family);

		if (x && x->km.state == XFRM_STATE_VALID) {
			xfrm[nx++] = x;
			daddr = remote;
			saddr = local;
			continue;
		}
		if (x) {
			error = (x->km.state == XFRM_STATE_ERROR ?
				 -EINVAL : -EAGAIN);
			xfrm_state_put(x);
		}

		if (!tmpl->optional)
			goto fail;
	}
	return nx;

fail:
	for (nx--; nx>=0; nx--)
		xfrm_state_put(xfrm[nx]);
	return error;
}

static int
xfrm_tmpl_resolve(struct xfrm_policy **pols, int npols, struct flowi *fl,
		  struct xfrm_state **xfrm,
		  unsigned short family)
{
	struct xfrm_state *tp[XFRM_MAX_DEPTH];
	struct xfrm_state **tpp = (npols > 1) ? tp : xfrm;
	int cnx = 0;
	int error;
	int ret;
	int i;

	for (i = 0; i < npols; i++) {
		if (cnx + pols[i]->xfrm_nr >= XFRM_MAX_DEPTH) {
			error = -ENOBUFS;
			goto fail;
		}

		ret = xfrm_tmpl_resolve_one(pols[i], fl, &tpp[cnx], family);
		if (ret < 0) {
			error = ret;
			goto fail;
		} else
			cnx += ret;
	}

	/* found states are sorted for outbound processing */
	if (npols > 1)
		xfrm_state_sort(xfrm, tpp, cnx, family);

	return cnx;

 fail:
	for (cnx--; cnx>=0; cnx--)
		xfrm_state_put(tpp[cnx]);
	return error;

}

/* Check that the bundle accepts the flow and its components are
 * still valid.
 */

static struct dst_entry *
xfrm_find_bundle(struct flowi *fl, struct xfrm_policy *policy, unsigned short family)
{
	struct dst_entry *x;
	struct xfrm_policy_afinfo *afinfo = xfrm_policy_get_afinfo(family);
	if (unlikely(afinfo == NULL))
		return ERR_PTR(-EINVAL);
	x = afinfo->find_bundle(fl, policy);
	xfrm_policy_put_afinfo(afinfo);
	return x;
}

/* Allocate chain of dst_entry's, attach known xfrm's, calculate
 * all the metrics... Shortly, bundle a bundle.
 */

static int
xfrm_bundle_create(struct xfrm_policy *policy, struct xfrm_state **xfrm, int nx,
		   struct flowi *fl, struct dst_entry **dst_p,
		   unsigned short family)
{
	int err;
	struct xfrm_policy_afinfo *afinfo = xfrm_policy_get_afinfo(family);
	if (unlikely(afinfo == NULL))
		return -EINVAL;
	err = afinfo->bundle_create(policy, xfrm, nx, fl, dst_p);
	xfrm_policy_put_afinfo(afinfo);
	return err;
}


static int stale_bundle(struct dst_entry *dst);

/* Main function: finds/creates a bundle for given flow.
 *
 * At the moment we eat a raw IP route. Mostly to speed up lookups
 * on interfaces with disabled IPsec.
 */
int xfrm_lookup(struct dst_entry **dst_p, struct flowi *fl,
		struct sock *sk, int flags)
{
	struct xfrm_policy *policy;
	struct xfrm_policy *pols[XFRM_POLICY_TYPE_MAX];
	int npols;
	int pol_dead;
	int xfrm_nr;
	int pi;
	struct xfrm_state *xfrm[XFRM_MAX_DEPTH];
	struct dst_entry *dst, *dst_orig = *dst_p;
	int nx = 0;
	int err;
	u32 genid;
	u16 family;
	u8 dir = policy_to_flow_dir(XFRM_POLICY_OUT);

restart:
	genid = atomic_read(&flow_cache_genid);
	policy = NULL;
	for (pi = 0; pi < ARRAY_SIZE(pols); pi++)
		pols[pi] = NULL;
	npols = 0;
	pol_dead = 0;
	xfrm_nr = 0;

	if (sk && sk->sk_policy[1])
		policy = xfrm_sk_policy_lookup(sk, XFRM_POLICY_OUT, fl);

	if (!policy) {
		/* To accelerate a bit...  */
		if ((dst_orig->flags & DST_NOXFRM) || xfrm_policy_lists_empty(XFRM_POLICY_OUT))
			return 0;

		policy = flow_cache_lookup(fl, dst_orig->ops->family,
					   dir, xfrm_policy_lookup);
	}

	if (!policy)
		return 0;

	family = dst_orig->ops->family;
	policy->curlft.use_time = (unsigned long)xtime.tv_sec;
	pols[0] = policy;
	npols ++;
	xfrm_nr += pols[0]->xfrm_nr;

	switch (policy->action) {
	case XFRM_POLICY_BLOCK:
		/* Prohibit the flow */
		err = -EPERM;
		goto error;

	case XFRM_POLICY_ALLOW:
#ifndef CONFIG_XFRM_SUB_POLICY
		if (policy->xfrm_nr == 0) {
			/* Flow passes not transformed. */
			xfrm_pol_put(policy);
			return 0;
		}
#endif

		/* Try to find matching bundle.
		 *
		 * LATER: help from flow cache. It is optional, this
		 * is required only for output policy.
		 */
		dst = xfrm_find_bundle(fl, policy, family);
		if (IS_ERR(dst)) {
			err = PTR_ERR(dst);
			goto error;
		}

		if (dst)
			break;

#ifdef CONFIG_XFRM_SUB_POLICY
		if (pols[0]->type != XFRM_POLICY_TYPE_MAIN) {
			pols[1] = xfrm_policy_lookup_bytype(XFRM_POLICY_TYPE_MAIN,
							    fl, family,
							    XFRM_POLICY_OUT);
			if (pols[1]) {
				if (pols[1]->action == XFRM_POLICY_BLOCK) {
					err = -EPERM;
					goto error;
				}
				npols ++;
				xfrm_nr += pols[1]->xfrm_nr;
			}
		}

		/*
		 * Because neither flowi nor bundle information knows about
		 * transformation template size. On more than one policy usage
		 * we can realize whether all of them is bypass or not after
		 * they are searched. See above not-transformed bypass
		 * is surrounded by non-sub policy configuration, too.
		 */
		if (xfrm_nr == 0) {
			/* Flow passes not transformed. */
			xfrm_pols_put(pols, npols);
			return 0;
		}

#endif
		nx = xfrm_tmpl_resolve(pols, npols, fl, xfrm, family);

		if (unlikely(nx<0)) {
			err = nx;
			if (err == -EAGAIN && flags) {
				DECLARE_WAITQUEUE(wait, current);

				add_wait_queue(&km_waitq, &wait);
				set_current_state(TASK_INTERRUPTIBLE);
				schedule();
				set_current_state(TASK_RUNNING);
				remove_wait_queue(&km_waitq, &wait);

				nx = xfrm_tmpl_resolve(pols, npols, fl, xfrm, family);

				if (nx == -EAGAIN && signal_pending(current)) {
					err = -ERESTART;
					goto error;
				}
				if (nx == -EAGAIN ||
				    genid != atomic_read(&flow_cache_genid)) {
					xfrm_pols_put(pols, npols);
					goto restart;
				}
				err = nx;
			}
			if (err < 0)
				goto error;
		}
		if (nx == 0) {
			/* Flow passes not transformed. */
			xfrm_pols_put(pols, npols);
			return 0;
		}

		dst = dst_orig;
		err = xfrm_bundle_create(policy, xfrm, nx, fl, &dst, family);

		if (unlikely(err)) {
			int i;
			for (i=0; i<nx; i++)
				xfrm_state_put(xfrm[i]);
			goto error;
		}

		for (pi = 0; pi < npols; pi++) {
			read_lock_bh(&pols[pi]->lock);
			pol_dead |= pols[pi]->dead;
			read_unlock_bh(&pols[pi]->lock);
		}

		write_lock_bh(&policy->lock);
		if (unlikely(pol_dead || stale_bundle(dst))) {
			/* Wow! While we worked on resolving, this
			 * policy has gone. Retry. It is not paranoia,
			 * we just cannot enlist new bundle to dead object.
			 * We can't enlist stable bundles either.
			 */
			write_unlock_bh(&policy->lock);
			if (dst)
				dst_free(dst);

			err = -EHOSTUNREACH;
			goto error;
		}
		dst->next = policy->bundles;
		policy->bundles = dst;
		dst_hold(dst);
		write_unlock_bh(&policy->lock);
	}
	*dst_p = dst;
	dst_release(dst_orig);
 	xfrm_pols_put(pols, npols);
	return 0;

error:
	dst_release(dst_orig);
	xfrm_pols_put(pols, npols);
	*dst_p = NULL;
	return err;
}
EXPORT_SYMBOL(xfrm_lookup);

static inline int
xfrm_secpath_reject(int idx, struct sk_buff *skb, struct flowi *fl)
{
	struct xfrm_state *x;
	int err;

	if (!skb->sp || idx < 0 || idx >= skb->sp->len)
		return 0;
	x = skb->sp->xvec[idx];
	if (!x->type->reject)
		return 0;
	xfrm_state_hold(x);
	err = x->type->reject(x, skb, fl);
	xfrm_state_put(x);
	return err;
}

/* When skb is transformed back to its "native" form, we have to
 * check policy restrictions. At the moment we make this in maximally
 * stupid way. Shame on me. :-) Of course, connected sockets must
 * have policy cached at them.
 */

static inline int
xfrm_state_ok(struct xfrm_tmpl *tmpl, struct xfrm_state *x, 
	      unsigned short family)
{
	if (xfrm_state_kern(x))
		return tmpl->optional && !xfrm_state_addr_cmp(tmpl, x, family);
	return	x->id.proto == tmpl->id.proto &&
		(x->id.spi == tmpl->id.spi || !tmpl->id.spi) &&
		(x->props.reqid == tmpl->reqid || !tmpl->reqid) &&
		x->props.mode == tmpl->mode &&
		((tmpl->aalgos & (1<<x->props.aalgo)) ||
		 !(xfrm_id_proto_match(tmpl->id.proto, IPSEC_PROTO_ANY))) &&
		!(x->props.mode != XFRM_MODE_TRANSPORT &&
		  xfrm_state_addr_cmp(tmpl, x, family));
}

/*
 * 0 or more than 0 is returned when validation is succeeded (either bypass
 * because of optional transport mode, or next index of the mathced secpath
 * state with the template.
 * -1 is returned when no matching template is found.
 * Otherwise "-2 - errored_index" is returned.
 */
static inline int
xfrm_policy_ok(struct xfrm_tmpl *tmpl, struct sec_path *sp, int start,
	       unsigned short family)
{
	int idx = start;

	if (tmpl->optional) {
		if (tmpl->mode == XFRM_MODE_TRANSPORT)
			return start;
	} else
		start = -1;
	for (; idx < sp->len; idx++) {
		if (xfrm_state_ok(tmpl, sp->xvec[idx], family))
			return ++idx;
		if (sp->xvec[idx]->props.mode != XFRM_MODE_TRANSPORT) {
			if (start == -1)
				start = -2-idx;
			break;
		}
	}
	return start;
}

int
xfrm_decode_session(struct sk_buff *skb, struct flowi *fl, unsigned short family)
{
	struct xfrm_policy_afinfo *afinfo = xfrm_policy_get_afinfo(family);
	int err;

	if (unlikely(afinfo == NULL))
		return -EAFNOSUPPORT;

	afinfo->decode_session(skb, fl);
	err = security_xfrm_decode_session(skb, &fl->secid);
	xfrm_policy_put_afinfo(afinfo);
	return err;
}
EXPORT_SYMBOL(xfrm_decode_session);

static inline int secpath_has_nontransport(struct sec_path *sp, int k, int *idxp)
{
	for (; k < sp->len; k++) {
		if (sp->xvec[k]->props.mode != XFRM_MODE_TRANSPORT) {
			if (idxp)
				*idxp = k;
			return 1;
		}
	}

	return 0;
}

int __xfrm_policy_check(struct sock *sk, int dir, struct sk_buff *skb, 
			unsigned short family)
{
	struct xfrm_policy *pol;
	struct xfrm_policy *pols[XFRM_POLICY_TYPE_MAX];
	int npols = 0;
	int xfrm_nr;
	int pi;
	struct flowi fl;
	u8 fl_dir = policy_to_flow_dir(dir);
	int xerr_idx = -1;
	int *xerr_idxp = &xerr_idx;

	if (xfrm_decode_session(skb, &fl, family) < 0)
		return 0;
	nf_nat_decode_session(skb, &fl, family);

	/* First, check used SA against their selectors. */
	if (skb->sp) {
		int i;

		for (i=skb->sp->len-1; i>=0; i--) {
			struct xfrm_state *x = skb->sp->xvec[i];
			if (!xfrm_selector_match(&x->sel, &fl, family))
				return 0;
		}
	}

	pol = NULL;
	if (sk && sk->sk_policy[dir])
		pol = xfrm_sk_policy_lookup(sk, dir, &fl);

	if (!pol)
		pol = flow_cache_lookup(&fl, family, fl_dir,
					xfrm_policy_lookup);

	if (!pol) {
		if (skb->sp && secpath_has_nontransport(skb->sp, 0, xerr_idxp)) {
			xfrm_secpath_reject(xerr_idx, skb, &fl);
			return 0;
		}
		return 1;
	}

	pol->curlft.use_time = (unsigned long)xtime.tv_sec;

	pols[0] = pol;
	npols ++;
#ifdef CONFIG_XFRM_SUB_POLICY
	if (pols[0]->type != XFRM_POLICY_TYPE_MAIN) {
		pols[1] = xfrm_policy_lookup_bytype(XFRM_POLICY_TYPE_MAIN,
						    &fl, family,
						    XFRM_POLICY_IN);
		if (pols[1]) {
			pols[1]->curlft.use_time = (unsigned long)xtime.tv_sec;
			npols ++;
		}
	}
#endif

	if (pol->action == XFRM_POLICY_ALLOW) {
		struct sec_path *sp;
		static struct sec_path dummy;
		struct xfrm_tmpl *tp[XFRM_MAX_DEPTH];
		struct xfrm_tmpl *stp[XFRM_MAX_DEPTH];
		struct xfrm_tmpl **tpp = tp;
		int ti = 0;
		int i, k;

		if ((sp = skb->sp) == NULL)
			sp = &dummy;

		for (pi = 0; pi < npols; pi++) {
			if (pols[pi] != pol &&
			    pols[pi]->action != XFRM_POLICY_ALLOW)
				goto reject;
			if (ti + pols[pi]->xfrm_nr >= XFRM_MAX_DEPTH)
				goto reject_error;
			for (i = 0; i < pols[pi]->xfrm_nr; i++)
				tpp[ti++] = &pols[pi]->xfrm_vec[i];
		}
		xfrm_nr = ti;
		if (npols > 1) {
			xfrm_tmpl_sort(stp, tpp, xfrm_nr, family);
			tpp = stp;
		}

		/* For each tunnel xfrm, find the first matching tmpl.
		 * For each tmpl before that, find corresponding xfrm.
		 * Order is _important_. Later we will implement
		 * some barriers, but at the moment barriers
		 * are implied between each two transformations.
		 */
		for (i = xfrm_nr-1, k = 0; i >= 0; i--) {
			k = xfrm_policy_ok(tpp[i], sp, k, family);
			if (k < 0) {
				if (k < -1 && xerr_idxp)
					*xerr_idxp = -(2+k);
				goto reject;
			}
		}

		if (secpath_has_nontransport(sp, k, xerr_idxp))
			goto reject;

		xfrm_pols_put(pols, npols);
		return 1;
	}

reject:
	xfrm_secpath_reject(xerr_idx, skb, &fl);
reject_error:
	xfrm_pols_put(pols, npols);
	return 0;
}
EXPORT_SYMBOL(__xfrm_policy_check);

int __xfrm_route_forward(struct sk_buff *skb, unsigned short family)
{
	struct flowi fl;

	if (xfrm_decode_session(skb, &fl, family) < 0)
		return 0;

	return xfrm_lookup(&skb->dst, &fl, NULL, 0) == 0;
}
EXPORT_SYMBOL(__xfrm_route_forward);

/* Optimize later using cookies and generation ids. */

static struct dst_entry *xfrm_dst_check(struct dst_entry *dst, u32 cookie)
{
	/* Code (such as __xfrm4_bundle_create()) sets dst->obsolete
	 * to "-1" to force all XFRM destinations to get validated by
	 * dst_ops->check on every use.  We do this because when a
	 * normal route referenced by an XFRM dst is obsoleted we do
	 * not go looking around for all parent referencing XFRM dsts
	 * so that we can invalidate them.  It is just too much work.
	 * Instead we make the checks here on every use.  For example:
	 *
	 *	XFRM dst A --> IPv4 dst X
	 *
	 * X is the "xdst->route" of A (X is also the "dst->path" of A
	 * in this example).  If X is marked obsolete, "A" will not
	 * notice.  That's what we are validating here via the
	 * stale_bundle() check.
	 *
	 * When a policy's bundle is pruned, we dst_free() the XFRM
	 * dst which causes it's ->obsolete field to be set to a
	 * positive non-zero integer.  If an XFRM dst has been pruned
	 * like this, we want to force a new route lookup.
	 */
	if (dst->obsolete < 0 && !stale_bundle(dst))
		return dst;

	return NULL;
}

static int stale_bundle(struct dst_entry *dst)
{
	return !xfrm_bundle_ok((struct xfrm_dst *)dst, NULL, AF_UNSPEC, 0);
}

void xfrm_dst_ifdown(struct dst_entry *dst, struct net_device *dev)
{
	while ((dst = dst->child) && dst->xfrm && dst->dev == dev) {
		dst->dev = &loopback_dev;
		dev_hold(&loopback_dev);
		dev_put(dev);
	}
}
EXPORT_SYMBOL(xfrm_dst_ifdown);

static void xfrm_link_failure(struct sk_buff *skb)
{
	/* Impossible. Such dst must be popped before reaches point of failure. */
	return;
}

static struct dst_entry *xfrm_negative_advice(struct dst_entry *dst)
{
	if (dst) {
		if (dst->obsolete) {
			dst_release(dst);
			dst = NULL;
		}
	}
	return dst;
}

static void xfrm_prune_bundles(int (*func)(struct dst_entry *))
{
	int i;
	struct xfrm_policy *pol;
	struct dst_entry *dst, **dstp, *gc_list = NULL;

	read_lock_bh(&xfrm_policy_lock);
	for (i=0; i<2*XFRM_POLICY_MAX; i++) {
#ifdef CONFIG_XFRM_SUB_POLICY
		for (pol = xfrm_policy_list_sub[i]; pol; pol = pol->next) {
			write_lock(&pol->lock);
			dstp = &pol->bundles;
			while ((dst=*dstp) != NULL) {
				if (func(dst)) {
					*dstp = dst->next;
					dst->next = gc_list;
					gc_list = dst;
				} else {
					dstp = &dst->next;
				}
			}
			write_unlock(&pol->lock);
		}

#endif
		for (pol = xfrm_policy_list[i]; pol; pol = pol->next) {
			write_lock(&pol->lock);
			dstp = &pol->bundles;
			while ((dst=*dstp) != NULL) {
				if (func(dst)) {
					*dstp = dst->next;
					dst->next = gc_list;
					gc_list = dst;
				} else {
					dstp = &dst->next;
				}
			}
			write_unlock(&pol->lock);
		}
	}
	read_unlock_bh(&xfrm_policy_lock);

	while (gc_list) {
		dst = gc_list;
		gc_list = dst->next;
		dst_free(dst);
	}
}

static int unused_bundle(struct dst_entry *dst)
{
	return !atomic_read(&dst->__refcnt);
}

static void __xfrm_garbage_collect(void)
{
	xfrm_prune_bundles(unused_bundle);
}

int xfrm_flush_bundles(void)
{
	xfrm_prune_bundles(stale_bundle);
	return 0;
}

void xfrm_init_pmtu(struct dst_entry *dst)
{
	do {
		struct xfrm_dst *xdst = (struct xfrm_dst *)dst;
		u32 pmtu, route_mtu_cached;

		pmtu = dst_mtu(dst->child);
		xdst->child_mtu_cached = pmtu;

		pmtu = xfrm_state_mtu(dst->xfrm, pmtu);

		route_mtu_cached = dst_mtu(xdst->route);
		xdst->route_mtu_cached = route_mtu_cached;

		if (pmtu > route_mtu_cached)
			pmtu = route_mtu_cached;

		dst->metrics[RTAX_MTU-1] = pmtu;
	} while ((dst = dst->next));
}

EXPORT_SYMBOL(xfrm_init_pmtu);

/* Check that the bundle accepts the flow and its components are
 * still valid.
 */

int xfrm_bundle_ok(struct xfrm_dst *first, struct flowi *fl, int family, int strict)
{
	struct dst_entry *dst = &first->u.dst;
	struct xfrm_dst *last;
	u32 mtu;

	if (!dst_check(dst->path, ((struct xfrm_dst *)dst)->path_cookie) ||
	    (dst->dev && !netif_running(dst->dev)))
		return 0;

	last = NULL;

	do {
		struct xfrm_dst *xdst = (struct xfrm_dst *)dst;

		if (fl && !xfrm_selector_match(&dst->xfrm->sel, fl, family))
			return 0;
		if (fl && !security_xfrm_flow_state_match(fl, dst->xfrm))
			return 0;
		if (dst->xfrm->km.state != XFRM_STATE_VALID)
			return 0;
		if (xdst->genid != dst->xfrm->genid)
			return 0;

		if (strict && fl && dst->xfrm->props.mode != XFRM_MODE_TUNNEL &&
		    !xfrm_state_addr_flow_check(dst->xfrm, fl, family))
			return 0;

		mtu = dst_mtu(dst->child);
		if (xdst->child_mtu_cached != mtu) {
			last = xdst;
			xdst->child_mtu_cached = mtu;
		}

		if (!dst_check(xdst->route, xdst->route_cookie))
			return 0;
		mtu = dst_mtu(xdst->route);
		if (xdst->route_mtu_cached != mtu) {
			last = xdst;
			xdst->route_mtu_cached = mtu;
		}

		dst = dst->child;
	} while (dst->xfrm);

	if (likely(!last))
		return 1;

	mtu = last->child_mtu_cached;
	for (;;) {
		dst = &last->u.dst;

		mtu = xfrm_state_mtu(dst->xfrm, mtu);
		if (mtu > last->route_mtu_cached)
			mtu = last->route_mtu_cached;
		dst->metrics[RTAX_MTU-1] = mtu;

		if (last == first)
			break;

		last = last->u.next;
		last->child_mtu_cached = mtu;
	}

	return 1;
}

EXPORT_SYMBOL(xfrm_bundle_ok);

int xfrm_policy_register_afinfo(struct xfrm_policy_afinfo *afinfo)
{
	int err = 0;
	if (unlikely(afinfo == NULL))
		return -EINVAL;
	if (unlikely(afinfo->family >= NPROTO))
		return -EAFNOSUPPORT;
	write_lock_bh(&xfrm_policy_afinfo_lock);
	if (unlikely(xfrm_policy_afinfo[afinfo->family] != NULL))
		err = -ENOBUFS;
	else {
		struct dst_ops *dst_ops = afinfo->dst_ops;
		if (likely(dst_ops->kmem_cachep == NULL))
			dst_ops->kmem_cachep = xfrm_dst_cache;
		if (likely(dst_ops->check == NULL))
			dst_ops->check = xfrm_dst_check;
		if (likely(dst_ops->negative_advice == NULL))
			dst_ops->negative_advice = xfrm_negative_advice;
		if (likely(dst_ops->link_failure == NULL))
			dst_ops->link_failure = xfrm_link_failure;
		if (likely(afinfo->garbage_collect == NULL))
			afinfo->garbage_collect = __xfrm_garbage_collect;
		xfrm_policy_afinfo[afinfo->family] = afinfo;
	}
	write_unlock_bh(&xfrm_policy_afinfo_lock);
	return err;
}
EXPORT_SYMBOL(xfrm_policy_register_afinfo);

int xfrm_policy_unregister_afinfo(struct xfrm_policy_afinfo *afinfo)
{
	int err = 0;
	if (unlikely(afinfo == NULL))
		return -EINVAL;
	if (unlikely(afinfo->family >= NPROTO))
		return -EAFNOSUPPORT;
	write_lock_bh(&xfrm_policy_afinfo_lock);
	if (likely(xfrm_policy_afinfo[afinfo->family] != NULL)) {
		if (unlikely(xfrm_policy_afinfo[afinfo->family] != afinfo))
			err = -EINVAL;
		else {
			struct dst_ops *dst_ops = afinfo->dst_ops;
			xfrm_policy_afinfo[afinfo->family] = NULL;
			dst_ops->kmem_cachep = NULL;
			dst_ops->check = NULL;
			dst_ops->negative_advice = NULL;
			dst_ops->link_failure = NULL;
			afinfo->garbage_collect = NULL;
		}
	}
	write_unlock_bh(&xfrm_policy_afinfo_lock);
	return err;
}
EXPORT_SYMBOL(xfrm_policy_unregister_afinfo);

static struct xfrm_policy_afinfo *xfrm_policy_get_afinfo(unsigned short family)
{
	struct xfrm_policy_afinfo *afinfo;
	if (unlikely(family >= NPROTO))
		return NULL;
	read_lock(&xfrm_policy_afinfo_lock);
	afinfo = xfrm_policy_afinfo[family];
	if (unlikely(!afinfo))
		read_unlock(&xfrm_policy_afinfo_lock);
	return afinfo;
}

static void xfrm_policy_put_afinfo(struct xfrm_policy_afinfo *afinfo)
{
	read_unlock(&xfrm_policy_afinfo_lock);
}

static struct xfrm_policy_afinfo *xfrm_policy_lock_afinfo(unsigned int family)
{
	struct xfrm_policy_afinfo *afinfo;
	if (unlikely(family >= NPROTO))
		return NULL;
	write_lock_bh(&xfrm_policy_afinfo_lock);
	afinfo = xfrm_policy_afinfo[family];
	if (unlikely(!afinfo))
		write_unlock_bh(&xfrm_policy_afinfo_lock);
	return afinfo;
}

static void xfrm_policy_unlock_afinfo(struct xfrm_policy_afinfo *afinfo)
{
	write_unlock_bh(&xfrm_policy_afinfo_lock);
}

static int xfrm_dev_event(struct notifier_block *this, unsigned long event, void *ptr)
{
	switch (event) {
	case NETDEV_DOWN:
		xfrm_flush_bundles();
	}
	return NOTIFY_DONE;
}

static struct notifier_block xfrm_dev_notifier = {
	xfrm_dev_event,
	NULL,
	0
};

static void __init xfrm_policy_init(void)
{
	xfrm_dst_cache = kmem_cache_create("xfrm_dst_cache",
					   sizeof(struct xfrm_dst),
					   0, SLAB_HWCACHE_ALIGN,
					   NULL, NULL);
	if (!xfrm_dst_cache)
		panic("XFRM: failed to allocate xfrm_dst_cache\n");

	INIT_WORK(&xfrm_policy_gc_work, xfrm_policy_gc_task, NULL);
	register_netdevice_notifier(&xfrm_dev_notifier);
}

void __init xfrm_init(void)
{
	xfrm_state_init();
	xfrm_policy_init();
	xfrm_input_init();
}

