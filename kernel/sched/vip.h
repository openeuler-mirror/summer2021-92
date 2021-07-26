/*
 *   File Name ：vip.h
 *   Author    ：Zhi Song
 *   Date      ：2021-7-17
 *   Descriptor：
 */

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VIP_H
#define _VIP_H

#include <linux/sched.h>
#include <uapi/linux/sched.h>

struct vip_rq {
	struct load_weight load;
	unsigned int nr_running, h_nr_running;
	unsigned long nr_uninterruptible;

	u64 exec_clock;
	u64 min_vruntime;
#ifndef CONFIG_64BIT
	u64 min_vruntime_copy;
#endif

	struct rb_root_cached tasks_timeline;
	struct rb_node *rb_leftmost;

	/*
	 * 'curr' points to currently running entity on this vip_rq.
	 * It is set to NULL otherwise (i.e when none are currently running).
	 */
	struct sched_entity *curr, *next, *last, *skip;

#ifdef	CONFIG_SCHED_DEBUG
	unsigned int nr_spread_over;
#endif

#ifdef CONFIG_SMP
/*
 * Load-tracking only depends on SMP, VIP_GROUP_SCHED dependency below may be
 * removed when useful for applications beyond shares distribution (e.g.
 * load-balance).
 */


	/*
	 *   h_load = weight * f(tg)
	 *
	 * Where f(tg) is the recursive weight fraction assigned to
	 * this group.
	 */
	unsigned long h_load;
#endif /* CONFIG_SMP */
};


static inline int vip_policy(int policy)
{
	if (policy == SCHED_VIP)
		return 1;
	return 0;
}

static inline int task_has_vip_policy(struct task_struct *p)
{
	return vip_policy(p->policy);
}

#endif  /* _VIP_H */