#ifndef _LINUX_BVT_SCHEDULE_H
#define _LINUX_BVT_SCHEDULE_H

#include <linux/glib.h>
#include <linux/heap.h>
#include <linux/time.h>

#ifdef CONFIG_HIGH_RES_TIMERS
#include <linux/hrtimer.h>
#else
#include <linux/timer.h>
#endif

#include <linux/coop_sched_domains.h>
#include <linux/coop_poll.h>

#define BVT_MIN_TIMESLICE 200 /* in usec */
extern struct timespec ts_bvt_min_timeslice;
extern volatile suseconds_t bvt_sched_granularity;
extern volatile unsigned int bvt_sched_tracing;

#define BVT_LOG_BUF_SIZE 1024
#define CONFIG_BVTPRD 20000

struct debug_entry
{
int isTargetSet;
struct timespec target_virtual_time;
struct timespec curr_timeslice;
struct timespec curr_time;
pid_t pid;
};


/* Fudge constants */
#define COOP_DEAD_FUDGE_DEFAULT 20000000 /* in nsecs, set at 20 ms */
#define COOP_DEAD_FUDGE_INC 100000 /* in nsecs, 100 microsecs */
#define COOP_DEAD_FUDGE_MAX 500000000 /* in nsecs, 500 ms */
/* Slack constants */
#define COOP_DEAD_SLACK 10000000 /* in nsecs , set 10 ms */

/* This is the main per cpu bvt queue structure 
 * This is a member of struct runqueue 
 * needed by sched.c 
 * Synchronization issues: The spinlock that protects the 
 * runqueue also protects the following data structure.
 * Any reference modification to bvtqueue must be done within
 * the corresponding runqueue spinlocks held.
 * There are two pairs of inline functions defined in linux/sched.h 
 * that locks the runqueue and returns a reference to the corresponding bvtqueue. 
 * They are:
 * 1. inline bvtqueue* get_task_bq_locked(struct task_struct *tsk, 
 *                                        unsigned long *flags);
 * 2. inline void put_task_bq_locked(struct bvt_queue *bq, unsigned long flags);
 * 3. inline bvtqueue* get_cpu_bq_locked(int cpu, unsigned long *flags);
 * 4. inline void put_cpu_bq_locked(int cpu,  unsigned long flags);
 * 5. inline void get_task_bq(struct task_struct *p);
 * All accesses must be done through these functions.
 */
struct bvtqueue
{
	heap_t*   bvt_heap;
	heap_t*   global_coop_deadline_heap; /* Per cpu global heap for storing all the deadline events */
	heap_t*	  global_coop_sleep_heap; /* Per cpu global heap for storing deadlines for all sleeping coop tasks*/
	/* The pointer to the running bvt task */
	struct task_struct *running_bvt_task;

	/* the current scheduling timeslice 
	 * in timespec 
	 */
	struct timespec curr_bvt_period;

#ifdef CONFIG_HIGH_RES_TIMERS
	/* The bvt high resolution timer */
	struct hrtimer bvt_timer;
#else
	struct timer_list bvt_timer;
#endif

	/* Per cpu debug log */
	struct debug_entry bvt_debug_buffer[BVT_LOG_BUF_SIZE];
	unsigned int bvtLogCount;

	/* this is bad design: we only have global coop domains 
	 * The best effort domain parameters are per task basis.
	 * yet, we have one slot for bvt domain.
	 */
	struct bvt_domain bvt_domains[DOM_MAX_TYPE];

	/* the coop queues also are now in the runqueue 
	 * instead of being a per CPU variable themselves
	 */
	coop_queue cq[NR_COOP_DOMAINS];
	
	/* This is used to keep track of the last coop deadline and
	 * calculate the coop period
	 */
	struct timeval last_coop_deadline;
	struct timespec max_virtual_time;
	int isTargetSet;
	bool fudged_flag;
	struct timespec ts_slack; /* Slack given to coop tasks before they get policed*/
	struct timespec ts_now; /* Our scheduler's view of monotonic time */

	/* Stat Variables */	
	unsigned long adj;
	unsigned long noadj;
	unsigned long fudge;
	unsigned long nofudge;
	unsigned int count;
	struct timespec tot_time; /* Total time = user time + system time */	

	int bvt_timer_active;
};


/* bvt_struct is the bvt specific declarations that
 * must be incorporated into the task_struct
 * used by include/linux/sched.h
 */

struct bvt_struct
{
   	struct timespec               bvt_timeslice_start;
	struct timespec               bvt_timeslice_end;
	
	struct fairshare_sched_param  private_sched_param;

	struct task_struct            *me; 
	int                           is_well_behaved;
	
};

/* This function returns true if a task is 
 * under our scheduling regime.
 */

#define is_bvt(p) (p->sched_class == &faircoop_sched_class) 

#ifdef CONFIG_HIGH_RES_TIMERS
#define bvt_timer_cancel(timer) hrtimer_cancel(timer); 
#else
#define bvt_timer_cancel(timer) del_timer(timer); 
#endif

/* global bvt init function; called from init/main.c:start_kernel()
 */
void __init bvt_global_init(void);

/* bvt proc initialization function called from
 * kernel/fork.c:copy_process()
 */
void bvt_proc_init(struct task_struct *p);

/* bvt proc destroy, called from kernel/exit.c: do_exit()
 */
void detach_coop_fairshare_sched(struct task_struct* tsk);

extern int is_best_effort(struct task_struct* tsk);

extern struct file_operations proc_bvtstat_operations;

void insert_task_into_bvt_queue(struct bvtqueue *bq, 
				struct task_struct *t);
void __do_set_bvt(struct task_struct *p, int need_lock);
void remove_task_from_bvt_queue(struct bvtqueue *bq, 
				struct task_struct *p);
void do_policing(struct bvtqueue *bq, 
		 struct task_struct *tsk);

void init_bvt_domain(struct bvtqueue*,struct task_struct*);

void tv_fairshare_now_adjusted(struct timeval*);

struct bvtqueue* get_task_bq_locked(struct task_struct *tsk, unsigned long *flags);
void put_task_bq_locked(struct bvtqueue *bq, unsigned long *flags);
struct bvtqueue* get_cpu_bq_locked(int cpu, unsigned long *flags);
void put_cpu_bq_locked(int cpu, unsigned long *flags);
struct bvtqueue* cpu_bq(int cpu);
struct bvtqueue* get_task_bq(struct task_struct *tsk);
#endif /*  _LINUX_BVT_SCHEDULE_H */

