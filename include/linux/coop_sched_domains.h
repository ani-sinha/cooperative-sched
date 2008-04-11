#ifndef _LINUX_COOP_SCHED_DOMAINS_H 
#define _LINUX_COOP_SCHED_DOMAINS_H  

/* This header defines the scheduling domains related to the 
 * cooperative scheduling regime.
 * Also defined the sched_param struct.
 * In general, we have two scheduling domains - one which
 * incorporates all the cooperative real time processes. The
 * other incorporates all the best effort processes. All best 
 * effort processes are scheduled by the fair share scheduler
 * based on their individual *virtual time* whereas all the real 
 * time processes are scheduled as a group and the group as a whole 
 * has a single virtual time.
 * 
 * This is a joint work of all the following authors:
 * Anirban Sinha, anirbans@cs.ubc.ca
 * Charles Krasic, krasic@cs.ubc.ca
 * Ashvin Goel, ashvin@eecg.toronto.edu
 */

#include <linux/types.h>
#include <linux/time.h>
#include <linux/spinlock_types.h>

enum domain_type 
{	
	DOM_REALTIME_COOP0,
	DOM_REALTIME_COOP1,
	DOM_REALTIME_COOP2,
	DOM_REALTIME_COOP3,
	DOM_REALTIME_COOP4,
	DOM_REALTIME_COOP5,
	DOM_REALTIME_COOP6,
	DOM_REALTIME_COOP7,
	DOM_REALTIME_COOP8,
	DOM_REALTIME_COOP9,
	DOM_REALTIME_COOP10,
	DOM_REALTIME_COOP11,
	DOM_REALTIME_COOP12,
	DOM_REALTIME_COOP13,
	DOM_REALTIME_COOP14,
	DOM_REALTIME_TEMP,
	DOM_BEST_EFFORT,
	DOM_MAX_TYPE,
	DOM_LEAVE,
};

#define NR_COOP_DOMAINS (DOM_MAX_TYPE - 1)

/* the following structure keeps track of the scheduling parameters
 * in our scheduling regime. 
 */

struct fairshare_sched_param {
	enum domain_type              dom_type;
	struct timespec               bvt_virtual_time;
	struct timespec               bvt_actual_time;
	struct timespec               insertion_ts;
	heap_node*                    bheap_ptr;
	struct timespec		      fudge; /* Amount of fudge it can burn at a go before waiting for replenishment */
};


/* every task in our scheduling regime belongs to one 
 * of the domains specified above 
 * Each of the domains will have some per cpu data and some 
 * global data. The current implementation is kept open
 * so that we can enhance it in the future.
 * This structure is a member of each per cpu runqueue.
 */
struct bvt_domain {
	
	enum domain_type dom_type;
	
        /* number of kernel tasks in this domain. 
	 * in SMP, this figure is per cpu basis.  
	 */
	long num_tasks;
	/* This is per domain scheduling parameters currently, this is
	 * only relevant for real coop domains as best effort tasks
	 * use their private per task sched parameters
	 */
	struct fairshare_sched_param dom_sched_param;
	
	/* ... + other per domain stuff that can be added in future */
};

#define for_each_available_coop_domain(dom_id) \
	for((dom_id)=0; (dom_id) < (NR_COOP_DOMAINS); (dom_id)++)

#define task_domain(tsk) (tsk)->cf.dom_id

#endif





