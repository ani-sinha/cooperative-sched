/* This is the kernel bvt scheduler for cooperative processes 
 * It is hoped that the bvt scheduling with facilitate uniform sharing 
 * of cpu resources and reduce the need for cooperative applications to 
 * mutually trust each other. This is also the inherent mechanism
 * for policing and scheduling best effort tasks.
 * This is the kernel bvt scheduler for cooperative processes 
 * It is hoped that the bvt scheduling with facilitate uniform sharing 
 * of cpu resources and reduce the need for cooperative applications to 
 * mutually trust each other. This is also the inherent mechanism
 * for policing and scheduling best effort tasks.
 *
 * High res timer incorporated: April 1 1, 2007.
 * Details on high res timers is here: 
 * http://lwn.net/Articles/167897/ 
 * 
 * Profiling of bvt syscall per process incorporated:
 * April 24, 2007.
 * 
 * Experimental new heuristic that enforces the period 
 * more strongly merged: April 26 2007
 * 
 * Policing and coop-real time domains introduced:
 * May 22, 2007.
 * 
 * This is the joint work of all the following authors:
 * Mayukh Saubhasik, mayukh@cs.ubc.ca
 * Anirban Sinha, anirbans@cs.ubc.ca
 * Charles 'Buck' Krasic, krasic@cs.ubc.ca
 * Ashvin Goel, ashvin@eecg.toronto.edu

 * Note about timekeeping: 
 * kernel uses all different forms and notions about what monotonic "now" is.
 * Please check fairshare_now() function for explanations
 * on a few. However, our user level coop processes uses do_gettimeofday()
 * system call to register their "now" value. This is actually the wall 
 * clock time. Thus, all coop related 
 * time comparisons (and while reporting time to userspace) 
 * uses do_gettimeofday() call to get the value of 
 * "now". Unfortunately, this value is not the same as the monotonic clock
 * value (see wall_to_monotonic offset timespec) that is used by the 
 * timer code (highres timers or otherwise). 
 * Hence, we are forced to use two different notions of time, 
 * do_gettimeofday() and fairshare_now(). The former is used to report 
 * and compare time values in the coop heap. The later is used by the 
 * fairshare timer scheduling code.  
 */

#include <linux/types.h>
#include <linux/heap.h>
#include <asm/uaccess.h>
#include <linux/seq_file.h>
#include <linux/fs.h>
#ifdef CONFIG_HIGH_RES_TIMERS
#include <linux/ktime.h>
#endif
#include <linux/coop_fairshare_sched.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <asm/types.h>
#include <asm/cputime.h>
#include<linux/kernel_stat.h>

/* Read only data*/
/* This is the global bvt timeslice 
 * period in microseconds, can be asynchronously updated thru the proc filesystem*/
volatile suseconds_t bvt_sched_granularity;
volatile unsigned int bvt_sched_tracing;
/*volatile struct timespec ts_fudge;*/

struct timespec ts_bvt_min_timeslice;

/* End of read only global data */

#if defined(CONFIG_SCHED_COOPREALTIME)

#define BVT_DEBUG_FLAG "[BVT]"

#include <linux/print_debug.h>
#define bvt_print_debug(fmt,arg...) do { \
		print_debug(BVT_DEBUG_FLAG, fmt,##arg);   \
	}while(0);

#define BVT_HEAP_INIT_CAP 1024

/* Forward declarations */
static void bvt_borrow(struct rq *rq,struct task_struct *p,int wakeup);
static void dequeue_task_faircoop(struct rq *rq, struct task_struct *p,int sleep);
static void yield_task_faircoop(struct rq *rq);
static void check_preempt_faircoop(struct rq* rq, struct task_struct *p);
static struct task_struct* __sched pick_next_task_arm_timer(struct rq *rq);
static void __sched update_bvt_prev(struct rq *rq, struct task_struct *prev);
static void set_curr_task_faircoop(struct rq *rq);
static void task_tick_faircoop(struct rq *rq, struct task_struct *p, int queued);
static void task_new_faircoop(struct rq *rq, struct task_struct *p);
static void switched_from_faircoop(struct rq *this_rq, struct task_struct *task, int running);
static void switched_to_faircoop(struct rq *this_rq, struct task_struct *task, int running);
static void prio_changed_faircoop(struct rq* this_rq, struct task_struct *task, int oldprio, int running);

#ifdef CONFIG_SMP
static unsigned long
load_balance_faircoop(struct rq *this_rq, int this_cpu, struct rq *busiest,
		  unsigned long max_load_move,
		  struct sched_domain *sd, enum cpu_idle_type idle,
		  int *all_pinned, int *this_best_prio);


static int
move_one_task_faircoop(struct rq *this_rq, int this_cpu, struct rq *busiest,
		   struct sched_domain *sd, enum cpu_idle_type idle);

static void select_task_rq_faircoop(struct task_struct *p, int sync);
static void join_domain_faircoop(struct rq* rq);
static void leave_domain_faircoop(struct rq* rq);
#endif

/* Scheduling class struct */
static const struct sched_class faircoop_sched_class = {
	.next = &idle_sched_class,
	.enqueue_task = bvt_borrow,
	.dequeue_task = dequeue_task_faircoop,
	.yield_task = yield_task_faircoop,
	.check_preempt_curr = check_preempt_faircoop,
	.pick_next_task = pick_next_task_arm_timer,
	.put_prev_task = update_bvt_prev,
	.set_curr_task = set_curr_task_faircoop,
	.task_tick = task_tick_faircoop,
	.task_new = task_new_faircoop,

#ifdef CONFIG_SMP
	.load_balance 	= load_balance_faircoop,
	.move_one_task 	= move_one_task_faircoop,
	.select_task_rq = select_task_rq_faircoop,
	.join_domain = join_domain_faircoop,
	.leave_domain = leave_domain_faircoop
#endif

	.prio_changed = prio_changed_faircoop,
	.switched_to = switched_to_faircoop,
	.switched_from = switched_from_faircoop
};


inline struct bvtqueue* get_task_bq_locked(struct task_struct *tsk, 
					   unsigned long *flags) 
{
    struct rq *rqueue;
	rqueue = task_rq_lock(tsk, flags);
	return &rqueue->bq;
}

inline void put_task_bq_locked(struct bvtqueue* bq, 
			       unsigned long *flags) 
{
	struct rq *rqueue = container_of(bq,struct rq,bq);
	task_rq_unlock(rqueue,flags);
}

inline struct bvtqueue* get_cpu_bq_locked(int cpu, unsigned long *flags) 
{
	struct rq *rqueue;
	local_irq_save(*flags);
	rqueue = cpu_rq(cpu);
	spin_lock_irq(&rqueue->lock);
	return &rqueue->bq;
}


inline void put_cpu_bq_locked(int cpu, unsigned long *flags) 
{

	struct rq *rqueue = cpu_rq(cpu);
	spin_unlock_irqrestore(&rqueue->lock, *flags);
}

inline struct bvtqueue* cpu_bq(int cpu)
{
	struct rq *rqueue;
	rqueue = cpu_rq(cpu);
	return &rqueue->bq;
}

inline struct bvtqueue* get_task_bq(struct task_struct *p)
{
	struct rq *rqueue = task_rq(p);
	return &rqueue->bq;
}

inline int is_best_effort(struct task_struct* p) 
{
	if (!p->cf.bvt_dom) return 0;
	return (task_domain(p) == DOM_BEST_EFFORT);
}

void do_policing(struct bvtqueue *bq, struct task_struct *tsk);

static inline struct task_struct* get_task(struct fairshare_sched_param* sp)
{
	struct bvt_struct *bs;
	if (sp->dom_type == DOM_BEST_EFFORT) {
		bs = container_of(sp,struct bvt_struct,private_sched_param); 
		return bs->me;
	}else /* pointer to real coop sched_param. They are not specifically associated 
	       * with any task */
		return NULL;
}

/* gets the current kernel time using the monotonic clock.
 * @ts: The timespect value that represents *current time* 
 */
static inline void fairshare_now(struct timespec *ts) {
	/* There are several functions that returns the value of "now":- 
	 * 
	 * 1. current_kernel_time() simply reads xtime variable
	 *    using the seq lock and returns the value directly.  xtime
	 *    is updated on every global timer interrupt for SMP machines
	 *    with a global time source. It is not updated on the local
	 *    APIC timer interrupt which keeps track of the process
	 *    accounting and is often programmed to fire at HZ
	 *    frequency. For UP machines, there is only one time source
	 *    anyways, so it is updated before rest of the process time
	 *    accounting code in the same handler.
	 *
	 *    *ts = current_kernel_time();
	 * 
	 * 2. In NON-NUMA machines & machines where 
	 *    tsc is reliable, sched_clock() first does an rdtsc() to read the 
	 *    time stamp counter. It then converts that value to ns. The
	 *    NUMA case is a special case where TSC is not synchronized
	 *    across all cpus. In those cases and where tsc is unstable,
	 *    sched_clock() uses the 64 bit
	 *    jiffies along with HZ to arrive at a ns value.  
	 *  
	 *    *ts = ns_to_timespec(sched_clock());
	 *
	 * 3. ktime_get_ts: The difference between current_kernel_time()
	 *    and the one below is that
	 *    current_kernel_time() returns the xtime value directly
	 *    whereas ktime_get_ts() does a interpolation correction
	 *    on the top of xtime value to get the wall time. It then
	 *    converts wall time to monotonic time. It is also the
	 *    interface used by the highres timers. Hence I hope that
	 *    use of this function will make timing accounting more
	 *    fine grained.
	 */

	ktime_get_ts(ts);

} /* fairshare_now */

/* insert_task_into_bvt_queue:
 * @t: the task_struct of the task 
 * @bq: pointer to the per cpu bvt queue
 * @flag: whether we are allowed to grow the stack, = 0:no, = 1:yes
 * and put into the queue
 * The corresponding runqueue lock must be acquired before calling
 * this function.
 */

void insert_task_into_bvt_queue(struct bvtqueue *bq, 
				struct task_struct *t)
{
	g_assert(t);
	g_assert(is_bvt(t));

	if(!is_bvt(t)) return;

	fairshare_now(&t->cf.task_sched_param->insertion_ts);
	//t->cf.task_sched_param->insertion_ts = ns_to_timespec(sched_clock());

	t->cf.task_sched_param->bheap_ptr = 
			heap_insertt(bq->bvt_heap, 
				    t->cf.task_sched_param, 
				    t->cf.task_sched_param);

	if (!t->cf.task_sched_param->bheap_ptr) { 
		bvt_print_debug("heap_insert returned null:"
				"Unable to insert proc node into bvt heap");
		panic("Unable to insert proc node into bvt heap");
	}
}/* insert_task_into_bvt_queue */



/* find_fairshare_period:
 * finds the fairshare period for the current CPU
 * Note that based on whether we are scheduling best effort
 * or real-coop, the fairshare period will differ.
 * @fair_share_period: output parameter, the period
 * @for_coop_realtime: whether we are finding the period 
 * for real coop tasks. 
 */
static void find_fairshare_period(suseconds_t *fair_share_period,
				  struct task_struct *next)
{
	struct bvtqueue *bq;
	unsigned long  nr_tasks;
	unsigned long  nr_besteffort = 0;
	unsigned long nr_realcoop = 0;
	int dom_id;

	bq = cpu_bq(smp_processor_id());

	nr_besteffort = bq->bvt_domains[DOM_BEST_EFFORT].num_tasks;
	
	for_each_available_coop_domain(dom_id) {
		nr_realcoop   += bq->bvt_domains[dom_id].num_tasks;
	}

	nr_tasks = nr_besteffort + nr_realcoop;

	if (nr_tasks) {
		*fair_share_period = bvt_sched_granularity / nr_tasks;
	}else {
		printk (KERN_ERR "undefined fair share period: impossible, nrtasks = %d %d %d\n",nr_tasks,nr_besteffort,nr_realcoop);
		BUG();
	}
	if (is_coop_realtime(next)) {
		*fair_share_period = *fair_share_period * 
		  bq->bvt_domains[task_domain(next)].num_tasks;
	}
}
/* find_fairshare_period() */

/* calculate_bvt_period: 
 * This is the part where we find the timeslice based on the 
 * preferential treatment given to the coop processes 
 * @next: The next chosen bvt process, either real-coop or
 * best effort.
 */
static void calculate_bvt_period(struct task_struct *next)
{
	struct timespec ts_coop_prd;
	struct timespec ts_zero;
	struct task_struct *next_coop_task;
	suseconds_t fair_share_period;
	int period_set;
	struct timespec period;
	struct bvtqueue *bq;

	next_coop_task = NULL;
	memset(&ts_zero,0,sizeof(struct timespec));
	bq = cpu_bq(smp_processor_id());
	
	if (bq->fudged_flag) {
	
		/* This task is running coz of fudging 
 		   Therefore it only gets a timeslice of zero, forcing it to yield as soon as its
		   done with its deadline event */
		bq->curr_bvt_period = ts_zero;
		
		/* Reset the fudged flag */
		bq->fudged_flag = false;

	}
	else {
	
		find_fairshare_period(&fair_share_period, next);

		period = ns_to_timespec(fair_share_period * NSEC_PER_USEC);

		/* get the next most important deadline event from the 
		 * current heap. Also get the time until the next 
		 * coop event (timeout)
		 */
		period_set = find_coop_period(next, &next_coop_task, &ts_coop_prd);

		if (period_set) 
			period = (timespec_compare(&period, &ts_coop_prd) < 0)? period:ts_coop_prd;
		
		/* this is where we limit the number of context switches */
		bq->curr_bvt_period = 
			(timespec_compare(&period, &ts_bvt_min_timeslice) < 0)?
			ts_bvt_min_timeslice : period;
		
	}

} /* calculate_bvt_period */

/* This is actually the borrowing part for interractive applications
 * It is called from activate_task in sched.c
 * The scheduling class enqueue function is supposed to set the 
 * on_rq flag
 */
static void bvt_borrow(struct rq *rq,struct task_struct *p,int wakeup)
{
	struct fairshare_sched_param          *top_node = NULL;
	struct timeval                        tv_zero = { .tv_sec = 0, .tv_usec = 0};
	struct bvtqueue* bq;

	if(rq == NULL) {
		printk(KERN_ERR "rq null\n");
		return;
	}
	
	if(p == NULL) {
		printk(KERN_ERR "Task is null\n");
		return;
	}

	if(!is_bvt(p)) {
		printk(KERN_ERR "Task is not in faircoop scheduling class\n");
		return;
	}
	
	bq = &rq->bq;

	p->cf.bvt_dom->num_tasks++;

    /* handle real coop wakeups seperately here */
	if (is_coop_realtime(p)) {

		int dom_id = task_domain(p);

		/* reinsert the coop nodes into the coop heap */
		if (timeval_compare(&(p->cf.coop_t.dead_p.t_deadline), 
		&tv_zero) > 0) {
			/* insert into timeout heap */
			if (bq == NULL) {
				printk(KERN_ERR "bq is null\n");
				return;
			}
			insert_task_into_timeout_queue(NULL,&(bq->cq[dom_id]),p,0);
			/* and remove from coop sleep queue */
			remove_task_from_coop_queue(p, &(bq->cq[dom_id]),3);
		}			
		/* if there are other coops running, do not borrow and do not
		 * insert my nodes into the heap 
		 */
		if (p->cf.bvt_dom == NULL) {
			printk(KERN_ERR "bvt_dom is null\n");
			return;
		}
		if (task_domain(p) != DOM_REALTIME_TEMP && 
			(p->cf.bvt_dom->num_tasks >1)) 
			return;
	} /* if */
	
	if (likely(!heap_is_empty(bq->bvt_heap))) {
		top_node = (struct fairshare_sched_param*) heap_min_data(bq->bvt_heap);
	}

	/* Only borrow forward, borrowing backward allows a task to cheat on fairness */
	if (top_node) { 
		if( (timespec_compare(&top_node->bvt_virtual_time, &p->cf.task_sched_param->bvt_virtual_time) > 0) ) 
			p->cf.task_sched_param->bvt_virtual_time = top_node->bvt_virtual_time;
	}
	
	/* DO NOT reset the virtual time of the system, this again allows tasks to cheat, if they time it right */
	/* This will overflow in 136 years */
	else{ 
		if (p->cf.task_sched_param == NULL) {
			printk(KERN_ERR "task_sched_param null\n");
			return;
		}
		p->cf.task_sched_param->bvt_virtual_time = bq->max_virtual_time;

	}
	
	/* insert task into the bvt queue 
	 */
	if (likely(!p->cf.task_sched_param->bheap_ptr)) {
		insert_task_into_bvt_queue(bq,p); 
	}

	p->se.on_rq = 1;
}
/* bvt_borrow */

/* update_virtual_times: Update the virtual times
 * of the best effort and the real-coop guys 
 * This function is called with the runQ lock held
 */
static void update_virtual_times(struct task_struct *p)
{
	struct timespec    ts_delta;
	long nr_realcoop = 0;
	suseconds_t nsec;
	cputime_t cputime;
	cputime64_t tmp;
	unsigned long delta_exec;	
	//struct cpu_usage_stat *cpustat;

	struct bvtqueue *bq = cpu_bq(smp_processor_id());

	p->cf.bvt_t.bvt_timeslice_end = cpu_bq(task_cpu(p))->ts_now;
	//p->cf.bvt_t.bvt_timeslice_end = ns_to_timespec(task_rq(p)->clock);
        
	ts_delta = timespec_sub(p->cf.bvt_t.bvt_timeslice_end, p->cf.bvt_t.bvt_timeslice_start); 
	
	/* Stats */

	/* Update the actual time, before incorporating domain details */
	set_normalized_timespec(&p->cf.bvt_t.private_sched_param.bvt_actual_time, 
				p->cf.bvt_t.private_sched_param.bvt_actual_time.tv_sec  + ts_delta.tv_sec,
				p->cf.bvt_t.private_sched_param.bvt_actual_time.tv_nsec + ts_delta.tv_nsec);

	set_normalized_timespec(&bq->tot_time,bq->tot_time.tv_sec + ts_delta.tv_sec,bq->tot_time.tv_nsec + ts_delta.tv_nsec);

	/* Convert actual time to jiffies using HZ, and override utime value*/
	//p->utime = timespec_to_cputime(&p->cf.bvt_t.private_sched_param.bvt_actual_time);
	//p->utimescaled = cputime_to_scaled(timespec_to_cputime(&p->cf.bvt_t.private_sched_param.bvt_actual_time));

	/* Update cfs stats */
	delta_exec = (unsigned long)(task_rq(p)->clock - p->se.exec_start);
	p->se.sum_exec_runtime += delta_exec;

	//cputime = timespec_to_cputime(&bq->tot_time);
	//cpustat = &kstat_this_cpu.cpustat;
	/* Convert total time to cputime. */
	//tmp = cputime_to_cputime64(cputime);
	//cpustat->user = tmp;

	/* Stats end */

	if (is_coop_realtime(p) && task_domain(p) != DOM_REALTIME_TEMP) 
		nr_realcoop = bq->bvt_domains[task_domain(p)].num_tasks;

	/* note: nr_realcoop will be 0 only when a single real-coop
	 * guy is running in the system in this domain and is going into a
	 * cooperative sleep.
	 */

	if (is_coop_realtime(p) && (nr_realcoop > 1) ) {
		
		nsec     = (suseconds_t) timespec_to_ns(&ts_delta);
		nsec     = nsec / nr_realcoop; 
		ts_delta = ns_to_timespec(nsec);
	}
	
	set_normalized_timespec(&p->cf.task_sched_param->bvt_virtual_time, 
				p->cf.task_sched_param->bvt_virtual_time.tv_sec  + ts_delta.tv_sec,
				p->cf.task_sched_param->bvt_virtual_time.tv_nsec + ts_delta.tv_nsec);

	/* Update max vt */
	if(timespec_compare(&(p->cf.task_sched_param->bvt_virtual_time), &(bq->max_virtual_time)) > 0) 
		bq->max_virtual_time = p->cf.task_sched_param->bvt_virtual_time;
}
/* update_virtual_times */

/* handle_bvt_timeout: handle bvt timeout for a particular bvt process
 * The goal of the function is to call schedule() and chhose a new
 * process to run when the old bvt process has run out of its fair
 * share.
 */
 
#ifdef CONFIG_HIGH_RES_TIMERS

static enum hrtimer_restart handle_bvt_timeout(struct hrtimer *timer)
{
	struct task_struct *p;

 	/* This jugglery was not required with the original hrtimer
 	 * api.  The callback in this new api only takes the timer
 	 * pointer and no other parameters can be passed. Therefore we
 	 * need to embed this timer in another struct and then use the
 	 * following jugglery to get any member of the parent struct.
 	 */
	struct bvtqueue *bq = container_of(timer,struct bvtqueue,bvt_timer);
 	p = bq->running_bvt_task;

	if (is_coop_realtime(p)) {
		/* there is a case of policing here. We need to
		 * decrement the number of coop tasks and remove this
		 * tasks's node from the coop heap
		 */
		unsigned long flags;
		int dom_id = task_domain(p);
		struct bvtqueue *bq = get_task_bq_locked(p, &flags); 
		
		test_remove_task_from_coop_bvt_queues(p, &(bq->cq[dom_id]));
		do_policing(bq,p);
		bq->bvt_domains[DOM_BEST_EFFORT].num_tasks++;
		/* insert task back into heap */
		insert_task_into_bvt_queue(bq,p); 
		put_task_bq_locked(bq, &flags);

	} /* if */

	bq->bvt_timer_active = 0;
 	set_tsk_need_resched(p); 

 	return HRTIMER_NORESTART;

} /*  handle_bvt_timeout */

#else 
#if 0
/* this is the old shitty jiffy level timer part only kept for those
 * kernels that do not have highres timer support. */
static void handle_bvt_timeout(unsigned long __data)
{
 	struct task_struct *p = (struct task_struct*) __data;

	if (is_coop_realtime(p)) {
		/* there is a case of policing here. We need to
		 * decrement the number of coop tasks and remove this
		 * tasks's node from the coop heap
		 */
		unsigned long flags;
		int dom_id = task_domain(p);
		struct bvtqueue *bq = get_task_bq_locked(p, &flags); 
	
		test_remove_task_from_coop_bvt_queues(p,&(bq->cq[dom_id]));
		do_policing(bq, p);

		bq->bvt_domains[DOM_BEST_EFFORT].num_tasks++;
		/* insert task back into heap */
		insert_task_into_bvt_queue(bq,p); 

		put_task_bq_locked(bq, &flags);

	} /* if */

	bq->bvt_timer_active = 0;
 	set_tsk_need_resched(p); 

} /*  handle_bvt_timeout */
#endif
#endif

/* The following function schedules a dynamic 
 * timer to fire after the 
 * bvt interval expires for the process p. It schedules the bvt task to 
 * preempt after that interval.
 * Must be called with the task bvt queue lock held
 * @bq: The bvt queue for the process p, assumed that the corresponding 
 * runqueue lock is acquired before calling this function.
 * @p: the process for which the timer is scheduled.
 * The timer interval is obtained from the global value 
 * curr_bvt_period
 */
#ifdef CONFIG_HIGH_RES_TIMERS

void schedule_dynamic_bvt_timer(struct bvtqueue *bq, 
				struct task_struct *p)
{
	struct timespec   ts;
	struct timespec deadline;
	//struct timespec ts_now;	
	//ts_now = ns_to_timespec(task_rq(p)->clock);
	
	ts = bq->curr_bvt_period;
	/* now() + ts = deadline */
	set_normalized_timespec(&deadline,ts.tv_sec + bq->ts_now.tv_sec,
				ts.tv_nsec + bq->ts_now.tv_nsec);
	p->cf.coop_t.deadline = deadline;
	
	if (is_coop_realtime(p)) {
		/* add slack value for only coop realtime tasks so that they
		 * do not get policed if they yield within this slack interval
		 */
		set_normalized_timespec(&deadline,deadline.tv_sec + bq->ts_slack.tv_sec,
					deadline.tv_nsec + bq->ts_slack.tv_nsec);
	} /* if */

	/* we initialize the highres timer here 
	 * IMPORTANT: Initializing them here actually ties them
	 * to a specific cpu on which the current code runs.
	 * There is no highres timer interface through which we can
	 * initialize the per cpu timers on different CPUs in SMP
	 * as of today - the writing of this code. I know this is 
	 * ugly :(
	 */
	
	hrtimer_init(&bq->bvt_timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
	bq->bvt_timer.expires  = timespec_to_ktime(deadline);
	bq->bvt_timer.function = handle_bvt_timeout;
	/* Use the irqsafe_no_softirq callback mode*/
	bq->bvt_timer.cb_mode = HRTIMER_CB_IRQSAFE_NO_SOFTIRQ;
	hrtimer_start(&bq->bvt_timer, bq->bvt_timer.expires,HRTIMER_MODE_ABS);

	bq->bvt_timer_active = 1;

} /* schedule_dynamic_bvt_timer */
       
#else /* !CONFIG_BVT_HIGHRES_TIMER: use regular timer wheel */

static void schedule_dynamic_bvt_timer(struct bvtqueue *bq, 
				       struct task_struct *p) {
	struct timespec   ts;
 	unsigned long     expire;
 	unsigned long     njiffies;
	struct timespec   deadline;
	//struct timespec ts_now;	
	//ts_now = ns_to_timespec(task_rq(p)->clock);
	
	ts = bq->curr_bvt_period;
	
	/* now() + ts = deadline */
	set_normalized_timespec(&deadline,ts.tv_sec + bq->ts_now.tv_sec,
				ts.tv_nsec + bq->ts_now.tv_nsec);
	p->cf.coop_t.deadline = deadline;

	if (is_coop_realtime(p)) {
		/* add slack value for only coop realtime tasks so that they
		 * do not get policed if they yield within this slack interval
		 */
		set_normalized_timespec(&ts,ts.tv_sec + bq->ts_slack.tv_sec,
					ts.tv_nsec + bq->ts_slack.tv_nsec);
	} /* if */

	njiffies = timespec_to_jiffies(&ts) + 1;

 	expire = njiffies + jiffies;
 
 	setup_timer(&bq->bvt_timer, &handle_bvt_timeout, (unsigned long) p);
 	__mod_timer(&bq->bvt_timer, expire);
 
	bq->bvt_timer_active = 1;

}/* schedule_dynamic_bvt_timer */

#endif /* CONFIG_BVT_HIGHRES_TIMER */

/* charge_running_times:
 * Update the running time of a task safely 
 * @bq: the pointer to the per cpu bvt structure in the 
 * runqueue
 * @prev: The pointer to the task struct whose time we
 * want to update.
 */
static inline void charge_running_times(struct bvtqueue *bq, 
					struct task_struct *prev)
{
	int was_in_heap;

	was_in_heap = (int)prev->cf.task_sched_param->bheap_ptr;
	
	if (was_in_heap) {
		heap_delete(bq->bvt_heap, prev->cf.task_sched_param->bheap_ptr);
		prev->cf.task_sched_param->bheap_ptr = NULL; 
	}
	
	update_virtual_times(prev);
	
	if(was_in_heap) {
		insert_task_into_bvt_queue(bq,prev); 
	}
}
/* charge_running_times */

/* Called with runQ lock held */
inline void tv_fairshare_now_adjusted(struct timeval *tv)
{
	struct timespec ts_now_adjusted;
	//struct timespec ts_now;
	//ts_now = ns_to_timespec(task_rq(current)->clock);
	set_normalized_timespec(&ts_now_adjusted,
	       cpu_bq(smp_processor_id())->ts_now.tv_sec  - wall_to_monotonic.tv_sec,
	       cpu_bq(smp_processor_id())->ts_now.tv_nsec - wall_to_monotonic.tv_nsec);
	
	tv->tv_sec  = ts_now_adjusted.tv_sec;
	tv->tv_usec = ts_now_adjusted.tv_nsec / NSEC_PER_USEC;
}

/* choose_next_bvt:
 * Picks the next most eligible guy to run, and also updates upcoming deadline
 * Upcoming deadline is used to calculate the timeslice
 */
static struct task_struct* __sched choose_next_bvt(struct bvtqueue *bq) 
{
	struct fairshare_sched_param *new_node = NULL;
	struct task_struct *next_coop_task     = NULL;
	struct task_struct *next_overall_coop_task = NULL;
	struct task_struct *next_earliest_deadline_task = NULL;
	struct timeval tv_now;
	struct timespec *next_virtual_time, *next_coop_virtual_time, *min_virt_time;
	struct timespec ts_diff;
	struct timespec ts_zero;
	struct timespec ts_fudge;
	struct task_struct *next = NULL;
	set_normalized_timespec(&ts_zero, 0 , 0);

	if (likely(!heap_is_empty(bq->bvt_heap)))
		new_node = (struct fairshare_sched_param*) heap_min_data(bq->bvt_heap);
	else return NULL;
		
	if (new_node->dom_type == DOM_BEST_EFFORT) 
	{
		next = get_task(new_node);
	}else {
		
		/* our coop algorithm runs on top of best effort. So we need to 
		 * override the fairshare heuristics with our coop heuristics here
		 * We play a trick here, since there are basically one bvt domain 
		 * and multiple coop domains, if the node is not a bvt node, 
		 * it must be one of the coop nodes and the dom_type is actually
		 * represents the dom_id. 
		 */
		choose_next_coop(&next_coop_task, new_node->dom_type);
		
		if (next_coop_task) 
		{
			next = next_coop_task;
		} else {
			printk(KERN_ERR "oops, this was not anticipated!\n");
			BUG();
		} /* if */
	} /* if */


	/* Store the actual value for the min_virt_time for the system, before incorporating slack*/
	
	min_virt_time = &(next->cf.task_sched_param->bvt_virtual_time);
	find_nearest_global_deadline(&next_earliest_deadline_task);
	set_normalized_timespec(&ts_fudge,0,bvt_sched_granularity*NSEC_PER_USEC);

		    
	/* Returns the wall time */
	tv_fairshare_now_adjusted(&tv_now);

	/* If the earliest deadline task has an expired deadline, choose it else choose the highest priority asap task 
 	 * The next_overall_coop_task is 'the' most eligible guy to run in the sytem now, in terms of timeliness
	 */
	
	if (next_earliest_deadline_task)
	{
	/* Check if this deadline is expired or not */
	if(timeval_compare(&(next_earliest_deadline_task->cf.coop_t.dead_p.t_deadline), 
			   &tv_now) < 0) 
		{
		next_overall_coop_task = next_earliest_deadline_task;	
		}
	else    {
		next_overall_coop_task = NULL;
		}
	}
	else next_overall_coop_task = NULL;

	/* Fudging Logic*/
	if (next_overall_coop_task && ((next) != next_overall_coop_task)) {
		/* if the deadline expired, adjust "next" provided the
		 * virtual times are "epsilon" distance apart where
		 * epsilon is COOP_DEAD_SLACK. Its also a measure of
		 * how much unfairness we would like to introduce in
		 * our system.
		 */ 
		next_coop_virtual_time = &next_overall_coop_task->cf.task_sched_param->bvt_virtual_time;
		next_virtual_time = &((next)->cf.task_sched_param->bvt_virtual_time);

		if (timespec_compare(next_coop_virtual_time, next_virtual_time) == 0 ) {
			/* Fudge in this case, if ts_fudge is greater than zero */
			if (timespec_compare(&ts_fudge, &ts_zero) > 0 ) {
				next = next_overall_coop_task;
			
			}
		}
		else if (timespec_compare(next_coop_virtual_time, next_virtual_time) > 0) {
			ts_diff = timespec_sub(*next_coop_virtual_time, *next_virtual_time);
			
			/* Fudge if diff lesser than ts_fudge */
			if (timespec_compare(&ts_diff, &ts_fudge) <= 0) {
				bq->fudge++;
				next = next_overall_coop_task;
				bq->fudged_flag = true;
				} else {
					/* Don't fudge*/
					bq->nofudge++;
				} /* else */
			
			bq->count++;	
			
		
		} /* if */


	} /* if */


	/* Sanity Checks */
	if (timespec_compare(&bq->max_virtual_time, min_virt_time) < 0) {
		printk(KERN_ERR "All hell hath break loose, max virt time less than min\n");
		BUG();
	}

	if ( ((next)->cf.task_sched_param->bvt_virtual_time.tv_sec < 0) || ((next)->cf.task_sched_param->bvt_virtual_time.tv_nsec < 0) ) {
		printk(KERN_ERR "Virt time overflowed\n");
		BUG();
	}
	
	return next;
}
/* choose_next_bvt */

/* update_bvt_prev: This function does bvt updates on the bvt process
 * we are context switching from @bq: the pointer to the per cpu bvt
 * structure in the runqueue @prev: the pointer to the task_struct of
 * the process we want to context switch from.
 */
static void __sched update_bvt_prev(struct rq *rq, struct task_struct *prev) 
{
	struct bvtqueue *bq = &rq->bq;

	/* we need to update our scheduler clock. 
	 * This is the only place where we update the global 
	 * ts_now value used by all our time accounting functions
	 * Thanks to Buck for suggesting this. 
	 */
	fairshare_now(&bq->ts_now);

	if(!is_bvt(prev)) 
		return;
	/* Forgoe accounting, incase timer still running */
	if(bq->bvt_timer_active)
		return;
	charge_running_times(bq,prev);


}
/* update_bvt_prev */



/* prepare_bvt_context_switch:
 * handle context switch to a bvt task 
 * This is called from within schedule()
 * Be careful: this function is called from within 
 * a critical section and thus irq's and preemptions are
 * disabled. 
 * It is extremely important to optimize the code path
 * in this function. 
 * Further, the corresponding runqueue for bq must be locked 
 * before calling this function. 
 * Assumption: next is in the runqueue  
 * of which bq is a member. This assumption is 
 * true when the function is called from schedule() 
 * @return 0: Don't arm timeout timer
 *         1: Arm timeout timer 
 */

static struct task_struct* __sched pick_next_task_arm_timer(struct rq *rq)
{
	struct bvtqueue *bq = &rq->bq;
	struct task_struct *next = NULL;

	/* timer continue with existing task */
	if (is_bvt(rq->curr) && bq->bvt_timer_active){ 
		return rq->curr;	
	} /* if */

	if (unlikely(heap_is_empty(bq->bvt_heap))) {
		return NULL;
	}
	
	next = choose_next_bvt(bq);
	if (NULL == next)
		return NULL;

	bq->running_bvt_task = next;
    /* BUG FIX: Remove this guy's entry from the timeout heap, since he is about to run*/
	if(is_coop_realtime(next)) {
		remove_task_from_coop_queue(next,
 								&(bq->cq[task_domain(next)]),0);
	}
	//next->cf.bvt_t.bvt_timeslice_start = ns_to_timespec(rq->clock);
	next->cf.bvt_t.bvt_timeslice_start = bq->ts_now;
	calculate_bvt_period(next);

	/* Arm the timer now */
	schedule_dynamic_bvt_timer(&rq->bq,next);

	if (bvt_sched_tracing == 2) {
		printk("Pid = %u, Utime = %u Virtual time = %u.%u, ,Actual time = %u.%u, Timeslice = %u.%u\n",next->pid,next->utime,
						next->cf.task_sched_param->bvt_virtual_time.tv_sec,next->cf.task_sched_param->bvt_virtual_time.tv_nsec,
						next->cf.bvt_t.private_sched_param.bvt_actual_time.tv_sec, 
						next->cf.bvt_t.private_sched_param.bvt_actual_time.tv_nsec,
						bq->curr_bvt_period.tv_sec, bq->curr_bvt_period.tv_nsec);	
	}
	else if (bvt_sched_tracing == 5) {
		printk("%llu %llu %llu\n",timespec_to_ns(&bq->ts_now), rq->clock, sched_clock());
	}
	
	/* Update exec_start, to enable cfs process accounting */
	next->se.exec_start = rq->clock;
	return next;

}
/* prepare_bvt_context_switch*/

void inline remove_task_from_bvt_queue(struct bvtqueue *bq, 
				       struct task_struct *p)
{
	if (p->cf.task_sched_param->bheap_ptr) {
		heap_delete(bq->bvt_heap,p->cf.task_sched_param->bheap_ptr);
		p->cf.task_sched_param->bheap_ptr = NULL;
	}
}
/* remove_task_from_bvt_queue*/

/* set_tsk_as_besteffort: marks a task as a best effort task only
 * called from one place: __do_set_bvt(); @tsk: the task_struct for
 * the task
 */
void init_bvt_domain(struct bvtqueue *bq, 
				  struct task_struct* tsk)
{
	/* multiple calls to this function should not 
	 * mess with our accounting logic 
	 */
	if (tsk->cf.bvt_dom == &(bq->bvt_domains[DOM_BEST_EFFORT]))
		return;

	/* register this task as belonging to the 
	 * best effort domain 
	 */
	tsk->cf.bvt_dom = &(bq->bvt_domains[DOM_BEST_EFFORT]);
	tsk->cf.dom_id  = DOM_BEST_EFFORT;

	/* task virtual time is going to be the virtual time of 
	 * the individual task 
	 */
	tsk->cf.task_sched_param = &tsk->cf.bvt_t.private_sched_param;
}
/* set_tsk_as_besteffort */

/* do_policing: This is the main policing function.  Important: It
 * only goes one way, i.e., demotes a real coop task to a best effort
 * task. So this function is only called for a real coop task and not
 * a best effort task. The assertion at the very beginning is for the
 * same purpose.
 */
void do_policing(struct bvtqueue *bq, struct task_struct *tsk)
{
	g_assert(tsk);
	g_assert(bq);
	g_assert(is_coop_realtime(tsk)); 

	if(bvt_sched_tracing == 1)
		printk(KERN_INFO "Task %d is being policed\n",tsk->pid);
	
	/* register this task as belonging to the 
	 * best effort domain 
	 */
	tsk->cf.bvt_dom = &(bq->bvt_domains[DOM_BEST_EFFORT]);
	tsk->cf.dom_id  = DOM_BEST_EFFORT;
	
	/* task virtual time is going to be the virtual time of the
	 * individual task,update pvt vt to be the domain's vt at this point  
	 */
	tsk->cf.bvt_t.private_sched_param.bvt_virtual_time = tsk->cf.task_sched_param->bvt_virtual_time;
	tsk->cf.task_sched_param = &tsk->cf.bvt_t.private_sched_param;
	
	#if defined(CONFIG_SMP)
	/* Unpin the task*/
	set_cpus_allowed(tsk,cpu_online_map);
	#endif
	clear_coop_task(tsk);

} /* do_policing */

/* bvt_timeout_gt: The comparison function that compares two nodes in
 * the bvt heap.  The first comparison parameter is the virtual
 * time. The node having the smaller virtual time gets to run next.
 * If the two nodes have the same virtual time, then I use a second
 * key to sort the nodes, their preempt time.
 */

static gboolean bvt_timeout_gt(heap_key_t a, heap_key_t b)
{
	struct fairshare_sched_param *node1 = a;
	struct fairshare_sched_param *node2 = b;
	struct timespec vt1,vt2;

	vt1 = (node1->bvt_virtual_time);
	vt2 = (node2->bvt_virtual_time);

	if (!timespec_compare(&vt1, &vt2)) {
		
		/* FIFO ordering, LIFO ordering results in a live deadlock with ping ponging tasks */
		return (timespec_compare(&node1->insertion_ts, 
				    &node2->insertion_ts)>0);
	}
	else
	{
		return(timespec_compare(&vt1,&vt2) > 0);
	}
}
/* bvt_timeout_gt */

/* initializes the global bvt parameters */

void __init bvt_global_init()
{

	struct bvtqueue *bq;
	unsigned long flags;
	int cpu,j;
	heap_t *temp1;
	heap_t *temp2;
	heap_t *temp3;

	bvt_sched_granularity = (suseconds_t) CONFIG_BVTPRD;
	bvt_sched_tracing = 0; /* Off by default */
	ts_bvt_min_timeslice = ns_to_timespec(BVT_MIN_TIMESLICE * NSEC_PER_USEC);

	for_each_possible_cpu(cpu) {

		/* BUG FIX: create heap might sleep, do alloc outside of lock
		 * region (Cannot sleep while holding a lock) */
		temp1 = create_heap(bvt_timeout_gt, BVT_HEAP_INIT_CAP);
		temp2 = create_heap(coop_poll_timeout_gt, BVT_HEAP_INIT_CAP);
		temp3 = create_heap(coop_poll_timeout_gt, BVT_HEAP_INIT_CAP);	
		bq = get_cpu_bq_locked(cpu, &flags);
		/* XXX All these heap sizes should be fixed dynamically 
		 * in the future, depending on total memory available */
		bq->bvt_heap  =	temp1;
		bq->global_coop_deadline_heap = temp2;
		bq->global_coop_sleep_heap = temp3;

		if (unlikely(!temp1 || !temp2 || !temp3)) {  
			/* memory allocation error */
			bvt_print_debug("unable to allocate memory for the bvt heap.");
			panic("unable to allocate memory for the bvt heap");
			
		}else  
			printk(KERN_INFO "[BVT] allocated memory for bvt heap\n");

		bq->running_bvt_task = NULL;
		/* initializing per cpu domain parameters */

		for(j=0;j<DOM_MAX_TYPE; j++) {
			bq->bvt_domains[j].dom_type  = j;
			bq->bvt_domains[j].num_tasks = 0;
			memset(&bq->bvt_domains[j].dom_sched_param.bvt_virtual_time,0,sizeof(struct timespec));
			memset(&bq->bvt_domains[j].dom_sched_param.bvt_actual_time,0,sizeof(struct timespec));
			memset(&bq->bvt_domains[j].dom_sched_param.insertion_ts,0,sizeof(struct timespec));;
			set_normalized_timespec(&bq->bvt_domains[j].dom_sched_param.fudge, 0, 0);
			bq->bvt_domains[j].dom_sched_param.bheap_ptr       = NULL;
			bq->bvt_domains[j].dom_sched_param.dom_type        = j;
		}

		memset(&(bq->last_coop_deadline),0,sizeof(struct timeval));
		memset(&(bq->max_virtual_time),0,sizeof(struct timespec));
		memset(&(bq->tot_time),0,sizeof(struct timespec));
		bq->isTargetSet = -1;
		bq->fudged_flag = false;
		/*set_normalized_timespec(&ts_fudge,0,bvt_sched_granularity);*/
		set_normalized_timespec(&bq->ts_slack,0,COOP_DEAD_SLACK);
		bq->count = 0;
		bq->adj=0;
		bq->noadj=0;
		bq->fudge=0;
		bq->nofudge=0;

		bq->bvtLogCount = 0;
		
		/* we do not initialize the highres timers here.
		 * the reason for doing this is that in smp case
		 * initializing them
		 * here would tie them to a specific cpu on which this
		 * code runs. We want timers per cpu basis and want to tie them
		 * to their corresponding cpu.
		 * Unfortunately, the highres api does not allow us
		 * to specify cpu index when initializing :(
		 */
		put_cpu_bq_locked(cpu,&flags);
	} /* for */
	
	return;
}
/* bvt_global_init */


/* bvt_proc_init:
 * initializes the process specific data structures for
 * the bvt kernel process scheduler 
 */

inline void bvt_proc_init(struct task_struct *p)
{
	memset(&p->cf.bvt_t,0, sizeof(struct bvt_struct));
	memset(&p->cf.bvt_t.private_sched_param,
	       0, 
	       sizeof(struct fairshare_sched_param));
	p->cf.bvt_t.private_sched_param.dom_type = DOM_BEST_EFFORT;

	p->cf.bvt_dom                             = NULL;

	p->cf.bvt_t.me = p;
	p->cf.dom_id = -1;

}
/* bvt_proc_init*/

static int show_bvtstat(struct seq_file *seq, void *v) {
	int cpu;
	seq_printf(seq, "timestamp %lu\n", jiffies);
	seq_printf(seq, "global bvt period = %ld000 nsec\n", (long) bvt_sched_granularity);
	
	for_each_online_cpu(cpu) {
		struct bvtqueue *bq;
		bq = cpu_bq(cpu);
		seq_printf(seq, "current bvt period (cpu %d) = %ld nsec\n", 
			   cpu,(long) timespec_to_ns(&bq->curr_bvt_period));
		seq_printf(seq, "adjustments: %ld\n", bq->adj);
		seq_printf(seq, "no adjustments: %ld\n", bq->noadj);
		seq_printf(seq, "fudge: %ld\n", bq->fudge);
		seq_printf(seq, "nofudge: %ld\n", bq->nofudge);
		seq_printf(seq, "Current Fudge in nsecs = %ld000\n", bvt_sched_granularity);
	}

	seq_printf(seq, "minimum bvt period = %ld usec\n", (long) BVT_MIN_TIMESLICE);
	return 0;
}

void detach_coop_fairshare_sched(struct task_struct* tsk) 
{
	if (!is_bvt(tsk)) return;
}

static int bvtstat_open(struct inode *inode, struct file *file)
{
	unsigned int size = PAGE_SIZE * (1 + num_online_cpus() / 32);
	
	char *buf;

	struct seq_file *m;
	int res;

        /* don't ask for more than the kmalloc() max size, currently 128 KB */
	if (size > 128 * 1024)
		size = 128 * 1024;

	buf = kmalloc(size, GFP_KERNEL);
	
	if (!buf)
		return -ENOMEM;
	res = single_open(file, show_bvtstat, NULL);
	if (!res) {
		m = file->private_data;
		m->buf = buf;
		m->size = size;
	} else
		kfree(buf);
	return res;
} /* bvtstat_open */

struct file_operations proc_bvtstat_operations = {
	.open    = bvtstat_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

static void task_new_faircoop(struct rq *rq, struct task_struct *p)
{
bvt_borrow(rq,p,0);
}

static void dequeue_task_faircoop(struct rq *rq, struct task_struct *p,int sleep)
{

	struct bvtqueue *bq = &rq->bq;
    
	if(!is_bvt(p)) return;
    bq->running_bvt_task = NULL;
    
    bvt_timer_cancel(&bq->bvt_timer);
    bq->bvt_timer_active = 0; 
    
    if (is_coop_realtime(p))
    {    
        int dom_id = p->cf.dom_id;
        /* note that policing has not yet taken place, so no
         * matter whether we are cooperatively sleeping or
         * not, we remove our nodes from coop heap because the task 
         * is getting deactivated.
         */
        test_remove_task_from_coop_bvt_queues(p,&(bq->cq[dom_id]));
    	/* Only check for policing, if the task is going to sleep */ 
        if (!p->exit_state && sleep) {
            if(!p->cf.coop_t.is_well_behaved) {
                do_policing(bq,p);
            }
            else {
				insert_task_into_sleep_queue(NULL,&(bq->cq[dom_id]),p,0);
            } /* else */
        } /* !p->exit_state */
    } else  {
        remove_task_from_bvt_queue(bq,p);
        p->cf.bvt_dom->num_tasks--;
    } /* else */

	p->se.on_rq = 0;
}

#ifdef CONFIG_SMP
static unsigned long
load_balance_faircoop(struct rq *this_rq, int this_cpu, struct rq *busiest,
		  unsigned long max_load_move,
		  struct sched_domain *sd, enum cpu_idle_type idle,
		  int *all_pinned, int *this_best_prio)
{
	return 0;
}

static int
move_one_task_faircoop(struct rq *this_rq, int this_cpu, struct rq *busiest,
		   struct sched_domain *sd, enum cpu_idle_type idle)
{
	return 0;
}

static void select_task_rq_faircoop(struct task_struct *p, int sync)
{
}

static void join_domain_faircoop(struct rq *rq)
{
}

static void leave_domain_faircoop(struct rq* rq)
{
}
#endif

static void task_tick_faircoop(struct rq *rq, struct task_struct *p, int queued)
{
}

static void set_curr_task_faircoop(struct rq *rq)
{
	fairshare_now(&current->cf.bvt_t.bvt_timeslice_start);
	current->se.exec_start = rq->clock;
	//current->cf.bvt_t.bvt_timeslice_start = ns_to_timespec(sched_clock());
	set_tsk_need_resched(current);
}

static void yield_task_faircoop(struct rq *rq)
{
/* Dequeue, and then enqueue the task */
struct task_struct* temp;
temp = rq->curr;
dequeue_task_faircoop(rq,temp,0);
bvt_borrow(rq,temp,0);
}

static void check_preempt_faircoop(struct rq* rq, struct task_struct *p)
{
}

static void prio_changed_faircoop(struct rq* this_rq, struct task_struct *task, int oldprio, int running)
{
}


static void switched_from_faircoop(struct rq *this_rq, struct task_struct *task, int running)
{
}

static void switched_to_faircoop(struct rq *this_rq, struct task_struct *task, int running)
{
}


#endif


