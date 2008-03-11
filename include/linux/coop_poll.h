#ifndef _LINUX_COOP_POLL_H
#define _LINUX_COOP_POLL_H

#include <linux/time.h>
#include <linux/heap.h>

/* The following structures define the param struct for the coop_poll
 * system call.  This definition must also be exported in the
 * userspace.
 */

struct coop_param_t
{
	/* deadline for the timeout event */
	struct timeval t_deadline; 
	
	/* deadline for the asap event */
	struct timeval t_asap;
	
	/* asap priotity */
	int p_asap;

	/* whether asap exisis */
	int have_asap;
};


/* called from kernel/exit.c */
void kill_coop_task(struct task_struct*);

/* called from init/main.c:start_kernel() */
void __init coop_init(void);

/* called from kernel/fork.c:copy_process() */
void coop_proc_init(struct task_struct*);

/* Declarations for coop_poll syscall.
 * This file specifies common declarations and 
 * function call for coop_poll syscall. 
 *
 * Anirban Sinha, Charles Krasic {anirbans,krasic}@cs.ubc.ca
 * Ashvin Goel {ashvin@eece.utoronto.edu}
 * Nov 2006 - Aug 2007.
 * Dec 22 2006: We use completions to achieve 
 * the sleeping and waiting semantics for non-policing coop_poll.
 * Jun 1st, 2007: Organizational changes to seperate out the 
 * policing and non-policing versions of coop_poll.
 * *************************************************************
 * This header contains declarations that are used 
 * in the other parts of the kernel code for incorporating
 * coop_poll related changes. It also includes declarations 
 * that must be ported to the user space for using the 
 * coop_poll syscall.
 * A deliberate attempt has been made to touch the 
 * other parts of the kernel as minimally as possible.
 * Comments specify which declarations are used in which 
 * other parts of the kernel.
 * *************************************************************
 */ 

/* The following data structures define the data
 * field of the heap nodes. They are specific to 
 * the different kinds of events we want to keep
 * track of.
 */

struct __deadline_node {
	struct timespec rank; /* to establish FIFO order */
	struct timeval t_deadline; /* deadline time, 
				    * this is also the key of the heap node */
};

struct __asap_node {
	struct timespec rank; /* for FIFO order */ 
	struct timeval t_asap; /* asap deadline */
	int priority; /* asap priority */
};

typedef struct __deadline_node deadline_info;
typedef struct __asap_node asap_info;

/* following definitions are relevant to the heap insertion and
 * deletion functions */
#define COOP_TIMEOUT_HEAP 1
#define COOP_ASAP_HEAP    2
#define COOP_SLEEP_HEAP   3

typedef heap_t* coop_heap_asap;
typedef heap_t* coop_heap_deadline;

/* This is the main per CPU coop_poll data structure 
 * The most important data structure is the
 * priority queue for the asap and the deadline events.
 * This is embedded in the struct bvtqueue which is 
 * in itself embedded in per CPU runqueue.
 * Note that in this implementation, it is no longer 
 * in itself a per CPU variable. 
 */

struct _coop_queue
{

        /* the priority queues for the two kinds of 
	 * events, asap and deadline
	 */

	coop_heap_asap       heap_asap;
	coop_heap_deadline   heap_deadline;

	/* This is the heap of all the processes sleeping on the coop
	 * sleep. */

	coop_heap_deadline   heap_coop_sleep;
  
	/* The following is used not only for
	 * stats but also to determine the rank 
	 * of a asap and timeout node in the heap.
	 * It is incremented per coop_poll syscall 
	 * and is never decremented.
	 */

	unsigned long num_yields;
	unsigned long num_coop_calls;
};

typedef struct _coop_queue coop_queue;

/* coop_struct: This is the per process coop data structure */

struct coop_struct 
{
        /* These are backpointers to its corresponding 
	 * heap nodes of the ASAP and deadline heaps to 
	 * which this process was a member.
	 */

	heap_node *coop_asap_heap_node;
	heap_node *coop_deadline_heap_node;
	heap_node *coop_sleep_deadline_node; 
	heap_node *coop_deadline_global_heap_node;
	heap_node *coop_sleep_deadline_global_heap_node;

	/* The following boolean value is used to distinguish a coop_poll
	 * task from a non-coop poll task.
	 * This flag should never be set by any non-coop poll task for
	 * its own purposes.'
	 */

	int is_coop_task;

	/* The following data structures keep track of the deadline and 
	 * asap parameters
	 */

	 deadline_info dead_p;
	 asap_info     asap_p;
	/* is the task well behaved ? */
	int is_well_behaved;
	/* deadline of the coop task saved here */
	struct timespec deadline;
};

/* These two macros set and get the status of the asap event
 * They could also be exported to the userspace for a cleaner interface 
 * though not strictly necessary. 
 */

#define SET_HAVE_NO_ASAP(coop_param) (coop_param).have_asap = 0;
#define SET_HAVE_ASAP(coop_param) (coop_param).have_asap = 1;
#define GET_HAVE_ASAP(coop_param) (coop_param).have_asap

#define is_coop(tsk) (tsk)->cf.coop_t.is_coop_task
#define set_coop_task(tsk) (tsk)->cf.coop_t.is_coop_task = 1;
#define clear_coop_task(tsk) (tsk)->cf.coop_t.is_coop_task = 0;

extern inline int is_coop_realtime(struct task_struct* tsk);

/* used in fs/proc/proc_misc.c */
extern struct file_operations proc_coopstat_operations;

void find_nearest_global_deadline(struct task_struct **w_dead);
void find_nearest_global_asap(struct task_struct **w_asap);
void find_nearest_global_deadline_overall(struct task_struct **overall);
void remove_task_from_coop_queue(struct task_struct*,coop_queue*,int);
coop_queue* cpu_cq(int,int);

int find_coop_period(struct task_struct *next, 
		     struct task_struct **next_coop,
		     struct timespec* coop_prd);
long insert_into_coop_heaps(coop_queue *cq,
			    struct task_struct *p, 
			    int which_heap); 
void choose_next_coop(struct task_struct** target_task, int dom_id);
void test_remove_task_from_coop_bvt_queues(struct task_struct *tsk, 
					   coop_queue *cq);
void remove_task_from_coop_sleep_queue(struct task_struct *tsk, 
				       coop_queue *cq);
void find_next_nearest_global_deadlines(struct task_struct *next_earliest_deadline_task,struct task_struct **next_to_next_earliest_deadline_task);
void find_second_nearest_global_deadline_overall(struct task_struct **overall);
void set_normalized_timeval(struct timeval *tv, time_t sec, long usec);
gboolean coop_poll_timeout_gt(heap_key_t a, heap_key_t b);

#endif /* _LINUX_COOP_POLL_H */
