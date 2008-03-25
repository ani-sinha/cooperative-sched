/* Nov 2006: This is a generic heap library has been ported from the userlevel
 * heap implementation in Qstream (http://www.qstream.org)
 * Parts of the code uses userlevel glib library keywords that has
 * been ported to traditional C syntax using glib.h definitions
 * - Anirban Sinha, anirbans@cs.ubc.ca
 * - Charles Krasic, krasic@cs.ubc.ca
 *
 * Mar 2007: We make modifications so that:
 * Now we have two variations of heap_insert() function.
 *
 * (a) normal heap_insert(): checks if heap has enough space 
 * and then grows heap if required provided we are not in atomic() or 
 * interrupt context. If we are in interrupt or atomic region,it panics!
 * 
 * (b) heap_insert_nogrow(): checks to see if heap has enough space 
 * and then panics if the space is insufficient. Does not attempt to 
 * grow the heap in any situation.
 * 
 * In both functions, if heap has enogh space, all memory 
 * allocations for heap is done during heap initialization, it is no
 * longer required to allocate memory for heap node at every call.
 * This makes the heap 
 * implementations scale to situations when heap_insert() is
 * called from within atomic blocks. Since we prefer not to use per-CPU
 * page cache for heap memory allocations, this is important. 
 * In atomic sections, essentially we would call the "nogrow" version of 
 * the heap_insert() function call. 
 * Similarly heap nodes are not actually deleted at every call to 
 * heap_delete(). The nodes are deleted once and for all when 
 * heap_destroy() is called. 
 * It is hoped that these reduced memory operations will make the 
 * heap implementation more efficient within the kernel. 
 * - Ani, anirbans@cs.ubc.ca
 */

#include <linux/heap.h>
#include <linux/spinlock.h>
#include <linux/hardirq.h>

#define HEAP_EXPANSION_FACTOR 2

#define FAIRCOOP_HEAP_DEBUG 1

#ifndef MAX
#define MAX(a, b)  (((a) > (b)) ? (a) : (b))
#endif

//static inline void heap_is_correct(heap_t *heap) __attribute__((always_inline));

#if defined(FAIRCOOP_HEAP_DEBUG)

#define heap_is_correct(heap) do { \
	gint __i; \
	if(bvt_sched_tracing == 3 && ( !irqs_disabled()) ) {\
		printk(KERN_ERR "Preemption/Irq enabled at %s line %d, \
		irq = %d, preempt = %d\n", __func__, __LINE__, irqs_disabled(), preempt_count()); \
		BUG(); \
	} \
	for(__i=1;__i<=heap->size;__i++) \
		if(!(heap->nodes[__i]->key)) { \
			printk(KERN_ERR "Heap got fucked at %s line %d, index with null entry = %d,heap size = %d\n",__func__,__LINE__, __i,heap->size); \
			dump_stack(); \
			panic("Halting"); \
		} \
	} while(0)

#else

void heap_is_correct(heap_t *heap) {}

#endif

#if 0
void heap_is_correct(heap_t *heap)
{
        gint i;

        for (i = 1; (i * 2 + 1) < heap->size; i++) {
                if (heap->key_comp(heap->nodes[i]->key, heap->nodes[i * 2]->key) 
                    || heap->key_comp(heap->nodes[i]->key, 
                                    heap->nodes[i * 2 + 1]->key)) {
			printk(KERN_ERR "%s: heap has a problem at i = %d\n", 
				  __func__, i);
			BUG();
                } /* for */
        } /* for */
}
/* heap_is_correct */
#endif

/* Allocate new heap node */
static inline heap_node *new_heap_node(heap_t *heap, heap_key_t key, heap_data_t data, gint index)
{
	/* The heap pointer is really not needed in this version of the code 
	 * but is intentionally kept as a parameter so that later we may 
	 * be able to optimise the memory allocation by allocating a chunk
	 * of memory using the slab allocator and using smaller chunks from 
	 * there as and when new memory allocation is needed for the nodes.
	 */
  
	heap_node *node;
	
	node = g_new(sizeof(heap_node) * 1);

	/* on failure to allocate memory for the new node */
	if (!node) {
		/* handle memory allocation error in the caller */
		return NULL;
	}

	node->key = key;
	node->data = data;
	node->index = index;
	return(node);
} /* new_heap_node */


/* Deallocate memory for the new node */

static inline void free_heap_node(heap_t *heap, heap_node *node)
{

	/* The heap pointer is really not needed in this version of the code 
	 * but is intentionally kept as a parameter so that later we may 
	 * be able to optimise the memory allocation by allocating a chunk
	 * of memory using the slab allocator and using smaller chunks from 
	 * there as and when new memory allocation is needed for the nodes.
	 */
  
	g_assert(node);
	g_free(node);

} /* free_heap_node */

/* Make the heap bigger */
 
static inline int grow_heap(heap_t *heap, gint capacity)
{
	int i;
       
	g_assert(heap);

	if(heap->capacity > capacity) return 0;

	/* Note: I have modified the original g_realloc prototype.
	 * It now takes the old allocated size as a parameter.
	 * Is there a way to get the amount of memory allocated to the old
	 * pointer automatically? 
	 */

	heap->nodes = g_realloc(heap->nodes, (heap->capacity + 1) * sizeof(*heap->nodes),
				(capacity + 1) * sizeof(*heap->nodes));

	if(unlikely(!heap->nodes)) return 1;

	
	for(i=heap->capacity+1; i<=capacity;i++) {
		heap->nodes[i] = new_heap_node(heap, NULL, NULL, i);
		if(!heap->nodes[i]) return 1;
	}
	
	heap->capacity = capacity;
	return 0;

} /* grow_heap */

/* Create a new heap */

heap_t * create_heap(heap_key_comp_func key_comp, gint initial_capacity)
{
	int i;
	heap_t *heap;

	g_assert(key_comp);

	heap = g_new0(heap_t, 1);

	if(likely(heap)) {
		g_assert(key_comp);
		heap->key_comp = key_comp;
		heap->capacity = MAX(1,initial_capacity);
		heap->size = 0;

		/* reserve one extra memory for the sentinel node */

		heap->nodes = g_new(sizeof (heap_node*) * (initial_capacity + 1)); 

		if(unlikely (!heap->nodes)) return NULL; /* memory allocation failure */
		
		for(i=1;i<=initial_capacity; i++)
		{
			heap->nodes[i] = new_heap_node(heap, NULL, NULL, i);
			if (unlikely(!heap->nodes[i])) return NULL;
		}
		
		return(heap);
	}
	else /* memory allocation failed */
		return NULL; /* It is the responsibility of the caller to 
			   * take care of the allocation failure 
			   */

} /* create_heap */

int heap_ensure_capacity(heap_t *heap, gint capacity)
{
	if (heap->capacity >= capacity) {
		return 0;
	} /* if */

	return grow_heap(heap, capacity);
} /* heap_ensure_capacity */
  
/* Cannot use this, as this may call realloc, which may sleep, This code is run in interrupt context */
#if 0
heap_node *heap_insertt(heap_t *heap, heap_key_t key, heap_data_t data)
{
	gint i;
	heap_node* node;

	heap_is_correct(heap);

	if (heap->size == heap->capacity) {

		if (unlikely (grow_heap(heap, heap->capacity * HEAP_EXPANSION_FACTOR))) 
			return NULL; 
		/* Error: memory allocation failure while expanding heap */

	} /* if */

	heap->size++;

	node = heap->nodes[heap->size];

	for (i = heap->size;
	     i > 1 && heap->key_comp(heap->nodes[i/2]->key, key);
	     i /= 2) {
		heap->nodes[i]          = heap->nodes[i / 2];
		heap->nodes[i]->index   = i;
		
	} /* for */
	
	/* Notice: No memory allocations !!! */
	
	heap->nodes[i]        = node;
	heap->nodes[i]->key   = key;
	heap->nodes[i]->data  = data;
	heap->nodes[i]->index = i;
	
	heap_is_correct(heap);
	
	return(heap->nodes[i]);
	

} /* heap_insert */
#endif

heap_node *heap_insertt(heap_t *heap, heap_key_t key, heap_data_t data)
{
	gint i;
	heap_node* node;

	g_assert(heap);
	g_assert(key);
	g_assert(data);

	heap_is_correct(heap);
	
	if (heap->size == heap->capacity) {
		printk(KERN_ERR "insufficient space in heap");
		dump_stack();
		BUG();
		
	} /* if */

	heap->size++;

	node = heap->nodes[heap->size];

	for (i = heap->size;
	     i > 1 && heap->key_comp(heap->nodes[i/2]->key, key);
	     i /= 2) {
		heap->nodes[i]          = heap->nodes[i / 2];
		heap->nodes[i]->index   = i;
		
	} /* for */
	
	/* Notice: No memory allocations !!! */
	
	heap->nodes[i]        = node;
	heap->nodes[i]->key   = key;
	heap->nodes[i]->data  = data;
	heap->nodes[i]->index = i;
	
	heap_is_correct(heap);
	
	return(heap->nodes[i]);
	

} /* heap_insert_nogrow */
    

/* Depending on whether the list is sorted in 
 * ascending or descending order,  this function
 * deletes minimum or maximum key. The key deleted is always 
 * the first key in the heap 
 */

heap_data_t heap_delete_min(heap_t *heap)
{
	heap_data_t  result;

	if (heap_is_empty(heap)) {
		return NULL;
	} /* if */

	result = heap->nodes[1]->data;

	heap_delete(heap, heap->nodes[1]);

	return(result);
} /* heap_delete_min */


/* The following function does not necessarily return the 
 * smaller of the two children! It depends on the user of 
 * the function whether the smaller or the larger of the 
 * two children is obtained based on his implementation 
 * of the key_comp function.
 * If key_comp(key1,key2)=true when key1 > key2 and
 *                        =false otherwise
 * the function actually returns the smaller of the two
 * children. 
 * Otheriwise it returns the larger of the two children.
 */

static __inline__ gint heap_smaller_child(heap_t *heap, gint i)
{
	gint child;

	child = i * 2;
	if((child < heap->size) && 
	   heap->key_comp(heap->nodes[child]->key,
			heap->nodes[child + 1]->key) ) {
		child++;
	} /* if */
	return(child);
}
/* heap_smaller_child */

/* heap_delete:
 * This function deletes an arbitrary node from 
 * the heap. 
 * Be careful about the polarity of the comparison
 * function if you are trying to follow the algorithm.
 */

void heap_delete(heap_t *heap, heap_node *node)
{
	gint i;
	gint child;
	gint parent;
	heap_node *replacement;
	heap_key_t   key;
	
	g_assert(node);

	g_assert(heap);

	heap_is_correct(heap);
	
	/* Replace the deleted value with whatever was last */
	replacement = heap->nodes[heap->size];
	key         = replacement->key;

	heap->size--;

	i = node->index;

	/*  Replace the node with the previous last value, but find a
	 *  spot that maintains the heap ordering rules. Only one of
	 *  the following loops will execute (if at all).  */
	
	parent = i/2;

	while((i > 1) && 
	      heap->key_comp(heap->nodes[parent]->key, key)) {
	
		/* we need to demote some ancestors */

		heap->nodes[i]        = heap->nodes[parent];
		heap->nodes[i]->index = i;
		i                     = parent;
		parent                /= 2;
	} /* while */

	/* Choose the smaller child */
	child = heap_smaller_child(heap, i);

	while((child <= heap->size) &&
	      heap->key_comp(key, heap->nodes[child]->key)) {

		/* promote the smallest decendants  */
	    
		heap->nodes[i]        = heap->nodes[child];
		heap->nodes[i]->index = i;
		i                     = child;
		child                 = heap_smaller_child(heap, i);
	} /* while */
    
	heap->nodes[i]        = replacement;
	heap->nodes[i]->index = i;


	/* Instead of deallocating memory, we simply mark the 
	 * old memory as available and update pointers 
	 */

	node->key                 = NULL;
	node->data                = NULL;
	node->index               = heap->size +1;
	heap->nodes[heap->size+1] = node;
	heap_is_correct(heap);

} /* heap_delete */

void
destroy_heap(heap_t *heap) 
{
	gint i;

	for (i = heap->capacity; i > 0; i--) {
		free_heap_node(heap, heap->nodes[i]);
	} /* for */

	g_free(heap->nodes);
	g_free(heap);
} /* destroy_heap */


