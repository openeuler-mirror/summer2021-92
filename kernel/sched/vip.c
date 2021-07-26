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

// 初始化vip的红黑树(vip_rq)
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
/* An entity is a task if it doesn't "own" a runqueue */
#define vip_entity_is_task(vip)	(!vip->my_q)

#else
#define vip_entity_is_task(vip)	1
#endif

#define for_each_sched_vip_entity(vip) \
		for (; vip; vip = NULL)

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

// TODO
// 需要不断被更新
// min_vruntime太小，导致后面创建的新进程根据这个值来初始化新进程的虚拟时间，岂不是新创建的进程有可能再一次疯狂了。这一次可能就是cpu0创建，在cpu0上面疯狂
static void update_vip_min_vruntime(struct vip_rq *vip_rq)
{
	// vip队列中当前运行任务vruntime可能是minvruntime

	// vip队列中红黑树最左下节点可能是minvruntime

	可以想一下，如果一个进程刚睡眠1ms，然后醒来后你却要奖励3ms（虚拟时间减去3ms），然后他竟然赚了2ms。作为调度器，我们不做亏本生意。你睡眠100ms，奖励你3ms，那就是没问题的
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
	for_each_sched_entity(vip_se) {
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

static void
account_vip_entity_enqueue(struct vip_rq *vip_rq, struct sched_entity *vip_se)
{
	update_load_add(&vip_rq->load, vip_se->load.weight);

	vip_rq->nr_running++;
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
	 * been changed after parent->se.parent,cfs_rq were copied to
	 * child->se.parent,cfs_rq. So call __set_task_cpu() to make those
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
 * All the scheduling class methods:
 */
const struct sched_class vip_sched_class = {
	.next			= &fair_sched_class,	// 下一个优先级调度类
	.enqueue_task		= enqueue_task_vip,  // 一个 task 变成就绪状态, 希望挂上这个调度器的就绪队列(rq)
	// .dequeue_task		= dequeue_task_vip,  // 一个 task 不再就绪了(比如阻塞了), 要从调度器的就绪队列上离开
	// .yield_task		= yield_task_vip,	// 跳过当前任务
	// .yield_to_task		= yield_to_task_vip,	// yield_to_task_vip(p) 跳过当前任务, 并且尽量调度任务 p

	// .check_preempt_curr	= check_preempt_curr_vip,	// 这个函数在有任务被唤醒时候调用, 看看能不能抢占当前任务

	// .pick_next_task		= __pick_next_task_vip,		// 选择下一个就绪的任务以运行
	// .put_prev_task		= put_prev_task_vip,		// 	put_prev_task(rq, prev); /* 将切换出去进程插到队尾 */
	// .set_next_task          = set_next_task_vip,		// This routine is mostly called to set vip_rq->curr field when a task migrates between groups/classes.

#ifdef CONFIG_SMP
	// .balance		= balance_vip,
	// .select_task_rq		= select_task_rq_vip,
	// .migrate_task_rq	= migrate_task_rq_vip,

	// .rq_online		= rq_online_vip,
	// .rq_offline		= rq_offline_vip,

	// .task_dead		= task_dead_vip,
	// .set_cpus_allowed	= set_cpus_allowed_common,
#endif

	// .task_tick		= task_tick_vip,		// 大部分情况下是用作时钟中断的回调 it might lead to process switch. This drives the running preemption.


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