/* SPDX-License-Identifier: GPL-2.0 */

#ifdef CONFIG_VIP_SCHED
#define CPUPRI_NR_PRIORITIES	(MAX_RT_PRIO + 42)		// MAX_RT_PRIO: RT, 40: VIP, 2: IDLE CFS

#define CPUPRI_INVALID		-1
#define CPUPRI_IDLE		 	 0
#define CPUPRI_NORMAL		 1
// CPUPRI_VIP: 3-42			 
/* values 43-102 are RT priorities 0-99 */

#else
#define CPUPRI_NR_PRIORITIES	(MAX_RT_PRIO + 2)

#define CPUPRI_INVALID		-1
#define CPUPRI_IDLE		 	 0
#define CPUPRI_NORMAL		 1
/* values 2-102 are RT priorities 0-99 */

#endif	/* CONFIG_VIP_SCHED */

struct cpupri_vec {
	atomic_t		count;
	cpumask_var_t		mask;
};

struct cpupri {
	struct cpupri_vec	pri_to_cpu[CPUPRI_NR_PRIORITIES];
	int			*cpu_to_pri;
};

#ifdef CONFIG_SMP
int  cpupri_find(struct cpupri *cp, struct task_struct *p, struct cpumask *lowest_mask);
void cpupri_set(struct cpupri *cp, int cpu, int pri);
int  cpupri_init(struct cpupri *cp);
void cpupri_cleanup(struct cpupri *cp);
#endif
