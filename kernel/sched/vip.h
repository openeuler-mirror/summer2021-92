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

struct vip_bandwidth {
#ifdef CONFIG_VIP_BANDWIDTH
	raw_spinlock_t		vip_runtime_lock;
	ktime_t				period;				// 设定的定时器周期时间，周期到了进行下一轮带宽控制
	u64					quota;				// 一个period周期内，一个组 可以使用的CPU限额(所有的用户组进程运行的时间累加在一起，保证总的运行时间小于quota)
											// 每个用户组会管理CPU个数的就绪队列group vip_rq。每个group vip_rq中也有限额时间，该限额时间是从全局用户组quota中申请
	u64					runtime;			// 记录剩余限额时间，在每次定时器回调函数中更新值为quota
	s64					hierarchical_quota;	// 层级管理任务组的限额比率

	u8					idle;				// 空闲状态，不需要运行时分配
	u8					period_active;		// 周期性计时已经启动
	u8					distribute_running;
	u8					slack_started;
	struct hrtimer		period_timer;		// 高精度定时器
	struct hrtimer		slack_timer;		// 延迟定时器，在任务出列时，将剩余的运行时间返回到全局池里
	struct list_head	throttled_vip_rq;	// 所有被throttle的vip_rq挂入此链表，在定时器的回调函数中遍历链表执行unthrottle vip_rq操作

	/* Statistics: */
	int					nr_periods;
	int					nr_throttled;
	u64					throttled_time;
#endif
}

struct vip_rq {
	struct load_weight load;
	// unsigned long		runnable_weight;
	unsigned int nr_running, h_nr_running;
	unsigned long nr_uninterruptible;

	u64 exec_clock;
	u64 min_vruntime;
#ifndef CONFIG_64BIT
	u64 min_vruntime_copy;
#endif

	struct rb_root_cached tasks_timeline;
	struct rb_node *rb_leftmost;		// ?? TODO -- Necessary?

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
	 * VIP load tracking
	 */
	struct sched_avg	avg;

#ifdef CONFIG_VIP_GROUP_SCHED
	/*
	 *   h_load = weight * f(tg)
	 *
	 * Where f(tg) is the recursive weight fraction assigned to
	 * this group.
	 */
#endif /* CONFIG_VIP_GROUP_SCHED */
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

#ifdef CONFIG_VIP_BANDWIDTH
	int			runtime_enabled;
	s64			runtime_remaining;

	u64			throttled_clock;
	u64			throttled_clock_task;
	u64			throttled_clock_task_time;
	int			throttled;
	int			throttle_count;
	struct list_head	throttled_list;
#endif /* CONFIG_VIP_BANDWIDTH */
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