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

// TODO struct vip_bandwidth {

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

#ifdef CONFIG_SMP		// TODO
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
#ifdef CONFIG_VIP_GROUP_SCHED
	struct rq *rq;  /* cpu runqueue to which this vip_rq is attached */

	/*
	 * leaf vip_rqs are those that hold tasks (lowest schedulable entity in
	 * a hierarchy). Non-leaf lrqs hold other higher schedulable entities
	 * (like users, containers etc.)
	 *
	 * leaf_vip_rq_list ties together list of leaf vip_rq's in a cpu. This
	 * list is used during load balance.
	 */
	int on_list;
	struct list_head leaf_vip_rq_list;
	struct task_group *tg;  /* group that "owns" this runqueue */
#endif /* CONFIG_VIP_GROUP_SCHED */

	int vip_throttled;
	u64 vip_time;
	u64 vip_runtime;

	u64 throttled_clock, throttled_clock_task;
	u64 throttled_clock_task_time;
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