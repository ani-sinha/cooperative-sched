 /* The coop_poll() system call interface for cooparative
 * soft real time user processes.
 * This file contains the coop_poll code that implements policing
 * as a part of the combined coop_poll, fairshare heuristics.
 * The system call number is 285
 * The original entry in #285 was sys_ni_syscall which is
 * a placeholder for non-implemented system calls.
 *  
 * Coded by Anirban Sinha, anirbans@cs.ubc.ca
 * Other contributors: Charles Krasic, Ashvin Goel 
 * krasic@cs.ubc.ca, ashvin@eecg.toronto.edu
 *
 * May 28, 2007: New non-sleeping version
 * introduced. It is hoped that this version of coop_poll() will be 
 * more conducive towards the policing mechanism through fairshare scheduling.
 *
 * TODO: integrate coop_poll() with kernel side of epoll().
 *
 * A Note about timekeeping: 
 * kernel uses all different forms and notions about what monotonic "now" is.
 * Please check kernel/bvt_schedule.c: fairshare_now() function 
 * for explanations
 * However, our user level coop processes uses do_gettimeofday()
 * system call to register their "now" value. This is actually the wall 
 * clock time. Thus, all coop related 
 * time comparisons (and while reporting time to userspace) 
 * uses do_gettimeofday() call to get the value of 
 * "now". Unfortunately, this value is not the same as the monotonic clock
 * value (see wall_to_monotonic offset timespec) that is used by the 
 * timer code (highres timers or otherwise). 
 * Hence, we are forced to use two different notions of time, 
 * do_gettimeofday() and fairshare_now(). The former is used to report 
 * and compare time values in the coop heap. The latter is used by the 
 * fairshare timer scheduling code.  
 */

#if defined(CONFIG_SCHED_COOPREALTIME)

#include <linux/heap.h> 
#include <linux/preempt.h>
#include <linux/mutex.h>
#include <linux/kernel.h>
#include <linux/jiffies.h>
#include <asm/uaccess.h>
#include <linux/cpumask.h>
#include <linux/sched.h>
#include <linux/coop_fairshare_sched.h>

#include <linux/seq_file.h>
#include <linux/fs.h>

static unsigned long total_deadlines = 0;
static unsigned long earlier_deadlines = 0;

#define __FUNC__ __func__
#define  HEAP_INIT_CAP 1024 /* initial capacity for the coop poll heaps */

gboolean coop_poll_asap_gt(heap_key_t a, heap_key_t b);
static inline void find_nearest_deadline(coop_queue *cq, struct task_struct **w_dead);

#if defined(CONFIG_PREEMPT_RT) || defined(CONFIG_SMP)
static 	__u32 curr_cpu;
#endif

inline coop_queue* cpu_cq(int cpu, int dom_id) 
{
	struct bvtqueue *bq = cpu_bq(cpu);
	coop_queue *cq = &(bq->cq[dom_id]);
	return cq;
}

/* task_cq_lock - lock a given coop_queue and return 
 * a reference to the structure..
 * Locks the corresponding runqueue of which this coop_queue
 * is a member.
 */
static inline coop_queue *task_cq_lock(struct task_struct *tsk,
				       int dom_id,
				       unsigned long *flags,
				       struct bvtqueue **bq) 
{
	coop_queue *cq;
	*bq = get_task_bq_locked(tsk,flags);
	cq = &((*bq)->cq[dom_id]);
	return cq;
}

/* cq_unlock:
 * unlock the coop queue and enable preemption
 * where applicable 
 */
static inline void cq_unlock(struct bvtqueue *bq, 
			     unsigned long *flags)
{
	put_task_bq_locked(bq, flags);
}
#define COOP_DEBUG "[COOP_POLL]"

#define coop_print_debug(fmt,arg...) do { \
		print_debug(COOP_DEBUG,fmt,##arg);   \
	}while(0);

#if 0
inline int is_coop_realtime(struct task_struct* p) {

	if (!p->cf.bvt_dom) return 0;
	
	return ((p->cf.dom_id >= DOM_REALTIME_COOP0) && 
		(p->cf.dom_id <= DOM_REALTIME_COOP14));
}
#endif

static int show_coopstat(struct seq_file *seq, void *v)
{
	int cpu, dom_id;
	coop_queue *cq;
	struct task_struct *node;
	
	seq_printf(seq,"timestamp %lu\n",jiffies);
	seq_printf(seq,"total deadlines\t%lu\n",total_deadlines);
	seq_printf(seq,"earlier deadlines\t%lu\n",earlier_deadlines);
	seq_printf(seq,"cpu# \tcoop_poll#\tyields#\n");
	node = NULL;
	/* XXX Can you call this without getting the runQ lock ?? The coop queues might be in an inconsistent state !!*/
	//find_nearest_global_deadline(&node);
	//if(node != NULL)
	//	seq_printf(seq,"Global earliest deadline %d, %u,%u\n",node->pid,node->cf.coop_t.dead_p.t_deadline.tv_sec,node->cf.coop_t.dead_p.t_deadline.tv_usec);
	for_each_online_cpu(cpu) {
		for_each_available_coop_domain(dom_id) {
			cq = cpu_cq(cpu,dom_id);
			/* runqueue-specific stats */
			seq_printf(seq,
				   "cpu%d:\t%lu\t\t%lu",cpu, cq->num_coop_calls, cq->num_yields);
			seq_printf(seq, "\n");
			node = NULL;
			/* XXX Can you call this without getting the runQ lock ?? The coop queues might be in an inconsistent state !!*/
			//find_nearest_deadline(cq, &node);
			//if(node != NULL) 
			//	seq_printf("Domain id = %d, pid = %d, Deadline = %u.%u\n",dom_id,node->pid,node->cf.coop_t.dead_p.t_deadline.tv_sec,node->cf.coop_t.dead_p.t_deadline.tv_usec);
		}
	}

	return 0;
}

static int coopstat_open(struct inode *inode, struct file *file)
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
	res = single_open(file, show_coopstat, NULL);
	if (!res) {
		m = file->private_data;
		m->buf = buf;
		m->size = size;
	} else
		kfree(buf);
	return res;
}

struct file_operations proc_coopstat_operations = {
	.open    = coopstat_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

# define coopstat_inc(cq, field)	do { (cq)->field++; } while (0)
# define coopstat_add(cq, field, amt)	do { (cq)->field += (amt); } while (0)

static void set_tsk_asap_info(struct task_struct *p, 
			      struct timeval *t_asap_deadline, 
			      int asap_prio,
			      struct timespec rank)
{
	p->cf.coop_t.asap_p.priority = asap_prio;
	p->cf.coop_t.asap_p.t_asap   = *t_asap_deadline;
	p->cf.coop_t.asap_p.rank     = rank;
}

static void set_tsk_deadline_info(struct task_struct *p, 
				  struct timeval *t_deadline, 
				  struct timespec rank)
{
	p->cf.coop_t.dead_p.rank       = rank;
	p->cf.coop_t.dead_p.t_deadline = *t_deadline;
}

/* Lock: Expects run Q lock to be held */
static long __insert_into_asap_heap(coop_queue *cq,struct task_struct *p) 
{
	p->cf.coop_t.coop_asap_heap_node = heap_insertt(cq->heap_asap, p, p);

	if(task_domain(p) == DOM_BEST_EFFORT)
		printk(KERN_ERR "Tsk %d, incorrectly inserting into asap_heap_node , cooprealtime flag = %d\n",p->pid,is_coop_realtime(p));

	if(unlikely (!p->cf.coop_t.coop_asap_heap_node)) {
		coop_print_debug("%s, %s:%d: Unable to allocate new memory for the new asap heap node!",
			__FILE__, __FUNC__, __LINE__);
		return -EFAULT;
	} else return 0;
} /* __insert_into_asap_heap */

/* Expects run Q lock to be held */
static long __insert_into_timeout_heap(coop_queue *cq,struct task_struct *p) 
{
	
	p->cf.coop_t.coop_deadline_heap_node = heap_insertt(cq->heap_deadline, p, p);
	if(task_domain(p) == DOM_BEST_EFFORT || !is_coop_realtime(p))
		printk(KERN_ERR "Tsk %d, incorrectly inserting into deadline_heap_node , cooprealtime flag = %d\n",p->pid,is_coop_realtime(p));

	/* Insert onto the global deadline heap */
	p->cf.coop_t.coop_deadline_global_heap_node = heap_insertt((get_task_bq(p)->global_coop_deadline_heap),p,p);

	if(unlikely ( (!p->cf.coop_t.coop_deadline_heap_node) || (!p->cf.coop_t.coop_deadline_global_heap_node))) {
		coop_print_debug("%s, %s:%d: Unable to allocate new memory for the new deadline heap node!",
				 __FILE__, __FUNC__, __LINE__);
		return -EFAULT;
	} else return 0;
	
} /*  __insert_into_timeout_heap */

static long __insert_into_sleep_heap(coop_queue *cq,struct task_struct *p) 
{
	p->cf.coop_t.coop_sleep_deadline_node = heap_insertt(cq->heap_coop_sleep, p, p);
	/* Insert onto the global sleep heap */
	p->cf.coop_t.coop_sleep_deadline_global_heap_node = heap_insertt((get_task_bq(p)->global_coop_sleep_heap),p,p);
	
	if(unlikely ( (!p->cf.coop_t.coop_sleep_deadline_node) || (!p->cf.coop_t.coop_sleep_deadline_global_heap_node)  ))  {
		coop_print_debug("%s, %s:%d: Unable to allocate new memory for the new deadline_sleep heap node!",

				 __FILE__, __FUNC__, __LINE__);
		return -EFAULT;
	} else return 0;
	
} /* __insert_into_coop_sleep_heap */

/* insert_task_into_asap_queue - insert any task into 
 * specified cpu coop asap queue.
 * @t_asap_deadline: the deadline of the asap event
 * @asap_prio: the priority of the asap event
 * cq: the specified coop queue of any processor
 * p: the specified task struct of the process to insert.
 * Note: coop queue lock must be acquired before calling this function  
 */

static long insert_task_into_asap_queue(struct timeval *t_asap_deadline, 
				 int asap_prio, coop_queue *cq, struct task_struct *p)
{
	struct timespec rank;

	g_assert(p);
	g_assert(t_asap_deadline);
	getnstimeofday(&rank);
	
	set_tsk_asap_info(p, t_asap_deadline, asap_prio,rank);

	return  __insert_into_asap_heap(cq,p);
}
/* insert_task_into_asap_queue */

/* insert any task into any specified coop timeout queue
 * @t_deadline: the deadline of the timeout event
 * @cq: the coop queue of any processor
 * @p: the pointer to the task struct of any process
 * This function must be called with coop_queue lock held
 */
long insert_task_into_timeout_queue(struct timeval *t_deadline, 
				    coop_queue *cq, struct task_struct *p,int fil_dead)
{
	struct timespec rank;

	printk("Task %d cr flag %d dom %d timeout q insertion\n",p->pid,is_coop_realtime(p),task_domain(p));

	g_assert(p);

	if(fil_dead) {
		g_assert(t_deadline);
		getnstimeofday(&rank);
		set_tsk_deadline_info(p, t_deadline,rank);
	}
	return __insert_into_timeout_heap(cq,p);
}
/* insert_task_into_timeout_queue*/

long insert_task_into_sleep_queue(struct timeval *t_deadline, 
				    coop_queue *cq, struct task_struct *p,int fil_dead)
{
	struct timespec rank;

	g_assert(p);

	if(fil_dead) {
		g_assert(t_deadline);
		getnstimeofday(&rank);
		set_tsk_deadline_info(p, t_deadline,rank);
	}
	return __insert_into_sleep_heap(cq,p);
}
/* insert_task_into_timeout_queue*/

/* get_user_arg: 
 * copies the data from user space to the kernel space 
 * Note: copy_from_user actually reruns the number of bytes that was
 * unable to be copied and 0 otherwise
 * hence, in case it retuns a non-zero value, we simply return error
 * @ki_param: input deadline parameters in kernel space
 * @ui_param: input deadline parameters in user space
 */

static int get_user_arg(struct coop_param_t *ki_param, 
			       struct coop_param_t __user *ui_param)
{
	memset(ki_param, 0,sizeof(struct coop_param_t) );
	return copy_from_user(ki_param, ui_param, sizeof(struct coop_param_t))? -EFAULT:0;
}
/* get_user_arg */


/* set_tsk_as_coop:
 * marks a task as a coop task
 * @tsk: the task_struct for the task 
 */

static void set_tsk_as_coop(struct task_struct* tsk, int dom_id)
{
	struct bvtqueue *bq;
	
	/* multiple calls to this function should not mess with our
	 * accounting logic
	 */

	if (is_coop_realtime(tsk)) {
		return;
	}

#if defined(CONFIG_SMP)
	cpumask_t mask;
	cpu_set(task_cpu(tsk), mask);

	/* cpu migration for coop tasks is not allowed */
	sched_setaffinity(tsk->pid,mask);
#endif
	bq = cpu_bq(task_cpu(tsk));

	/* remove the task from the bvt heap */

	if (tsk->cf.task_sched_param->bheap_ptr) {
		heap_delete(bq->bvt_heap,tsk->cf.task_sched_param->bheap_ptr);
		tsk->cf.task_sched_param->bheap_ptr = NULL;
	}

	set_coop_task(tsk);

    /* Now set the task's domain as the real time domain */
	tsk->cf.bvt_dom = &(bq->bvt_domains[dom_id]);
	tsk->cf.dom_id  = dom_id;
        
     /* the task virtual time is now the same as the domain virtual
	 * time
	 */
	tsk->cf.task_sched_param = &bq->bvt_domains[dom_id].dom_sched_param;

	/* increment the number of tasks in the real time domain if
	 * the task is runnnable.
	 */
	if (!tsk->state) {
		
		bq->bvt_domains[DOM_BEST_EFFORT].num_tasks--;
		bq->bvt_domains[dom_id].num_tasks++;
		
		/* if there is one coop guy running and this guy is
		 * runnable, borrow and insert the real coop node into
		 * the heap
		 */
		if (bq->bvt_domains[dom_id].num_tasks == 1)
		{
			/* Set domain's vt to be equal to this guy's */
			bq->bvt_domains[dom_id].dom_sched_param.bvt_virtual_time = tsk->cf.bvt_t.private_sched_param.bvt_virtual_time;
			insert_task_into_bvt_queue(bq,tsk);
		} /* if */
	} /* if */
}
/* set_tsk_as_coop */

void set_tsk_as_temp_coop(struct task_struct* tsk)
{
	struct bvtqueue *bq;
	
	/* multiple calls to this function should not mess with our
	 * accounting logic
	 */

	if (is_coop_realtime(tsk)) {
		return;
	}

#if defined(CONFIG_SMP)
	cpumask_t mask;
	cpu_set(task_cpu(tsk), mask);
	/* cpu migration for coop tasks is not allowed */
	sched_setaffinity(tsk->pid,mask);
#endif
	bq = cpu_bq(task_cpu(tsk));
	set_coop_task(tsk);

    /* Now set the task's domain as the real time domain */
	tsk->cf.bvt_dom = &(bq->bvt_domains[DOM_REALTIME_TEMP]);
	tsk->cf.dom_id  = DOM_REALTIME_TEMP;
        
	/* increment the number of tasks in the real time domain if
	 * the task is runnnable.
	 */
	if (!tsk->state) {
		bq->bvt_domains[DOM_BEST_EFFORT].num_tasks--;
		bq->bvt_domains[DOM_REALTIME_TEMP].num_tasks++;
	} /* if */
}

/* remove_task_from_coop_queue: remove task from one or both the coop
 * queues based on the argument flag.  
 * @tsk: The task to remove.  
 * @cq:The queue to remove from 
 * @which_queue: The flag, 
 * 0=> both timeout and asap queues,
 * 1=> timeout queue, 
 * 2=> asap queue, 
 * 3=> coop sleep queue,
 * -1=> do nothing.
 */ 
void remove_task_from_coop_queue(struct task_struct *tsk, 
				 coop_queue *cq, 
				 int which_queue)
{
	
	if (which_queue <0) return;
	
	if ((which_queue == 1) || (which_queue == 0))
	{
		if (tsk->cf.coop_t.coop_deadline_heap_node) {
			printk("Heap del del %d, %d %d\n",tsk->pid,
			is_coop_realtime(tsk),
			task_domain(tsk));

			heap_delete(cq->heap_deadline, 
		    tsk->cf.coop_t.coop_deadline_heap_node);
			heap_delete(get_task_bq(tsk)->global_coop_deadline_heap,
	 	    tsk->cf.coop_t.coop_deadline_global_heap_node);

			tsk->cf.coop_t.coop_deadline_heap_node = NULL;
			tsk->cf.coop_t.coop_deadline_global_heap_node = NULL;
		}
	}
	if ((which_queue == 2) || (which_queue == 0))
	{
		if (tsk->cf.coop_t.coop_asap_heap_node) {
			printk("Heap del asap %d, %d %d\n",tsk->pid,
			is_coop_realtime(tsk),
			task_domain(tsk));
			heap_delete(cq->heap_asap, 
		    tsk->cf.coop_t.coop_asap_heap_node);
			tsk->cf.coop_t.coop_asap_heap_node = NULL;
		}
	}
	if (which_queue == 3)
	{
		if (tsk->cf.coop_t.coop_sleep_deadline_node) {
			printk("Heap del sleep %d, %d %d\n",tsk->pid,
			is_coop_realtime(tsk),
			task_domain(tsk));
			heap_delete(cq->heap_coop_sleep, 
		    tsk->cf.coop_t.coop_sleep_deadline_node);
			heap_delete(get_task_bq(tsk)->global_coop_sleep_heap, 
		    tsk->cf.coop_t.coop_sleep_deadline_global_heap_node);

			tsk->cf.coop_t.coop_sleep_deadline_node = NULL;
			tsk->cf.coop_t.coop_sleep_deadline_global_heap_node = NULL;
		} /* if */
	} /* if */
	

}
/* remove_task_from_coop_queue */

void test_remove_task_from_coop_bvt_queues(struct task_struct *tsk, 
					   coop_queue *cq)
{
	struct bvtqueue *bq = cpu_bq(task_cpu(tsk));

	/* Remove from both asap and timeout Q*/
	remove_task_from_coop_queue(tsk,cq,0);
	bq->bvt_domains[task_domain(tsk)].num_tasks--;
	if(task_domain(tsk) == DOM_REALTIME_TEMP)
		remove_task_from_bvt_queue(bq,tsk);
	else if(bq->bvt_domains[task_domain(tsk)].num_tasks <= 0) 
		remove_task_from_bvt_queue(bq, tsk);
}
/* test_remove_task_from_coop_bvt_queues */

static inline void find_nearest_deadline(coop_queue *cq, struct task_struct **w_dead)
{
	if (!heap_is_empty(cq->heap_deadline)){
		*w_dead = (struct task_struct*) heap_min_data(cq->heap_deadline);
	}
} /* find_nearest_deadline */

static inline void find_nearest_asap(coop_queue *cq, struct task_struct **w_asap)
{
	if (!heap_is_empty(cq->heap_asap)){
		*w_asap = (struct task_struct*) heap_min_data(cq->heap_asap);
	}
} /* find_nearest_asap */


/* Return the nearest runnable global deadline task */
void find_nearest_global_deadline(struct task_struct **overall)
{
	struct bvtqueue* bq = cpu_bq(smp_processor_id());
	if (!heap_is_empty(bq->global_coop_deadline_heap)) {
		*overall = (struct task_struct*) heap_min_data(bq->global_coop_deadline_heap);
	}
	else *overall = NULL;
}

void find_nearest_global_sleep(struct task_struct **overall)
{
	struct bvtqueue* bq = cpu_bq(smp_processor_id());
	if (!heap_is_empty(bq->global_coop_sleep_heap)) {
		*overall = (struct task_struct*) heap_min_data(bq->global_coop_sleep_heap);
	}
	else *overall = NULL;

}

 /* Return: Task with earliest global deadline (considering both runnable and sleeping tasks) or
  * Null if heap is empty */
void find_nearest_global_deadline_overall(struct task_struct **overall)
{
	struct task_struct *temp1 = NULL;
	struct task_struct *temp2 = NULL;

	find_nearest_global_deadline(&temp1);
	find_nearest_global_sleep(&temp2);

	if (temp1 && temp2) {
		if (timeval_compare(&temp1->cf.coop_t.dead_p.t_deadline, 
					        &temp2->cf.coop_t.dead_p.t_deadline) < 0)
			*overall = temp1;
		else
			*overall = temp2;
	}
	else if (temp1) 
		*overall = temp1;
	else if(temp2)
		*overall = temp2;
	else 
		*overall = NULL;
}

void find_second_nearest_global_deadline_overall(struct task_struct **overall)
{
	struct bvtqueue* bq = cpu_bq(smp_processor_id());
	struct task_struct *temp1;
	
	if (!heap_is_empty(bq->global_coop_deadline_heap)) {
	// Remove the top element of the global deadline heap
	temp1 = (struct task_struct*)heap_delete_min(bq->global_coop_deadline_heap);	
	find_nearest_global_deadline_overall(overall);
	// Re-insert top element into the heap
	heap_insertt(bq->global_coop_deadline_heap,temp1,temp1);
	}
	else 
		find_nearest_global_sleep(overall);
	
}

static inline void find_nearest_coop_sleep_deadline(coop_queue *cq, 
						    struct task_struct **w_sleep)
{
	if (!heap_is_empty(cq->heap_coop_sleep)){ 
		/* yes, there are sleeping tasks, take the
		 * earliest deadline of the sleeping tasks */
		*w_sleep = (struct task_struct*) heap_min_data(cq->heap_coop_sleep);
	}
}
/* find_nearest_coop_sleep_deadline */


/* find_nearest_deadline_asap:
 * finds the nearest deadlines and asaps from the coop heap 
 * of the current CPU.
 * Note: does not lock the heaps, use carefully 
 */

static void find_nearest_deadline_asap(coop_queue *cq, 
				       struct task_struct **w_dead, 
				       struct task_struct **w_asap)
{
	find_nearest_asap(cq, w_asap);
	find_nearest_deadline(cq, w_dead);
} /* find_nearest_deadline_asap */


/* find_coop_period:
 * This function finds the coop period for the "next" 
 * task. 
 * @next: The next bvt task that we want to run.
 * @next_coop: The next most important real coop task in 
 * the heap. Output parameter.
 * @coop_prd: The coop period determined. Output parameter. 
 * Returns 1 if the coop period is set. Returns 0 otherwise.
 */
int find_coop_period(struct task_struct *next,
		     struct task_struct **next_coop,
		     struct timespec    *coop_prd)
{
	struct timeval tv_now;
	struct task_struct *next_earliest_deadline_task = NULL;
	struct bvtqueue *bq = cpu_bq(smp_processor_id());
	
	/* Find the nearest global deadline, considering both
	 * runnable and sleeping tasks */
	find_nearest_global_deadline_overall(&next_earliest_deadline_task);
	
    if(next_earliest_deadline_task) {
		tv_fairshare_now_adjusted(&tv_now);
	    
		if ( (timeval_compare(&(next_earliest_deadline_task->cf.coop_t.dead_p.t_deadline),&(tv_now)) <0)) {
	       set_normalized_timespec(coop_prd, 0,0);
	    }
	    else {
			set_normalized_timespec(coop_prd,
				        next_earliest_deadline_task->cf.coop_t.dead_p.t_deadline.tv_sec - tv_now.tv_sec,
		 	 	        (next_earliest_deadline_task->cf.coop_t.dead_p.t_deadline.tv_usec - tv_now.tv_usec)*NSEC_PER_USEC);
			
        }
	return 1;
	}
	else return 0;

}
/* find_coop_period */

/* choose_next_coop: 
 * This function chooses the next coop process to run
 * from the coop domain id specified as the argument. 
 * If there is an immediate task whose deadline has
 * expired, it returns 0. However, if there is a task
 * whose deadline is in the future and there are no asaps
 * in the asap queue, it returns the time left in a timespec
 * val. The function does not sleep!
 * @w_dead: the deadline node at the top of the heap 
 * @w_asap: the asap node at the top of the heap.
 * @target_task: output parameter specifying the next coop task to
 * run (if any). 
 */
void  choose_next_coop(struct task_struct** target_task, int dom_id)
{
	struct timeval tv_now;
	struct task_struct *w_dead = NULL;
	struct task_struct *w_asap = NULL;
	
	coop_queue *cq = cpu_cq(smp_processor_id(), dom_id);

	//do_gettimeofday(&tv_now);
	tv_fairshare_now_adjusted(&tv_now);

	find_nearest_deadline_asap(cq, &w_dead,&w_asap);
	
	/* if we have both timeout and asaps in the heap we check if the deadline of 
	 * the timeout event has expired. If yes, we run the timeout event. If no, 
	 * we run the asap event. The same logic is used in the main event loop 
	 * while choosing which event to dispatch next.
	 */
	
	if (w_dead && w_asap) {
		
                /* both asap and timeout event found */
		
		if(timeval_compare(&w_dead->cf.coop_t.dead_p.t_deadline, &tv_now) < 0) {
			
                        /* If deadline expired run deadline event
			 */
			*target_task = w_dead;

		} else {
			/* deadline did not expire, run asap event, 
			 */
			*target_task = w_asap;
		}
	} else if (w_dead) {
		
  		*target_task = w_dead;
			
	}else if (w_asap) {

		/* only asap event in the heap, just run the asap event */

		*target_task = w_asap;

	}else {
		/* Both heaps are empty */
		*target_task = NULL;	}

	return;
}
/* choose_next_coop*/

/**
 * set_normalized_timeval - set timespec sec and nsec parts and normalize
 *
 * @tv:		pointer to timesval variable to be set
 * @sec:	seconds to set
 * @usec:	microseconds to set
 *
 * Set seconds and microseconds field of a timesval variable and
 * normalize to the timesval storage format
 *
 * Note: The tv_usec part is always in the range of
 * 	0 <= tv_usec < USEC_PER_SEC
 * For negative values only the tv_sec field is negative !
 * This implementation is stolen from kernel/time.c
 */
void set_normalized_timeval(struct timeval *tv, 
				   time_t sec, long usec)
{
	while (usec >= USEC_PER_SEC) {
		usec -= USEC_PER_SEC;
		++sec;
	}
	while (usec < 0) {
		usec += USEC_PER_SEC;
		--sec;
	}
	tv->tv_sec = sec;
	tv->tv_usec = usec;
}

/* sys_coop_poll - The coop_poll system call interface
 * @i_param: input deadline parameters
 * @o_param: output deadline parameters
 * 
 * This version of the coop_poll does not force a process to 
 * sleep on a completion variable/waitqueue. Instead, it just 
 * calls schedule() to yield the 
 * processor to another coop task. 
 * This system call is thus, very similar to sched_yield() call
 * that also yields the processor for a very small amount of 
 * time. In a combined fairshare & coop heuristic, this is 
 * expected to result in a much more simpler code and is 
 * conceptually more consistent with what we want it
 * to do. 
 */

asmlinkage long sys_coop_poll(struct coop_param_t __user *i_param, 
			      struct coop_param_t __user *o_param,
			      int dom_id)
{

	struct coop_param_t ki_param;
	struct coop_param_t ko_param;
	
	int ret;
	unsigned short yields = 0;
	unsigned long flags;
	int flg;
	int valid_dom_id = 0;
	mm_segment_t fs;
	struct timeval tv_now;	
	struct timespec tomono,tv_diff;
	coop_queue *cq;	
	struct bvtqueue *bq;
	struct task_struct *w_asap;
	struct timeval  tv_zero = { .tv_sec = 0, .tv_usec = 0};
	struct task_struct *upcoming_deadline_task = NULL;
	
	ko_param.t_deadline = tv_zero;
	ko_param.t_asap     = tv_zero;
	ko_param.p_asap     = 0;
	ko_param.have_asap  = 0;

	/* Sanity checks */
	/* check for valid domain id */

	valid_dom_id = (dom_id >= 0    && 
			dom_id <= (NR_COOP_DOMAINS -1));

	if (!valid_dom_id) {
		return -EINVAL;
	}

	if (i_param==NULL || o_param==NULL ) {
		printk(KERN_ERR "Invalid parameters\n");
		return -EINVAL;
	}

	if(current==NULL) {
		printk(KERN_ERR "Current task is invalid\n");
		return -EINVAL;
	}

	if(!is_bvt(current)) {
		printk(KERN_ERR "Current task is not in faircoop scheduling class\n");
		return -EINVAL;
	}

	/* copy input values to kernel space 
	 * this is where the kernel checks for memory access violations 
	 * This might put the process to sleep 
	 */
	ret = get_user_arg(&ki_param, i_param);
	if (ret)
		return ret;

	
	/* if a task has no asaps or deadlines to report,
	 * we pretend as if this guy is only a besteffort guy 
	 */

	if (unlikely(!GET_HAVE_ASAP(ki_param)) && 
	    unlikely(timeval_compare(&ki_param.t_deadline,&tv_zero) == 0)) {
		goto yield;
	}
       
	/* acquire the coop queue lock. 
	 * this disabled preemption and local IRQs and acquires
	 * the runqueue spinlock
	 */	
	cq = task_cq_lock(current, dom_id, &flags, &bq); 

	/* Sanity checks */
	if(cq==NULL || bq==NULL) {
		printk(KERN_ERR "coop queue or bvt queue is null!\n");
		return -EINVAL;
	}

	/* update the number of coop count in coop queue */
	
	cq->num_coop_calls++;
	
	if (bq->bvt_timer_active) {
		bvt_timer_cancel(&bq->bvt_timer);
		bq->bvt_timer_active = 0;
	}

	if (!is_coop_realtime(current)) {
		set_tsk_as_coop(current, dom_id);
	}
	
	current->cf.coop_t.is_well_behaved = 1;

	do_gettimeofday(&tv_now);

	/* Not needed, since a runnning task doesn't have its info in the queues*/
	#if 0
	/* remove my stale nodes from the coop heaps and re-insert new
	 * nodes based on my updated information
	 */
	remove_task_from_coop_queue(current, cq,0);
	#endif

	/* Insert my info into the heap */
	
	if ( likely(timeval_compare(&ki_param.t_deadline,&tv_zero) != 0) ) {
	
		total_deadlines++;

		/* If the deadline info just provided, is earlier than the overall global 
		 * deadline -- update target_virtual_time 
		 */
		
		/* need to find next global coop deadline (including coop sleeps) */
		find_nearest_global_deadline_overall(&upcoming_deadline_task);

		if(upcoming_deadline_task && (timeval_compare( &(upcoming_deadline_task->cf.coop_t.dead_p.t_deadline), &(ki_param.t_deadline) ) > 0) )
		{
		
			earlier_deadlines++;	

		}

		ret = insert_task_into_timeout_queue(&ki_param.t_deadline, 
						     				cq, current,1);
		if (unlikely(ret < 0)){
			remove_task_from_coop_queue(current, cq,0);
			cq_unlock(bq, &flags);
			return ret;
		}
	} /* if */
	
	flg = 0; /* will be = 1 only when there are asaps */
	
	if ( likely(GET_HAVE_ASAP(ki_param))) {
		ret = insert_task_into_asap_queue(&ki_param.t_asap, 
						  ki_param.p_asap, 
						  cq,
						  current);
		if (ret < 0) {
			remove_task_from_coop_queue(current, cq,0);
			cq_unlock(bq, &flags);
			return ret;
		} else {
			flg = 1;
		}
	} /* if */
	
	/* if there are no asaps and the deadline is in the
	 * future, make the process sleep in coop_poll() until
	 * the deadline expires
	 */
	if (!flg && timeval_compare(&ki_param.t_deadline,&tv_now) > 0) {
	
		/* calculate the time difference */
		set_normalized_timespec(&tv_diff, 
				       ki_param.t_deadline.tv_sec  - tv_now.tv_sec,
				       (ki_param.t_deadline.tv_usec - tv_now.tv_usec)*1000);
		if (bvt_sched_tracing == 4)
			printk(KERN_INFO "Task %d going into cooperative sleep\n",current->pid);
	
		cq_unlock(bq,&flags);
		/* TODO Take care of the call being interrupted by a signal */
		hrtimer_nanosleep(&tv_diff,NULL,HRTIMER_MODE_REL,CLOCK_MONOTONIC);
		goto wakeup;
	}

	cq_unlock(bq, &flags);
yield:
	schedule(); /* this is how we now yield */
wakeup:
	yields++;
		
	cq = task_cq_lock(current, dom_id, &flags, &bq);

	/* update the current running task node */

	cq->num_yields += yields;

	w_asap = NULL;
	flg = 0;

	/* remove, obtain most imp asap and then reinsert */
	if (current->cf.coop_t.coop_asap_heap_node) {
		remove_task_from_coop_queue(current,cq,2);
		flg = 1;
	}

	find_nearest_asap(cq, &w_asap);
	
	if (flg)
		__insert_into_asap_heap(cq,current);
	
	if (w_asap) {
		ko_param.t_asap     = w_asap->cf.coop_t.asap_p.t_asap;
		ko_param.p_asap     = w_asap->cf.coop_t.asap_p.priority;
		SET_HAVE_ASAP(ko_param);
	}
	
	cq_unlock(bq, &flags);
		
	/* adjust for wall time, monotonic time difference */
	tomono = wall_to_monotonic;
	set_normalized_timespec(&current->cf.coop_t.deadline, 
				current->cf.coop_t.deadline.tv_sec  - tomono.tv_sec,
				current->cf.coop_t.deadline.tv_nsec - tomono.tv_nsec);
	
	ko_param.t_deadline.tv_sec  = current->cf.coop_t.deadline.tv_sec;
	ko_param.t_deadline.tv_usec = current->cf.coop_t.deadline.tv_nsec / NSEC_PER_USEC;
	
	/* reset the well behaved flag */
	current->cf.coop_t.is_well_behaved = 0;

	/* send it to user process */
	return copy_to_user(o_param, &ko_param, sizeof(struct coop_param_t))? -EFAULT:0;
} 
/* sys_coop_poll */

/* This is the comparison function that determines the sort order of
 * the asap_events heap.  The order is determined first by priority,
 * then by deadline, finally FIFO.
 * This function, the way it is coded, is tricky. Notice the following:
 * The function basically compares two ASAP events and returns true or false 
 * according as 
 * ev_a > ev_b ? true:false
 * However, ev_a > ev_b means that eb_b should be served first and then ev_a 
 * because either its priority is higher or deadline or its rank is lesser than 
 * that for ev_a. Thus, our heap is actually sorted in ascending order.
 * Note also that priority of events are in positive logic, that is higher value 
 * means higher priority and vice versa. 
 * In short:
 * rank (ev_a) > rank (ev_b) means ev_b arrived earlier in queue than ev_a
 * deadline(ev_a) > deadline(ev_b) means ev_b has a deadline earlier thab ev_a
 * prio(ev_a) < prio(ev_b) means ev_a has a lower priority than ev_b. 
 */

gboolean coop_poll_asap_gt(heap_key_t a, heap_key_t b)
{
        struct task_struct *ev_a = a;
        struct task_struct *ev_b = b;

        if(ev_a->cf.coop_t.asap_p.priority == ev_b->cf.coop_t.asap_p.priority) {
		if(!timeval_compare(&ev_a->cf.coop_t.asap_p.t_asap, 
				    &ev_b->cf.coop_t.asap_p.t_asap)) {
			/* FIFO ordering */
			return(timespec_compare(&ev_a->cf.coop_t.asap_p.rank, 
						&ev_b->cf.coop_t.asap_p.rank) > 0);
		} else {
			/* Application can specify a deadline to
			 * distinguish events of equal priority.
			 * Earlier deadlines get served first. */
			return(timeval_compare(&ev_a->cf.coop_t.asap_p.t_asap,
					       &ev_b->cf.coop_t.asap_p.t_asap)>0);
		} /* else */
        } else  {
                /* primary key: priority order (descending) */
                return(ev_a->cf.coop_t.asap_p.priority < 
		       ev_b->cf.coop_t.asap_p.priority);
        } /* else */
}
/* coop_poll_asap_gt */



/* The following function is the comparison function for timeout events 
 * The events are sorted first by their deadlines and then by FIFO order.
 * The way the function is coded might be a little confusing to the readers.
 * Check the comments for the asap comparison function for a detailed explanation
 * as to the signifance of the value returned.
 * In short, rank (ev_a) > rank(ev_b) means ev_b has a higher priority because 
 * it arrived earlier in the queue.
 * deadline(ev_a) > deadline (ev_b) means that ev_b has a deadline closer in the 
 * future than ev_a.
 */

gboolean coop_poll_timeout_gt(heap_key_t a, heap_key_t b)
{
	struct task_struct *ev_a = a;
    struct task_struct *ev_b = b;

	g_assert(ev_a);
	g_assert(ev_b);

        if(unlikely(!timeval_compare(&ev_a->cf.coop_t.dead_p.t_deadline,
				     &ev_b->cf.coop_t.dead_p.t_deadline))) {
		/* FIFO ordering */
		return(timespec_compare(&ev_a->cf.coop_t.dead_p.rank, 
					&ev_b->cf.coop_t.dead_p.rank) > 0);
        } else {
                /* primary key: priority order (descending) */
		return(timeval_compare(&ev_a->cf.coop_t.dead_p.t_deadline,
				       &ev_b->cf.coop_t.dead_p.t_deadline) > 0 );
        } /* else */

} 
/* coop_poll_timeout_gt */

/* coop_init:
 * Initialize the per cpu data structures 
 * for coop_poll 
 */

void __init coop_init(void)
{
	coop_queue *cq;
	int i, dom_id;

	for_each_possible_cpu(i) {
		for_each_available_coop_domain(dom_id) {
			cq = cpu_cq(i, dom_id);

			/* Allocating memory here has the disadvantage
			 * that regardless of whether we have a
			 * process using coop_poll, we allocate memory
			 * for the two coop_poll heaps.  Thus, this
			 * may not be very memory efficient.
			 */
			cq->heap_asap       = create_heap (coop_poll_asap_gt, HEAP_INIT_CAP);
			cq->heap_deadline   = create_heap (coop_poll_timeout_gt, HEAP_INIT_CAP);
			cq->heap_coop_sleep = create_heap (coop_poll_timeout_gt, HEAP_INIT_CAP);

			if (unlikely((!cq->heap_asap) 
				     || (!cq->heap_deadline) 
				     || (!cq->heap_coop_sleep)) ) {
				/* memory allocation error */
				panic("unable to allocate memory for the coop heaps");
				
			}			
			cq->num_coop_calls    = 0;
			cq->num_yields        = 0;
		} /* COOP domain for */
		printk(KERN_INFO "CPU [%d] memory allocated to coop heaps for %d"
		       " coop domains.\n", i, dom_id);
	} /* SMP for */

	return;
}
/* coop_init */

/* coop_proc_init:
 * initialize coop data structure when a process is forked 
 * @p: the task_struct of the process being initiaized.
 */

void coop_proc_init(struct task_struct *p)
{
	p->cf.bvt_dom = NULL;
	memset (&p->cf.coop_t, 0, sizeof(struct coop_struct));
	p->cf.coop_t.coop_deadline_heap_node = NULL;
	printk("Nullyfying coop deadline heap node for %d\n",p->pid);
}
/*coop_proc_init */

#else /* !defined SCHED_COOPREALTIME */

asmlinkage long sys_coop_poll(struct coop_param_t __user *i_param, 
			      struct coop_param_t __user *o_param)
{

	printk(KERN_WARNING "coop_poll called but kernel has not been" 
	       "compiled with coop_poll support\n");

	return -ENOSYS; /* system call not implemented */
}

void __init coop_init(void) {}
void coop_proc_init(struct task_struct *p) {}


#endif /* !defined (SCHED_COOPREALTIME) */




