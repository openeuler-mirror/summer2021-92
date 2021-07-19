/*
 *   File Name ：vip.h
 *   Author    ：Zhi Song
 *   Date      ：2021-07-16
 *   Descriptor：
 */

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SCHED_VIP_H
#define _SCHED_VIP_H

#define MIN_VIP_PRIO        100
#define MAX_VIP_PRIO        139
#define MAX_CFS_PRIO        179
#define VIP_PRIO_WIDTH      (MAX_VIP_PRIO - MIN_VIP_PRIO + 1)

/*
 * Convert user-nice values [ -20 ... 0 ... 19 ]
 * to vip static priority [ MIN_VIP_PRI + 1 ..MAX_VIP_PRIO ],
 * and back.
 */
#define NICE_TO_VIP_PRIO(nice)  (MAX_RT_PRIO + (nice) + 20)
#define PRIO_TO_VIP_NICE(prio)  ((prio) - MAX_RT_PRIO - 20)
#define TASK_VIP_NICE(p)        PRIO_TO_VIP_NICE((p)->static_prio)      // ?

extern unsigned int sched_vip_on;           /* Located in init/main.c */

static inline int cfs_prio(int prio)
{
	if (prio > MAX_VIP_PRIO && prio < MAX_PRIO)
		return 1;
	return 0;
}

static inline int vip_prio(int prio)
{
	if (prio >= MAX_RT_PRIO && prio < MAX_CFS_PRIO)
		return 1;
	return 0;
}

static inline void vip_prio_adjust_posi(int *prio)
{
	int priority = *prio;

	if (cfs_prio(priority))
		*prio = priority - VIP_PRIO_WIDTH;
}

static inline void vip_prio_adjust_nega(int *prio)
{
	int priority = *prio;

	if (vip_prio(priority))
		*prio = priority + VIP_PRIO_WIDTH;
}

#endif  /* _SCHED_VIP_H */
