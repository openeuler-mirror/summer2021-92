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