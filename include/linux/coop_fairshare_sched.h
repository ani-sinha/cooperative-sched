#ifndef _LINUX_COOP_FAIRSHARE_SCHED_H 
#define _LINUX_COOP_FAIRSHARE_SCHED_H 

#ifdef __KERNEL__

/* This header defines all the structures that are 
 * common for implementing both coop_poll and fairshare 
 * scheduling. Specific declarations for coop_poll are in
 * linux/coop_poll.h and specific declarations for fairshare 
 * are in linux/bvt_schedule.h 
 */

#include <linux/coop_poll.h>
#include <linux/sched_fairshare.h>
#include <linux/heap.h>

/* this is the per task structure embedded in task_struct */
struct coop_fairshare_struct 
{
	struct bvt_domain *bvt_dom;
	struct fairshare_sched_param *task_sched_param;
	struct coop_struct coop_t;
 	struct bvt_struct bvt_t;
	int dom_id;
};

extern struct bvt_domain bvt_domains[];

#endif /* __KERNEL__ */
#endif
