// SPDX-License-Identifier: GPL-2.0
/*
 *   File Name : vip.c
 *   Author    : Zhi Song
 *   Date      : 2021-7-19
 *   Descriptor:
 */

/*
 * Very-Important Scheduling Class (mapped to the SCHED_VIP policy)
 */
#include "sched.h"
#include "fair.h"	// vruntime related functions sharing
#include "vip.h"	// Contains vip_policy & task_has_vip_policy

void set_vip_load_weight(struct task_struct *p)
{
	int prio = p->static_prio - MIN_VIP_PRIO;
	struct load_weight *load = &p->vip.load;

	load->weight = scale_load(sched_prio_to_weight[prio]);
	load->inv_weight = sched_prio_to_wmult[prio];
}

const struct sched_class vip_sched_class;

static inline int vip_task(struct task_struct *p)
{
	return vip_prio(p->prio);
}

/**************************************************************
 * VIP operations on generic schedulable entities:
 */

#ifdef CONFIG_VIP_GROUP_SCHED
/* An entity is a task if it doesn't "own" a runqueue */
#define vip_entity_is_task(vip)	(!vip->my_q)

/* Walk up scheduling entities hierarchy */
#define for_each_sched_vip_entity(vip_se) \
		for (; vip_se; vip_se = vip_se->parent)

static inline struct sched_entity *parent_vip_entity(struct sched_entity *vip_se)
{
	return vip_se->parent;
}

/* runqueue on which this entity is (to be) queued */
static inline struct vip_rq *vip_rq_of(struct sched_entity *vip_se)
{
	return vip_se->vip_rq;
}

/* runqueue "owned" by this group */
static inline struct vip_rq *group_vip_rq(struct sched_entity *grp)
{
	return grp->vip_my_q;
}

static inline struct vip_rq *task_vip_rq(struct task_struct *p)
{
	return p->vip.vip_rq;
}

/* cpu runqueue to which this vip_rq is attached */
static inline struct rq *rq_of_vip_rq(struct vip_rq *vip_rq)
{
	return vip_rq->rq;
}

#else	/* !CONFIG_VIP_GROUP_SCHED */

#define vip_entity_is_task(vip)	1

#define for_each_sched_vip_entity(vip_se) \
		for (; vip_se; vip_se = NULL)

static inline struct sched_entity *parent_vip_entity(struct sched_entity *vip_se)
{
	return NULL;
}

static inline struct vip_rq *vip_rq_of(struct sched_entity *vip_se)
{
	struct task_struct *p = vip_task_of(vip_se);
	struct rq *rq = task_rq(p);

	return &rq->vip;
}

/* runqueue "owned" by this group */
static inline struct vip_rq *group_vip_rq(struct sched_entity *grp)
{
	return NULL;
}

static inline struct vip_rq *task_vip_rq(struct task_struct *p)
{
	return &task_rq(p)->vip;
}

static inline struct rq *rq_of_vip_rq(struct vip_rq* vip_rq)
{
	return container_of(vip_rq, struct rq, vip);
}

#endif	/* CONFIG_VIP_GROUP_SCHED */	/******************************************************/

static inline struct task_struct *vip_task_of(struct sched_entity *vip_se)
{
	return container_of(vip_se, struct task_struct, vip);
}

/**************************************************************
 * Scheduling class tree data structure manipulation methods:
 */

// 比较两个调度实体se的vruntime值大小，以确定搜索方向。
static inline int vip_entity_before(struct sched_entity *a,
				struct sched_entity *b)
{
	return (s64)(a->vruntime - b->vruntime) < 0;
}

/*
 * Enqueue an entity into the rb-tree:
 */
static void __enqueue_vip_entity(struct vip_rq *vip_rq, struct sched_entity *vip_se)
{
	// 操作红黑树
	struct rb_node **link = &(vip_rq->tasks_timeline.rb_root.rb_node);
	struct rb_node *parent = NULL;
	struct sched_entity *entry;
	int leftmost = 1;

	while (*link) {
		parent = *link;
		entry = rb_entry(parent, struct sched_entity, run_node);
		/*
		 * A standard rb-tree should not has same nodes. 
		 * But we dont care about collisions here. Nodes with
		 * the same key stay together.
		 */
		 if (vip_entity_before(vip_se, entry)) {
			 link = &parent->rb_left;
		 } else {
			 link = &parent->rb_right;
			 leftmost = 0;
		 }
	}

	rb_link_node(&vip_se->run_node, parent, link);
	rb_insert_color_cached(&vip_se->run_node,
					&vip_rq->tasks_timeline, leftmost);
}

static void __dequeue_vip_entity(struct vip_rq *vip_rq, struct sched_entity *vip_se)
{
	rb_erase_cached(&vip_se->run_node, &vip_rq->tasks_timeline);
}

struct sched_entity *__pick_first_vip_entity(struct vip_rq *vip_rq)
{
	struct rb_node *left = rb_first_cached(&vip_rq->tasks_timeline);

	if (!left)
		return NULL;

	return rb_entry(left, struct sched_entity, run_node);
}

static struct sched_entity *__pick_next_vip_entity(struct sched_entity *vip_se)
{
	struct rb_node *next = rb_next(&vip_se->run_node);

	if (!next)
		return NULL;

	return rb_entry(next, struct sched_entity, run_node);
}

static struct sched_entity *__pick_next_vip_entity(struct sched_entity *vip_se)
{
	struct rb_node *next = rb_next(&vip_se->run_node);

	if (!next)
		return NULL;

	return rb_entry(next, struct sched_entity, run_node);
}

/*
 * delta /= w
 */
// 根据任务权重计算调度实体实际运行时间对应的虚拟时间
static inline u64 calc_delta_vip(u64 delta, struct sched_entity *vip_se)
{
	if (unlikely(vip_se->load.weight != NICE_0_LOAD))
		delta = __calc_delta(delta, NICE_0_LOAD, &vip_se->load);	// 传入NICE_0_LOAD计算的是虚拟时间

	return delta;	// 任务load==虚拟时间计算的参照值NICE_0_LOAD则虚拟时间==实际运行时间
}

// 需要不断被更新
// min_vruntime太小，导致后面创建的新进程根据这个值来初始化新进程的虚拟时间，岂不是新创建的进程有可能再一次疯狂了。这一次可能就是cpu0创建，在cpu0上面疯狂
static void update_vip_min_vruntime(struct vip_rq *vip_rq)
{
	struct sched_entity *curr = vip_rq->curr;
	struct rb_node *leftmost = rb_first_cached(&vip_rq->tasks_timeline);

	u64 vruntime = vip_rq->min_vruntime;

	// vip队列中当前运行任务vruntime可能是minvruntime
	if (curr) {
		if (curr->on_rq)
			vruntime = vip_rq->curr->vruntime;
		else
			curr = NULL;
	}

	// vip队列中红黑树最左下节点可能是minvruntime
	if (leftmost) {
		struct sched_entity *vip_se = rb_entry(leftmost, struct sched_entity, run_node);

	可以想一下，如果一个进程刚睡眠1ms，然后醒来后你却要奖励3ms（虚拟时间减去3ms），然后他竟然赚了2ms。作为调度器，我们不做亏本生意。你睡眠100ms，奖励你3ms，那就是没问题的
		if (!curr)
			vruntime = vip_se->vruntime;
		else
			vruntime = min_vruntime(vruntime, vip_se->vruntime);
	}

	// 不能让min_runtime越来越小，以至于越来越多任务无法使用CPU资源
	/* ensure we never gain time by being placed backwards. */
	vip_rq->min_vruntime = max_vruntime(vip_rq->min_vruntime, vruntime);

#ifndef CONFIG_64BIT
	/* memory barrior for writting */
	smp_wmb();
	vip_rq->min_vruntime_copy = vip_rq->min_vruntime;
#endif
}
/*
 * We calculate the wall-time slice from the period by taking a part
 * proportional to the weight.
 *
 * s = p*P[w/rw]
 */
// 计算在这个vip队列中一个周期内(调度时延内)分配给这个权重的进程的真实运行时间
static u64 sched_vip_slice(struct vip_rq *vip_rq, struct sched_entity *vip_se)
{
	u64 slice = __sched_period(vip_rq->nr_running + !vip_se->on_rq);	// 根据当前就绪进程个数计算调度周期，默认情况下，进程不超过8个情况下，调度周期默认6ms

	// for循环根据vip_se->parent链表往上计算⽐例
	for_each_sched_vip_entity(vip_se) {
		struct load_weight *load;
		struct load_weight lw;

		vip_rq = vip_rq_of(vip_se);
		load = &vip_rq->load;	// 获得vip_se依附的vip_rq的负载信息

		if (unlikely(!vip_se->on_rq)) {
			lw = vip_rq->load;

			update_load_add(&lw, vip_se->load.weight);
			load = &lw;
		}
		slice = __calc_delta(slice, vip_se->load.weight, load);		// 计算slice = slice * vip_se->load.weight / vip_rq->load.weight的值
	}
	return slice;
}

/*
 * We calculate the vruntime slice of a to-be-inserted task.
 *
 * vs = s/w
 */
// 计算任务虚拟时间 - 这个任务在一个调度延时内运行时间对应的虚拟运行时间
static u64 sched_vip_vslice(struct vip_rq *vip_rq, struct sched_entity *vip_se)
{
	// 填入参数1：任务一个调度延时内所用实际时间
	// 填入参数2：任务的调度实体
	return calc_delta_vip(sched_vip_slice(vip_rq, vip_se), vip_se);
}


/*
 * Update the current task's runtime statistics.
 */
// 更新当前运行任务的 vruntime & viprq的min_vruntime
// TODO: newest version
static void update_curr_vip(struct vip_rq *vip_rq)
{
	struct sched_entity *curr = vip_rq->curr;
	u64 now = rq_of_vip_rq(vip_rq)->clock_task;
	u64 delta_exec;

	if (unlikely(!curr))
		return;

	delta_exec = now - curr->exec_start;	// 本次更新虚拟时间距离上次更新虚拟时间的差值
	if (unlikely((s64)delta_exec <= 0))
		return;

	schedstat_set(curr->vip_statistics->exec_max,
			  max((u64)delta_exec, curr->vip_statistics->exec_max));

	curr->sum_exec_runtime += delta_exec;
	schedstat_add(vip_rq->exec_clock, delta_exec);

	curr->vruntime += calc_delta_vip(delta_exec, curr);		// 填入特定参数调用__calc_delta来更新当前调度实体虚拟时间
	update_vip_min_vruntime(vip_rq);		// TODO

	curr->exec_start = now;

	if (vip_entity_is_task(curr)) {		// 防止entity是task_group有rq
		struct task_struct *curtask = vip_task_of(curr);
		
		// TODO group schedule

		trace_sched_stat_runtime(curtask, delta_exec, curr->vruntime);
		cpuacct_charge(curtask, delta_exec);
	}
}

// flag initial: 1 --> a new task from fork operation  0 --> not a newborn task
// 补偿睡眠进程 罚时新创建进程
static void
place_vip_entity(struct vip_rq *vip_rq, struct sched_entity *vip_se, int initial)
{
	u64 vruntime = vip_rq->min_vruntime;

	/*
	 * The 'current' period is already promised to the current tasks,
	 * however the extra weight of the new task will slow them down a
	 * little, place the new task so that it fits in the slot that
	 * stays open at the end.
	 */
	// 这里是处理创建的进程，针对刚创建的进程会进行一定的惩罚，将虚拟时间加上一个值就是惩罚，毕竟虚拟时间越小越容易被调度执行。惩罚的时间由sched_vip_vslice()计算。
	if (initial && sched_feat(START_DEBIT))
		vruntime += sched_vip_vslice(vip_rq, vip_se);		// 加上这个进程在一个调度延时内运行时间的虚拟运行时间

	// 被唤醒的旧进程人家都睡了那么久了，这里对他的vruntime减半latency作补偿
	/* sleeps up to a single latency don't count. */
	if (!initial) {
		unsigned long thresh = sysctl_sched_latency;

		/*
		 * Halve their sleep time's effect, to allow
		 * for a gentler effect of sleepers:
		 */
		if (sched_feat(GENTLE_FAIR_SLEEPERS))
			thresh >>= 1;

		vruntime -= thresh;
	}

	/* ensure we never gain time by being placed backwards. */		// 保证调度实体的虚拟时间不能倒退
	vip_se->vruntime = max_vruntime(vip_se->vruntime, vruntime);	
	// 可以想一下，如果一个进程刚睡眠1ms，然后醒来后你却要奖励3ms（虚拟时间减去3ms），然后他竟然赚了2ms。作为调度器，我们不做亏本生意。你睡眠100ms，奖励你3ms，那就是没问题的
}

static inline void
update_stats_wait_start_vip(struct vip_rq *vip_rq, struct sched_entity *vip_se)
{
	schedstat_set(vip_se->vip_statistics->wait_start,
			rq_of_vip_rq(vip_rq)->clock);
}

/*
 * Task is being enqueued - update stats:
 */
static void
update_stats_enqueue_vip(struct vip_rq *vip_rq, struct sched_entity *vip_se)
{
	/*
	 * Are we enqueueing a waiting task? (for current tasks
	 * a dequeue/enqueue event is a NOP)
	 */
	if (vip_se != vip_rq->curr)
		update_stats_wait_start_vip(vip_rq, vip_se);
}

update_stats_wait_end_vip(struct vip_rq *vip_rq, struct sched_entity *vip_se)
{
	schedstat_set(cip_se->vip_statistics->wait_max,
			max(vip_se->vip_statistics->wait_max,
			rq_of_vip_rq(vip_rq)->clock - vip_se->vip_statistics->wait_start));
	schedstat_set(vip_se->vip_statistics->wait_count,
			vip_se->vip_statistics->wait_count + 1);
	schedstat_set(vip_se->vip_statistics->wait_sum,
			vip_se->vip_statistics->wait_sum +
			rq_of_vip_rq(vip_rq)->clock - vip_se->vip_statistics->wait_start);
#ifdef CONFIG_SCHEDSTATS
	if (vip_entity_is_task(vip_se)) {
		trace_sched_stat_wait(vip_task_of(vip_se),
			rq_of_vip_rq(vip_rq)->clock - vip_se->vip_statistics->wait_start);
	}
#endif
	schedstat_set(vip_se->vip_statistics->wait_start, 0);
}

static inline void
update_stats_dequeue_vip(struct vip_rq *vip_rq, struct sched_entity *vip_se)
{
	/*
	 * Mark the end of the wait period if dequeueing a
	 * waiting task:
	 */
	if (se != vip_rq->curr)
		update_stats_wait_end_vip(vip_rq, vip_se);
}

/*
 * We are picking a new current task - update its stats:
 */
static inline void
update_stats_curr_start_vip(struct vip_rq *vip_rq, struct sched_entity *se)
{
	/*
	 * We are starting a new run period:
	 */
	se->exec_start = rq_clock_task(rq_of_vip_rq(vip_rq));
}

static void
account_vip_entity_enqueue(struct vip_rq *vip_rq, struct sched_entity *vip_se)
{
	update_load_add(&vip_rq->load, vip_se->load.weight);

	vip_rq->nr_running++;
}

static void
account_vip_entity_dequeue(struct vip_rq *vip_rq, struct sched_entity *vip_se)
{
	update_load_sub(&vip_rq->load, vip_se->load.weight);

	vip_rq->nr_running--;
}

// static void reweight_vip_entity(struct cfs_rq *cfs_rq, struct sched_entity *se,
// 			    unsigned long weight, unsigned long runnable)
// {
// 	if (se->on_rq) {
// 		/* commit outstanding execution time */
// 		if (cfs_rq->curr == se)
// 			update_curr(cfs_rq);
// 		account_entity_dequeue(cfs_rq, se);
// 		dequeue_runnable_load_avg(cfs_rq, se);
// 	}
// 	dequeue_load_avg(cfs_rq, se);

// 	se->runnable_weight = runnable;
// 	update_load_set(&se->load, weight);

// #ifdef CONFIG_SMP
// 	do {
// 		u32 divider = LOAD_AVG_MAX - 1024 + se->avg.period_contrib;

// 		se->avg.load_avg = div_u64(se_weight(se) * se->avg.load_sum, divider);
// 		se->avg.runnable_load_avg =
// 			div_u64(se_runnable(se) * se->avg.runnable_load_sum, divider);
// 	} while (0);
// #endif

// 	enqueue_load_avg(cfs_rq, se);
// 	if (se->on_rq) {
// 		account_entity_enqueue(cfs_rq, se);
// 		enqueue_runnable_load_avg(cfs_rq, se);
// 	}
// }

// #ifdef CONFIG_VIP_GROUP_SCHED
// #ifdef CONFIG_SMP
// /*
//  * All this does is approximate the hierarchical proportion which includes that
//  * global sum we all love to hate.
//  *
//  * That is, the weight of a group entity, is the proportional share of the
//  * group weight based on the group runqueue weights. That is:
//  *
//  *                     tg->weight * grq->load.weight
//  *   ge->load.weight = -----------------------------               (1)
//  *			  \Sum grq->load.weight
//  *
//  * Now, because computing that sum is prohibitively expensive to compute (been
//  * there, done that) we approximate it with this average stuff. The average
//  * moves slower and therefore the approximation is cheaper and more stable.
//  *
//  * So instead of the above, we substitute:
//  *
//  *   grq->load.weight -> grq->avg.load_avg                         (2)
//  *
//  * which yields the following:
//  *
//  *                     tg->weight * grq->avg.load_avg
//  *   ge->load.weight = ------------------------------              (3)
//  *				tg->load_avg
//  *
//  * Where: tg->load_avg ~= \Sum grq->avg.load_avg
//  *
//  * That is shares_avg, and it is right (given the approximation (2)).
//  *
//  * The problem with it is that because the average is slow -- it was designed
//  * to be exactly that of course -- this leads to transients in boundary
//  * conditions. In specific, the case where the group was idle and we start the
//  * one task. It takes time for our CPU's grq->avg.load_avg to build up,
//  * yielding bad latency etc..
//  *
//  * Now, in that special case (1) reduces to:
//  *
//  *                     tg->weight * grq->load.weight
//  *   ge->load.weight = ----------------------------- = tg->weight   (4)
//  *			    grp->load.weight
//  *
//  * That is, the sum collapses because all other CPUs are idle; the UP scenario.
//  *
//  * So what we do is modify our approximation (3) to approach (4) in the (near)
//  * UP case, like:
//  *
//  *   ge->load.weight =
//  *
//  *              tg->weight * grq->load.weight
//  *     ---------------------------------------------------         (5)
//  *     tg->load_avg - grq->avg.load_avg + grq->load.weight
//  *
//  * But because grq->load.weight can drop to 0, resulting in a divide by zero,
//  * we need to use grq->avg.load_avg as its lower bound, which then gives:
//  *
//  *
//  *                     tg->weight * grq->load.weight
//  *   ge->load.weight = -----------------------------		   (6)
//  *				tg_load_avg'
//  *
//  * Where:
//  *
//  *   tg_load_avg' = tg->load_avg - grq->avg.load_avg +
//  *                  max(grq->load.weight, grq->avg.load_avg)
//  *
//  * And that is shares_weight and is icky. In the (near) UP case it approaches
//  * (4) while in the normal case it approaches (3). It consistently
//  * overestimates the ge->load.weight and therefore:
//  *
//  *   \Sum ge->load.weight >= tg->weight
//  *
//  * hence icky!
//  */
// static long calc_vip_group_shares(struct vip_rq *vip_rq)
// {
// 	long tg_weight, tg_shares, load, shares;
// 	struct task_group *tg = vip_rq->tg;

// 	tg_shares = READ_ONCE(tg->vip_shares);

// 	load = max(scale_load_down(vip_rq->load.weight), vip_rq->avg.load_avg);

// 	tg_weight = atomic_long_read(&tg->load_avg);

// 	/* Ensure tg_weight >= load */
// 	tg_weight -= vip_rq->tg_load_avg_contrib;
// 	tg_weight += load;

// 	shares = (tg_shares * load);
// 	if (tg_weight)
// 		shares /= tg_weight;

// 	/*
// 	 * MIN_SHARES has to be unscaled here to support per-CPU partitioning
// 	 * of a group with small tg->shares value. It is a floor value which is
// 	 * assigned as a minimum load.weight to the sched_entity representing
// 	 * the group on a CPU.
// 	 *
// 	 * E.g. on 64-bit for a group with tg->shares of scale_load(15)=15*1024
// 	 * on an 8-core system with 8 tasks each runnable on one CPU shares has
// 	 * to be 15*1024*1/8=1920 instead of scale_load(MIN_SHARES)=2*1024. In
// 	 * case no task is runnable on a CPU MIN_SHARES=2 should be returned
// 	 * instead of 0.
// 	 */
// 	return clamp_t(long, shares, MIN_SHARES, tg_shares);
// }

// /*
//  * This calculates the effective runnable weight for a group entity based on
//  * the group entity weight calculated above.
//  *
//  * Because of the above approximation (2), our group entity weight is
//  * an load_avg based ratio (3). This means that it includes blocked load and
//  * does not represent the runnable weight.
//  *
//  * Approximate the group entity's runnable weight per ratio from the group
//  * runqueue:
//  *
//  *					     grq->avg.runnable_load_avg
//  *   ge->runnable_weight = ge->load.weight * -------------------------- (7)
//  *						 grq->avg.load_avg
//  *
//  * However, analogous to above, since the avg numbers are slow, this leads to
//  * transients in the from-idle case. Instead we use:
//  *
//  *   ge->runnable_weight = ge->load.weight *
//  *
//  *		max(grq->avg.runnable_load_avg, grq->runnable_weight)
//  *		-----------------------------------------------------	(8)
//  *		      max(grq->avg.load_avg, grq->load.weight)
//  *
//  * Where these max() serve both to use the 'instant' values to fix the slow
//  * from-idle and avoid the /0 on to-idle, similar to (6).
//  */
// static long calc_vip_group_runnable(struct vip_rq *vip_rq, long shares)
// {
// 	long runnable, load_avg;

// 	load_avg = max(vip_rq->avg.load_avg,
// 		       scale_load_down(vip_rq->load.weight));

// 	runnable = max(vip_rq->avg.runnable_load_avg,
// 		       scale_load_down(vip_rq->runnable_weight));

// 	runnable *= shares;
// 	if (load_avg)
// 		runnable /= load_avg;

// 	return clamp_t(long, runnable, MIN_SHARES, shares);
// }
// #endif /* CONFIG_SMP */

// static inline int throttled_hierarchy(struct vip_rq *vip_rq);

// /*
//  * Recomputes the group entity based on the current state of its group
//  * runqueue.
//  */
// // 更新VIP group sched_entity的权重信息
// static void update_vip_group(struct sched_entity *vip_se)
// {
// 	struct vip_rq *gvip_rq = group_vip_rq(vip_se);
// 	long shares, runnable;

// 	if (!gvip_rq)
// 		return;

// 	if (vip_throttled_hierarchy(gvip_rq))	// ?
// 		return;

// #ifndef CONFIG_SMP
// 	runnable = shares = READ_ONCE(gvip_rq->tg->vip_shares);

// 	if (likely(vip_se->load.weight == shares))
// 		return;
// #else
// 	shares   = calc_vip_group_shares(gvip_rq);
// 	runnable = calc_vip_group_runnable(gvip_rq, shares);
// #endif

// 	reweight_vip_entity(vip_rq_of(se), se, shares, runnable);
// }

// #else /* CONFIG_VIP_GROUP_SCHED */
// static inline void update_vip_group(struct sched_entity *vip_se)
// {
// }
// #endif /* CONFIG_VIP_GROUP_SCHED */

static void check_vip_spread(struct vip_rq *vip_rq, struct sched_entity *vip_se)
{
#ifdef CONFIG_SCHED_DEBUG
	s64 d = vip_se->vruntime - vip_rq->min_vruntime;

	if (d < 0)
		d = -d;

	if (d > 3*sysctl_sched_latency)
		schedstat_inc(vip_rq->nr_spread_over);
#endif
}


// 向可运行队列中插入一个新的节点，意味着有一个新的进程状态转换为可运行，这会发生在两种情况下：一是当进程由阻塞态被唤醒，二是fork产生新的进程时。
// 将其加入队列的过程本质上来说就是红黑树插入新节点的过程：
static void
enqueue_vip_entity(struct vip_rq *vip_rq, struct sched_entity *vip_se, int flags)
{
	bool renorm = !(flags & ENQUEUE_WAKEUP) || (flags & ENQUEUE_MIGRATED);
	bool curr = vip_rq->curr == vip_se;

	/*
	 * If we're the current task, we must renormalise before calling
	 * update_curr(). -- FROM linux kernel commit 2f95035
	 */
	if (renorm && curr)
		vip_se->vruntime += vip_rq->min_vruntime;
	
	/*
	 * Update run-time vip_statistics of the 'current'.
	 */
	update_curr_vip(vip_rq);
	
	// vip任务不会fork子任务，之前没有-min_vruntime因此无以下
	// if (renorm && !curr)
	//	vip_se->vruntime += vip_rq->min_vruntime;
	
	account_vip_entity_enqueue(vip_rq, vip_se);		// add load

	if (flags & ENQUEUE_WAKEUP) {
		place_vip_entity(vip_rq, vip_se, 0);
	}

	update_stats_enqueue_vip(vip_rq, vip_se);
	check_vip_spread(vip_rq, vip_se);
	if (vip_se != vip_rq->curr)
		__enqueue_vip_entity(vip_rq, vip_se);
	vip_se->on_rq = 1;
}
// enqueue_vip_sleeper

static void __clear_buddies_last_vip(struct sched_entity *vip_se)
{
	for_each_sched_vip_entity(vip_se) {
		struct vip_rq *vip_rq = vip_rq_of(vip_se);

		if (vip_rq->last == vip_se)
			vip_rq->last = NULL;
		else
			break;
	}
}

static void __clear_buddies_next_vip(struct sched_entity *vip_se)
{
	for_each_sched_vip_entity(vip_se) {
		struct vip_rq *vip_rq = vip_rq_of(vip_se);

		if (vip_rq->next == vip_se)
			vip_rq->next = NULL;
		else
			break;
	}
}

static void __clear_buddies_skip_vip(struct sched_entity *vip_se)
{
	for_each_sched_vip_entity(vip_se) {
		struct vip_rq *vip_rq = vip_rq_of(vip_se);

		if (vip_rq->skip == vip_se)
			vip_rq->skip = NULL;
		else
			break;
	}
}

static void clear_buddies_vip(struct vip_rq *vip_rq, struct sched_entity *vip_se)
{
	if (vip_rq->last == vip_se)
		__clear_buddies_last_vip(vip_se);

	if (vip_rq->next == vip_se)
		__clear_buddies_next_vip(vip_se);

	if (vip_rq->skip == vip_se)
		__clear_buddies_skip_vip(vip_se);
}

static void
dequeue_vip_entity(struct vip_rq *vip_rq, struct sched_entity *vip_se, int flags)
{
	/*
	 * Update run-time vip_statistics of the 'current'.
	 */
	update_curr_vip(vip_rq);

	update_stats_dequeue_vip(vip_rq, vip_se);
	if (flags & DEQUEUE_SLEEP) {
#if defined(CONFIG_SCHEDSTATS) || defined(CONFIG_LATENCYTOP)
		if (vip_entity_is_task(vip_se)) {
			struct task_struct *tsk = vip_task_of(vip_se);

			if (tsk->state & TASK_INTERRUPTIBLE)
				vip_se->vip_statistics->sleep_start = rq_of_vip_rq(vip_rq)->clock;
			if (tsk->state & TASK_UNINTERRUPTIBLE)
				vip_se->vip_statistics->block_start = rq_of_vip_rq(vip_rq)->clock;
		}
#endif
	}

	clear_buddies_vip(vip_rq, vip_se);

	if (vip_se != vip_rq->curr)
		__dequeue_vip_entity(vip_rq, vip_se);
	vip_se->on_rq = 0;
	account_vip_entity_dequeue(vip_rq, vip_se);

	/*
	 * Normalize the entity after updating the min_vruntime because the
	 * update can refer to the ->curr item and we need to reflect this
	 * movement in our normalized position.
	 */
	if (!(flags & DEQUEUE_SLEEP))
		vip_se->vruntime -= vip_rq->min_vruntime;

	update_vip_min_vruntime(vip_rq);
}

// check_preempt_tick的作用是根据当前进程已经运行的时间，判断是否需要将进程thread info结构体的flag
// 通过resched_curr设置为TIF_NEED_RESCHED。这样在执行完本次时钟中断后（即从handle irq返回后），根据需要进行重新调度。
// TODO
static void
check_preempt_tick_vip(struct vip_rq *vip_rq, struct sched_entity *curr)
{
	unsigned long ideal_runtime, delta_exec;
	struct sched_entity *vip_se;
	s64 delta;

	ideal_runtime = sched_vip_slice(vip_rq, curr);
	delta_exec = curr->sum_exec_runtime - curr->prev_sum_exec_runtime;
	if (delta_exec > ideal_runtime) {
		resched_curr(rq_of_vip_rq(vip_rq));
		/*
		 * The current task ran long enough, ensure it doesn't get
		 * re-elected due to buddy favours.
		 */
		clear_buddies_vip(vip_rq, curr);
		return;
	}

	/*
	 * Ensure that a task that missed wakeup preemption by a
	 * narrow margin doesn't have to wait for a full slice.
	 * This also mitigates buddy induced latencies under load.
	 */
	if (delta_exec < sysctl_sched_min_granularity)
		return;

	vip_se = __pick_first_vip_entity(vip_rq);
	delta = curr->vruntime - vip_se->vruntime;

	if (delta < 0)
		return;

	if (delta > ideal_runtime)
		resched_curr(rq_of_vip_rq(vip_rq));
}

static void
set_next_vip_entity(struct vip_rq *vip_rq, struct sched_entity *vip_se)
{
	/* 'current' is not kept within the tree. */
	if (vip_se->on_rq) {
		/*
		 * Any task has to be enqueued before it get to execute on
		 * a CPU. So account for the time it spent waiting on the
		 * runqueue.
		 */
		update_stats_wait_end_vip(vip_rq, vip_se);
		__dequeue_vip_entity(vip_rq, vip_se);
	}

	update_stats_curr_start_vip(vip_rq, vip_se);
	vip_rq->curr = vip_se;			// 昭告天下
#ifdef CONFIG_SCHEDSTATS
	/*
	 * Track our maximum slice length, if the CPU's load is at
	 * least twice that of our own weight (i.e. dont track it
	 * when there are only lesser-weight tasks around):
	 */
	if (vip_rq->load.weight >= 2*vip_se->load.weight) {
		vip_se->vip_statistics->slice_max = max(vip_se->vip_statistics->slice_max,
			vip_se->sum_exec_runtime - vip_se->prev_sum_exec_runtime);
	}
#endif
	vip_se->prev_sum_exec_runtime = vip_se->sum_exec_runtime;
}

static int
wakeup_preempt_vip_entity(struct sched_entity *curr, struct sched_entity *vip_se);

/*
 * Pick the next process, keeping these things in mind, in this order:
 * 1) keep things fair between processes/task groups
 * 2) pick the "next" process, since someone really wants that to run
 * 3) pick the "last" process, for cache locality
 * 4) do not run the "skip" process, if something else is available
 */
static struct sched_entity *pick_next_vip_entity(struct vip_rq *vip_rq)
{
	struct sched_entity *vip_se = __pick_first_vip_entity(vip_rq);
	struct sched_entity *left = vip_se;

	/*
	 * Avoid running the skip buddy, if running something else can
	 * be done without getting too unfair.
	 */
	if (vip_rq->skip == vip_se) {
		struct sched_entity *vip_second = __pick_next_vip_entity(vip_se);

		if (second && wakeup_preempt_vip_entity(second, left) < 1)
			vip_se = second;
	}

	/*
	 * Prefer last buddy, try to return the CPU to a preempted task.
	 */
	if (vip_rq->last && wakeup_preempt_vip_entity(vip_rq->last, left) < 1)
		vip_se = vip_rq->last;

	/*
	 * Someone really wants this to run. If it's not unfair, run it.
	 */
	if (vip_rq->next && wakeup_preempt_vip_entity(vip_rq->next, left) < 1)
		vip_se = vip_rq->next;

	clear_buddies_vip(vip_rq, vip_se);

	return vip_se;
}

static void put_prev_vip_entity(struct vip_rq *vip_rq, struct sched_entity *prev)
{
	/*
	 * If still on the runqueue then deactivate_task()
	 * was not called and update_curr() has to be done:
	 */
	if (prev->on_rq)
		update_curr_vip(vip_rq);

	check_vip_spread(vip_rq, prev);
	if (prev->on_rq) {
		update_stats_wait_start_vip(vip_rq, prev);
		/* Put 'current' back into the tree. */
		__enqueue_vip_entity(vip_rq, prev);
	}
	vip_rq->curr = NULL;
}


static void
vip_entity_tick(struct vip_rq *vip_rq, struct sched_entity *curr, int queued)
{
	/*
	 * Update run-time vip_statistics of the 'current'.
	 */
	update_curr_vip(vip_rq);

#ifdef CONFIG_SCHED_HRTICK
	/*
	 * queued ticks are scheduled to match the slice, so don't bother
	 * validating it and just reschedule.
	 */
	if (queued) {
		resched_curr(rq_of_vip_rq(vip_rq));
		return;
	}
#endif

	if (vip_rq->nr_running > 1)
		check_preempt_tick_vip(vip_rq, curr);
}

/**************************************************
 * VIP bandwidth control machinery
 */

#ifdef CONFIG_VIP_BANDWIDTH
/*
 * default period for vip group bandwidth.
 * default: 0.1s, units: nanoseconds
 */
static inline u64 default_vip_period(void)
{
	return 100000000ULL;
}

// /* check whether vip_rq, or any parent, is throttled */
// static inline int vip_throttled_hierarchy(struct vip_rq *vip_rq)
// {
// 	return vip_bandwidth_used() && vip_rq->throttle_count;
// }

static void init_vip_rq_runtime(struct vip_rq *vip_rq)
{
	vip_rq->runtime_enabled = 0;
	INIT_LIST_HEAD(&vip_rq->throttled_list);
}

void init_vip_bandwidth(struct vip_bandwidth *vip_b)
{
	raw_spin_lock_init(&vip_b->lock);
	vip_b->runtime = 0;
	vip_b->quota = RUNTIME_INF;
	vip_b->period = ns_to_ktime(default_vip_period());

	INIT_LIST_HEAD(&vip_b->throttled_vip_rq);
	hrtimer_init(&vip_b->period_timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS_PINNED);
	vip_b->period_timer.function = sched_vip_period_timer;
	hrtimer_init(&vip_b->slack_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	vip_b->slack_timer.function = sched_vip_slack_timer;
	vip_b->distribute_running = 0;
	vip_b->slack_started = false;
}

#else	/* CONFIG_VIP_BANDWIDTH */

#ifdef CONFIG_VIP_GROUP_SCHED
static void init_vip_rq_runtime(struct vip_rq *vip_rq) {}
#endif

void init_vip_bandwidth(struct vip_bandwidth *vip_b) {}

#endif	/* CONFIG_VIP_BANDWIDTH */

/*
 * The enqueue_task method is called before nr_running is
 * increased. Here we update the vip scheduling stats and
 * then put the task into the rbtree:
 */
// vip任务会fork新进程
static void
enqueue_task_vip(struct rq *rq, struct task_struct *p, int flags)
{
	struct sched_entity* vip_se = &(p->vip);
	struct vip_rq* vip_rq;

	// Key: enqueue_vip_entity
	for_each_sched_vip_entity(vip_se) {
		// 如果已在队列
		if(vip_se->on_rq)
			break;
		// 未在则入队列
		vip_rq = vip_rq_of(vip_se);
		enqueue_vip_entity(vip_rq, vip_se, flags);		// enqueue_vip_entity调用account_vip_entity_enqueue使vip_rq->nr_running++

		vip_rq->h_nr_running++;		// 组调度相关

		flags = ENQUEUE_WAKEUP;
	}

	if (!vip_se) {
		rq->vip_nr_running++;
		add_nr_running(rq, 1);		// add 1 to nr_running of rq
	}
}

/*
 * The dequeue_task_vip method is called before nr_running is
 * decreased. We remove the task from the rbtree and
 * update the vip scheduling stats:
 */
// TODO
static void dequeue_task_vip(struct rq *rq, struct task_struct *p, int flags)
{
	struct vip_rq *vip_rq;
	struct sched_entity *se = &p->vip;
	int task_sleep = flags & DEQUEUE_SLEEP;

	for_each_sched_vip_entity(se) {
		vip_rq = vip_rq_of(se);
		dequeue_vip_entity(vip_rq, se, flags);

		vip_rq->h_nr_running--;

		/* Don't dequeue parent if it has other entities besides us */
		if (vip_rq->load.weight) {
			/*
			 * Bias pick_next to pick a task from this vip_rq, as
			 * p is sleeping when it is within its sched_slice.
			 */
			if (task_sleep && parent_vip_entity(se))
				set_next_buddy_vip(parent_vip_entity(se));

			/* avoid re-evaluating load for this entity */
			se = parent_vip_entity(se);
			break;
		}
		flags |= DEQUEUE_SLEEP;
	}

	if (!se) {
		sub_nr_running(rq, 1);
		rq->vip_nr_running--;
	}
}

#ifdef CONFIG_SMP
static DEFINE_PER_CPU(cpumask_var_t, local_cpu_mask_vip);

static int find_lowest_rq_vip(struct task_struct *task)
{
	struct sched_domain *sd;
	struct cpumask *lowest_mask = this_cpu_cpumask_var_ptr(local_cpu_mask_vip);
	int this_cpu = smp_processor_id();
	int cpu      = task_cpu(task);

	/* Make sure the mask is initialized first */
	if (unlikely(!lowest_mask))
		return -1;

	if (task->nr_cpus_allowed == 1)
		return -1; /* No other targets possible */

	if (!cpupri_find(&task_rq(task)->rd->cpupri, task, lowest_mask))
		return -1; /* No targets found */

	/*
	 * At this point we have built a mask of CPUs representing the
	 * lowest priority tasks in the system.  Now we want to elect
	 * the best one based on our affinity and topology.
	 *
	 * We prioritize the last CPU that the task executed on since
	 * it is most likely cache-hot in that location.
	 */
	if (cpumask_test_cpu(cpu, lowest_mask))
		return cpu;

	/*
	 * Otherwise, we consult the sched_domains span maps to figure
	 * out which CPU is logically closest to our hot cache data.
	 */
	if (!cpumask_test_cpu(this_cpu, lowest_mask))
		this_cpu = -1; /* Skip this_cpu opt if not among lowest */

	rcu_read_lock();
	for_each_domain(cpu, sd) {
		if (sd->flags & SD_WAKE_AFFINE) {
			int best_cpu;

			/*
			 * "this_cpu" is cheaper to preempt than a
			 * remote processor.
			 */
			if (this_cpu != -1 &&
			    cpumask_test_cpu(this_cpu, sched_domain_span(sd))) {
				rcu_read_unlock();
				return this_cpu;
			}

			best_cpu = cpumask_first_and(lowest_mask,
						     sched_domain_span(sd));
			if (best_cpu < nr_cpu_ids) {
				rcu_read_unlock();
				return best_cpu;
			}
		}
	}
	rcu_read_unlock();

	/*
	 * And finally, if there were no matches within the domains
	 * just give the caller *something* to work with from the compatible
	 * locations.
	 */
	if (this_cpu != -1)
		return this_cpu;

	cpu = cpumask_any(lowest_mask);
	if (cpu < nr_cpu_ids)
		return cpu;

	return -1;
}

// 返回的CPU是给待唤醒的任务所设置的
static int
select_task_rq_vip(struct task_struct *p, int cpu, int sd_flag, int flags)
{
	struct task_struct *curr;
	struct rq *rq;

	/* For anything but wake ups, just return the task_cpu */
	if (sd_flag != SD_BALANCE_WAKE && sd_flag != SD_BALANCE_FORK)
		goto out;

	rq = cpu_rq(cpu);

	rcu_read_lock();
	curr = READ_ONCE(rq->curr); /* unlocked access */

	/*
	 * If the current task on @p's runqueue is an VIP task, then
	 * try to see if we can wake this VIP task up on another
	 * runqueue. Otherwise simply start this VIP task
	 * on its current runqueue.
	 *
	 * We want to avoid overloading runqueues. If the woken
	 * task is a higher priority, then it will stay on this CPU
	 * and the lower prio task should be moved to another CPU.
	 * Even though this will probably make the lower prio task
	 * lose its cache, we do not want to bounce a higher task
	 * around just because it gave up its CPU, perhaps for a
	 * lock?
	 *
	 * For equal prio tasks, we just let the scheduler sort it out.
	 *
	 * Otherwise, just let it ride on the affined RQ and the
	 * post-schedule router will push the preempted task away
	 *
	 * This test is optimistic, if we get it wrong the load-balancer
	 * will have to sort it out.
	 */
	// 如果满足这些条件(即curr不能迁移)，则找一个空闲的CPU给新唤醒的任务(p)
	if (curr && unlikely(vip_task(curr)) &&
	    (curr->nr_cpus_allowed < 2 ||
	     curr->prio <= p->prio)) {
		int target = find_lowest_rq_vip(p);

		/*
		 * Don't bother moving it if the destination CPU is
		 * not running a lower priority task.
		 */
		if (target != -1 &&
		    p->prio < cpu_rq(target)->vip.highest_prio.curr)
			cpu = target;
	}
	rcu_read_unlock();

out:
	return cpu;
}
#endif	/* CONFIG_SMP */

static unsigned long
wakeup_gran_vip(struct sched_entity *curr, struct sched_entity *vip_se)
{
	unsigned long gran = sysctl_sched_wakeup_granularity;

	/*
	 * Since its curr running now, convert the gran from real-time
	 * to virtual-time in his units.
	 *
	 * By using 'se' instead of 'curr' we penalize light tasks, so
	 * they get preempted easier. That is, if 'se' < 'curr' then
	 * the resulting gran will be larger, therefore penalizing the
	 * lighter, if otoh 'se' > 'curr' then the resulting gran will
	 * be smaller, again penalizing the lighter task.
	 *
	 * This is especially important for buddies when the leftmost
	 * task is higher priority than the buddy.
	 */
	return calc_delta_vip(gran, vip_se);
}

/*
 * Should 'se' preempt 'curr'.
 *
 * se3             se2    curr         se1
 * ------|---------------|------|-----------|--------> vruntime
 *         |<------gran------>|
 *                        
 *
 *    wakeup_preempt_entity(curr, se1) = -1
 *    wakeup_preempt_entity(curr, se2) =  0
 *    wakeup_preempt_entity(curr, se3) =  1
 *
 */
// 满足抢占的条件就是，唤醒的进程的虚拟时间首先要比正在运行进程的虚拟时间小，并且差值还要大于
// 一定的值才行（这个值是sysctl_sched_wakeup_granularity，称作唤醒抢占粒度）。
// 这样做的目的是避免抢占过于频繁，导致大量上下文切换影响系统性能。
// unsigned int sysctl_sched_wakeup_granularity = 1000000UL;
static int
wakeup_preempt_vip_entity(struct sched_entity *curr, struct sched_entity *vip_se)
{
	s64 gran, vdiff = curr->vruntime - vip_se->vruntime;

	if (vdiff <= 0)
		return -1;

	gran = wakeup_gran_vip(curr, vip_se);
	if (vdiff > gran)
		return 1;

	return 0;
}

static void set_last_buddy_vip(struct sched_entity *vip_se)
{
	if (vip_entity_is_task(vip_se))
		return;

	for_each_sched_vip_entity(vip_se)
		vip_rq_of(vip_se)->last = vip_se;
}

static void set_next_buddy_vip(struct sched_entity *vip_se)
{
	if (vip_entity_is_task(vip_se))
		return;

	for_each_sched_vip_entity(vip_se)
		vip_rq_of(vip_se)->next = vip_se;
}

static int higher_than_vip(int policy)
{
	if (policy == SCHED_DEADLINE || policy == SCHED_FIFO || policy == SCHED_RR)
		return 1;
	
	return 0;
}

static void set_skip_buddy_vip(struct sched_entity *vip_se)
{
	for_each_sched_vip_entity(vip_se)
		vip_rq_of(vip_se)->skip = vip_se;
}

/*
 * Preempt the current task with a `newly woken` task if needed:
 */
// 	.check_preempt_curr	= check_preempt_wakeup_vip,
// 检查当前运行进程curr是否能被新唤醒进程p抢占 -- 满足条件则需标记 TIF_NEED_RESCHED
static void check_preempt_wakeup_vip(struct rq *rq, struct task_struct *p, int wake_flags)
{
	struct task_struct *curr = rq->curr;
	struct sched_entity *se = &curr->vip, *pse = &p->vip;
	struct vip_rq *vip_rq = task_vip_rq(curr);
	int scale = vip_rq->nr_running >= sched_nr_latency;
	int next_buddy_marked = 0;

	if (unlikely(se == pse))
		return;

	if (sched_feat(NEXT_BUDDY) && scale && !(wake_flags & WF_FORK)) {
		set_next_buddy_vip(pse);
		next_buddy_marked = 1;
	}

	/*
	 * We can come here with TIF_NEED_RESCHED already set from new task
	 * wake up path.
	 *
	 * Note: this also catches the edge-case of curr being in a throttled
	 * group (e.g. via set_curr_task), since update_curr() (in the
	 * enqueue of curr) will have resulted in resched being set.  This
	 * prevents us from potentially nominating it as a false LAST_BUDDY
	 * below.
	 */
	if (test_tsk_need_resched(curr))
		return;

	if (likely(higher_than_vip(p->policy)))
		goto preempt;

	if (!sched_feat(WAKEUP_PREEMPTION))
		return;

	update_curr_vip(vip_rq_of(se));
	BUG_ON(!pse);
	if (wakeup_preempt_vip_entity(se, pse) == 1) {
		// 满足抢占两个条件：vruntime更小，curr与主动抢占的vruntime差值满足一定条件避免频繁切换
		/*
		 * Bias pick_next to pick the sched entity that is
		 * triggering this preemption.
		 */
		if (!next_buddy_marked)
			set_next_buddy_vip(pse);
		goto preempt;
	}

	return;

preempt:
	resched_curr(rq);
	/*
	 * Only set the backward buddy when the current task is still
	 * on the rq. This can happen when a wakeup gets interleaved
	 * with schedule on the ->pre_schedule() or idle_balance()
	 * point, either of which can * drop the rq lock.
	 *
	 * Also, during early boot the idle thread is in the vip class,
	 * for obvious reasons its a bad idea to schedule back to it.
	 */
	if (unlikely(!se->on_rq || curr == rq->idle))
		return;

	if (sched_feat(LAST_BUDDY) && scale && vip_entity_is_task(se))
		set_last_buddy_vip(se);
}

static struct task_struct *pick_next_task_vip(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	struct task_struct *p;
	struct vip_rq *vip_rq = &rq->vip;
	struct sched_entity *vip_se;

	if (!vip_rq->nr_running)
		return NULL;

// TODO: Group scheduling & SMP 
// A basic version of handling group scheduling.

	if (prev)
		put_prev_task(rq, prev);

	do {
		vip_se = pick_next_vip_entity(vip_rq, NULL);
		set_next_vip_entity(vip_rq, vip_se);
		vip_rq = group_vip_rq(vip_se);
	} while (vip_rq);

	p = task_of(vip_se);

	return p;

}

/*
 * Account for a descheduled task:
 */
static void put_prev_task_vip(struct rq *rq, struct task_struct *prev)
{
	struct sched_entity *vip_se = &prev->vip;
	struct vip_rq *vip_rq;

	for_each_sched_vip_entity(vip_se) {
		vip_rq = vip_rq_of(vip_se);
		put_prev_vip_entity(vip_rq, vip_se);
	}
}

/*
 * sched_yield() is very simple
 *
 * The magic of dealing with the ->skip buddy is in pick_next_entity.
 */
static void yield_task_vip(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	struct vip_rq *vip_rq = task_vip_rq(curr);
	struct sched_entity *vip_se = &curr->vip;

	/*
	 * Are we the only task in the tree?
	 */
	if (unlikely(rq->vip_nr_running == 1))
		return;

	clear_buddies_vip(vip_rq, vip_se);

	update_rq_clock(rq);
	/*
	 * Update run-time vip_statistics of the 'current'.
	 */
	update_curr_vip(vip_rq);
	/*
	 * Tell update_rq_clock() that we've just updated,
	 * so we don't do microscopic update in schedule()
	 * and double the fastpath cost.
	 */
	rq_clock_skip_update(rq, true);

	set_skip_buddy_vip(vip_se);
}

static bool yield_to_task_vip(struct rq *rq, struct task_struct *p, bool preempt)
{
	struct sched_entity *vip_se = &p->vip;

	if (!vip_se->on_rq)
		return false;

	/* Tell the scheduler that we'd really like pse to run next. */
	set_next_buddy_vip(vip_se);

	yield_task_vip(rq);

	return true;
}

#ifdef CONFIG_SMP
static void rq_online_vip(struct rq *rq)
{
	update_sysctl();
}

static void rq_offline_vip(struct rq *rq)
{
	update_sysctl();
}

#endif /* CONFIG_SMP */

/*
 * scheduler tick hitting a task of our scheduling class:
 */
static void task_tick_vip(struct rq *rq, struct task_struct *curr, int queued)
{
	struct vip_rq *vip_rq;
	struct sched_entity *vip_se = &rq->curr;
	
	for_each_sched_vip_entity(vip_se) {
		vip_rq = vip_rq_of(vip_se);
		vip_entity_tick(vip_rq, vip_se, queued);
	}

	if (static_branch_unlikely(&sched_numa_balancing))
		task_tick_numa(rq, curr);
}

// 完成当前创建的新进程的虚拟时间初始化
/*
 * called on fork with the child task as argument from the parent's context
 *  - child not yet on the tasklist
 *  - preemption disabled
 */
// TODO:Step forward unserstanding the function.
static void task_fork_vip(struct task_struct *p)
{
	// 子进程虚拟时间为父进程且减去队列的minvruntime, enqueue_vip_entity不用多加新cpu队列的minvruntime
	struct vip_rq *vip_rq;
	struct sched_entity *se = &p->vip, *curr;
	int this_cpu = smp_processor_id();
	struct rq *rq = this_rq();
	unsigned long flags;

	raw_spin_lock_irqsave(&rq->lock, flags);

	update_rq_clock(rq);

	vip_rq = task_vip_rq(current);
	curr = vip_rq->curr;

	/*
	 * Not only the cpu but also the task_group of the parent might have
	 * been changed after parent->se.parent,vip_rq were copied to
	 * child->se.parent,vip_rq. So call __set_task_cpu() to make those
	 * of child point to valid ones.
	 */
	rcu_read_lock();
	__set_task_cpu(p, this_cpu);
	rcu_read_unlock();

	update_curr_vip(vip_rq);

	if (curr)
		se->vruntime = curr->vruntime;
	place_vip_entity(vip_rq, se, 1);

	if (sysctl_sched_child_runs_first && curr && vip_entity_before(curr, se)) {
		/*
		 * Upon rescheduling, sched_class::put_prev_task() will place
		 * 'current' within the tree based on its new key value.
		 */
		swap(curr->vruntime, se->vruntime);
		resched_curr(rq);
	}

	se->vruntime -= vip_rq->min_vruntime;

	raw_spin_unlock_irqrestore(&rq->lock, flags);
}

/*
 * Priority of the task has changed. Check to see if we preempt
 * the current task.
 */
static void
prio_changed_vip(struct rq *rq, struct task_struct *p, int oldprio)
{
	if (!p->vip.on_rq)
		return;

	/*
	 * Reschedule if we are currently running on this runqueue and
	 * our priority decreased, or if we are not currently running on
	 * this runqueue and our priority is higher than the current's
	 */
	if (rq->curr == p) {
		if (p->prio > oldprio)
			resched_curr(rq);
	} else
		check_preempt_curr(rq, p, 0);
}

static void switched_from_vip(struct rq *rq, struct task_struct *p)
{
	struct sched_entity *se = &p->vip;
	struct vip_rq *vip_rq = vip_rq_of(se);

	if (!p->on_rq && p->state != TASK_RUNNING) {
		/*
		 * Fix up our vruntime so that the current sleep doesn't
		 * cause 'unlimited' sleep bonus.
		 */
		place_vip_entity(vip_rq, se, 0);
		se->vruntime -= vip_rq->min_vruntime;
	}
}

static void switched_to_vip(struct rq *rq, struct task_struct *p)
{
	BUG_ON(!vip_prio(p->static_prio));

	if (p->vip.on_rq) {
		/*
		* We were most likely switched from sched_rt, so
		* kick off the schedule if running, otherwise just see
		* if we can still preempt the current task.
		*/
		if (rq->curr == p)
			resched_curr(rq);
		else
			check_preempt_curr(rq, p, 0);
	}
}

/* Account for a task changing its policy or group.
 *
 * This routine is mostly called to set vip_rq->curr field when a task
 * migrates between groups/classes.
 */
static void set_curr_task_vip(struct rq *rq)
{
	struct sched_entity *vip_se = &rq->curr->vip;

	for_each_sched_vip_entity(se) {
		struct vip_rq *vip_rq = vip_rq_of(vip_se);

		set_next_vip_entity(vip_rq, vip_se);
	}
}

/* Account for a task changing its policy or group.
 *
 * This routine is mostly called to set vip_rq->curr field when a task
 * migrates between groups/classes.
 */
static void set_next_task_vip(struct rq *rq, struct task_struct *p, bool first)
{
	struct sched_entity *vip_se = &p->vip;

#ifdef CONFIG_SMP
	if (task_on_rq_queued(p)) {
		/*
		 * Move the next running task to the front of the list, so our
		 * vip_tasks list becomes MRU one.
		 */
		list_move(&vip_se->group_node, &rq->vip_tasks);
	}
#endif

	for_each_sched_entity(vip_se) {
		struct vip_rq *vip_rq = vip_rq_of(vip_se);

		set_next_entity(vip_rq, vip_se);
		/* ensure bandwidth has been allocated on our new vip_rq */
		// account_vip_rq_runtime(vip_rq, 0);		// TODO
	}
}

// 初始化vip_rq的红黑树 -- start_kernel -> sched_init -> init_vip_rq
void init_vip_rq(struct vip_rq *vip_rq)
{
	vip_rq->tasks_timeline.rb_root = RB_ROOT;
	vip_rq->tasks_timeline.rb_leftmost = NULL;
	vip_rq->min_vruntime = (u64)(-(1LL << 20));
#ifndef CONFIG_64BIT
	vip_rq->min_vruntime_copy = vip_rq->min_vruntime;
#endif
}

#ifdef CONFIG_VIP_GROUP_SCHED
int alloc_vip_sched_group(struct task_group *tg, struct task_group *parent)
{
	struct sched_entity *vip_se;
	struct vip_rq *vip_rq;
	int i;

	tg->vip_rq = kcalloc(nr_cpu_ids, sizeof(vip_rq), GFP_KERNEL);
	if (!tg->vip_rq)
		goto err;
	tg->vip = kcalloc(nr_cpu_ids, sizeof(vip_se), GFP_KERNEL);
	if (!tg->vip)
		goto err;

	tg->shares = NICE_0_LOAD;

	// init_vip_bandwidth(tg_vip_bandwidth(tg));		// TODO

	for_each_possible_cpu(i) {
		vip_rq = kzalloc_node(sizeof(struct vip_rq),
				      GFP_KERNEL, cpu_to_node(i));
		if (!vip_rq)
			goto err;

		vip_se = kzalloc_node(sizeof(struct sched_entity),
				  GFP_KERNEL, cpu_to_node(i));
		if (!vip_se)
			goto err_free_rq;

		init_vip_rq(vip_rq);
		init_tg_vip_entry(tg, vip_rq, vip_se, i, parent->vip[i]);
		init_entity_runnable_average(vip_se);
	}

	return 1;

err_free_rq:
	kfree(vip_rq);
err:
	return 0;
}

// 组调度里初始化的调度实体vip_se的vip_rq成员指向系统中per-CPU变量rq的VIP调度队列，my_q 成员指向组调度(task_group)里自身的VIP调度队列
void init_tg_vip_entry(struct task_group *tg, struct vip_rq *vip_rq,
			struct sched_entity *vip_se, int cpu,
			struct sched_entity *parent)
{
	struct rq *rq = cpu_rq(cpu);

	vip_rq->tg = tg;
	vip_rq->rq = rq;
	init_vip_rq_runtime(vip_rq);

	tg->vip_rq[cpu] = vip_rq;
	tg->vip[cpu] = vip_se;

	/* vip_se could be NULL for root_task_group */
	if (!vip_se)
		return;

	if (!parent) {
		vip_se->vip_rq = &rq->vip;
		vip_se->depth = 0;
	} else {
		vip_se->vip_rq = parent->my_q;
		vip_se->depth = parent->depth + 1;
	}

	vip_se->my_q = vip_rq;
	/* guarantee group entities always have weight */
	update_load_set(&vip_se->load, NICE_0_LOAD);
	vip_se->parent = parent;
}

#else /* CONFIG_VIP_GROUP_SCHED */
int alloc_vip_sched_group(struct task_group *tg, struct task_group *parent)
{
	return 1;
}
#endif /* CONFIG_VIP_GROUP_SCHED */

/*
 * All the scheduling class methods:
 */
const struct sched_class vip_sched_class = {
	.next			= &fair_sched_class,	// 下一个优先级调度类
	.enqueue_task		= enqueue_task_vip,  // 一个 task 变成就绪状态, 希望挂上这个调度器的就绪队列(rq)
	.dequeue_task		= dequeue_task_vip,  // 一个 task 不再就绪了(比如阻塞了), 要从调度器的就绪队列上离开
	.yield_task		= yield_task_vip,	// 跳过当前任务（do_sched_yield调用<-yield调用/syscall调用）
	.yield_to_task		= yield_to_task_vip,	// yield_to_task_vip(p) 跳过当前任务, 并且尽量调度任务 p

	.check_preempt_curr	= check_preempt_wakeup_vip,	// 这个函数在有任务被唤醒时候调用, 看看能不能抢占当前任务

	.pick_next_task		= pick_next_task_vip,		// 选择下一个就绪的任务以运行
	.put_prev_task		= put_prev_task_vip,		// 	put_prev_task(rq, prev); /* 将切换出去进程插到队尾 */
	.set_next_task          = set_next_task_vip,		// This routine is mostly called to set vip_rq->curr field when a task migrates between groups/classes.

#ifdef CONFIG_SMP
	// .balance		= balance_vip,
	.select_task_rq		= select_task_rq_vip,
#ifdef CONFIG_VIP_GROUP_SCHED
	.migrate_task_rq	= migrate_task_rq_vip,
#endif
	.rq_online		= rq_online_vip,		// oneline offline 没必要
	.rq_offline		= rq_offline_vip,

	// .task_dead		= task_dead_vip,
	// .set_cpus_allowed	= set_cpus_allowed_common,
#endif
	.set_curr_task		= set_curr_task_vip;
	.task_tick		= task_tick_vip,		// 大部分情况下是用作时钟中断的回调 it might lead to process switch. This drives the running preemption.


// * called on fork with the child task as argument from the parent's context
//  *  - child not yet on the tasklist
//  *  - preemption disabled
	.task_fork		= task_fork_vip,		// 完成当前创建的新进程的虚拟时间初始化

	.prio_changed		= prio_changed_vip,
	.switched_from		= switched_from_vip,
	.switched_to		= switched_to_vip,

	// .get_rr_interval	= get_rr_interval_vip,

	.update_curr		= update_curr_vip,		// 更新当前运行任务的 vruntime & viprq的min_vruntime

#ifdef CONFIG_VIP_GROUP_SCHED
	.task_change_group	= task_change_group_fair,
#endif

// #ifdef CONFIG_UCLAMP_TASK
// 	.uclamp_enabled		= 1,
// #endif
};