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

/**************************************************************
 * VIP operations on generic schedulable entities:
 */

#ifdef CONFIG_VIP_GROUP_SCHED
/* An entity is a task if it doesn't "own" a runqueue */
#define vip_entity_is_task(vip)	(!vip->my_q)

/* Walk up scheduling entities hierarchy */
#define for_each_sched_vip_entity(vip_se) \
		for (; vip_se; vip_se = vip_se->parent)

#else	/* !CONFIG_VIP_GROUP_SCHED */

#define vip_entity_is_task(vip)	1

#define for_each_sched_vip_entity(vip_se) \
		for (; vip_se; vip_se = NULL)

#endif	/* CONFIG_VIP_GROUP_SCHED */	/******************************************************/

static inline struct task_struct *vip_task_of(struct sched_entity *vip_se)
{
	return container_of(vip_se, struct task_struct, vip);
}

static inline struct vip_rq *task_vip_rq(struct task_struct *p)
{
	return &task_rq(p)->vip;
}

static inline struct vip_rq *vip_rq_of(struct sched_entity *vip_se)
{
	struct task_struct *p = vip_task_of(vip_se);
	struct rq *rq = task_rq(p);

	return &rq->vip;
}

static inline struct rq *rq_of_vip_rq(struct vip_rq* vip_rq)
{
	return container_of(vip_rq, struct rq, vip);
}

static inline struct sched_entity *parent_vip_entity(struct sched_entity *vip_se)
{
	return NULL;
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
	u64 slice = __sched_period(vip_rq->nr_running + !vip_se->on_rq);

	for_each_sched_vip_entity(vip_se) {
		struct load_weight *load;
		struct load_weight lw;

		vip_rq = vip_rq_of(vip_se);
		load = &vip_rq->load;

		if (unlikely(!vip_se->on_rq)) {
			lw = vip_rq->load;

			update_load_add(&lw, vip_se->load.weight);
			load = &lw;
		}
		slice = __calc_delta(slice, vip_se->load.weight, load);
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
vip_entity_tick(struct vip_rq *vip_rq, struct sched_entity *curr, int queued)
{
	/*
	 * Update run-time bt_statistics of the 'current'.
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
 * update the fair scheduling stats:
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
	 * Also, during early boot the idle thread is in the fair class,
	 * for obvious reasons its a bad idea to schedule back to it.
	 */
	if (unlikely(!se->on_rq || curr == rq->idle))
		return;

	if (sched_feat(LAST_BUDDY) && scale && vip_entity_is_task(se))
		set_last_buddy_vip(se);
}

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

	// TODO NUMA tick judging
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

/*
 * All the scheduling class methods:
 */
const struct sched_class vip_sched_class = {
	.next			= &fair_sched_class,	// 下一个优先级调度类
	.enqueue_task		= enqueue_task_vip,  // 一个 task 变成就绪状态, 希望挂上这个调度器的就绪队列(rq)
	.dequeue_task		= dequeue_task_vip,  // 一个 task 不再就绪了(比如阻塞了), 要从调度器的就绪队列上离开
	// .yield_task		= yield_task_vip,	// 跳过当前任务
	// .yield_to_task		= yield_to_task_vip,	// yield_to_task_vip(p) 跳过当前任务, 并且尽量调度任务 p

	.check_preempt_curr	= check_preempt_wakeup_vip,	// 这个函数在有任务被唤醒时候调用, 看看能不能抢占当前任务

	// .pick_next_task		= __pick_next_task_vip,		// 选择下一个就绪的任务以运行
	// .put_prev_task		= put_prev_task_vip,		// 	put_prev_task(rq, prev); /* 将切换出去进程插到队尾 */
	// .set_next_task          = set_next_task_vip,		// This routine is mostly called to set vip_rq->curr field when a task migrates between groups/classes.

#ifdef CONFIG_SMP
	// .balance		= balance_vip,
	// .select_task_rq		= select_task_rq_vip,
	// .migrate_task_rq	= migrate_task_rq_vip,

	// .rq_online		= rq_online_vip,		// oneline offline 没必要
	// .rq_offline		= rq_offline_vip,

	// .task_dead		= task_dead_vip,
	// .set_cpus_allowed	= set_cpus_allowed_common,
#endif

	.task_tick		= task_tick_vip,		// 大部分情况下是用作时钟中断的回调 it might lead to process switch. This drives the running preemption.


// * called on fork with the child task as argument from the parent's context
//  *  - child not yet on the tasklist
//  *  - preemption disabled
	.task_fork		= task_fork_vip,		// 完成当前创建的新进程的虚拟时间初始化

	// .prio_changed		= prio_changed_vip,
	// .switched_from		= switched_from_vip,
	// .switched_to		= switched_to_vip,		// 上下文切换

	// .get_rr_interval	= get_rr_interval_vip,

	.update_curr		= update_curr_vip,		// 更新当前运行任务的 vruntime & viprq的min_vruntime

// #ifdef CONFIG_VIP_GROUP_SCHED
// 	.task_change_group	= task_change_group_vip,
// #endif

// #ifdef CONFIG_UCLAMP_TASK
// 	.uclamp_enabled		= 1,
// #endif
};