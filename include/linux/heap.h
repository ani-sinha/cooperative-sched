#ifndef _LINUX_HEAP_H
#define _LINUX_HEAP_H


#include <linux/glib.h>

typedef gpointer heap_key_t;
typedef gpointer heap_data_t;

/* Note, it is required that these only be called on a non-empty
 * heap. 
 */

#define heap_is_empty(heap)		((heap)->size == 0)
#define heap_min_data(heap)	        ((heap)->nodes[1]->data)
#define heap_min_key(heap)	        ((heap)->nodes[1]->key)

/* The function that compares the key values of the heap nodes
 * The actual implementation and the polarity 
 * is left for the implementor of the function
 */

typedef gboolean (* heap_key_comp_func)(heap_key_t a, heap_key_t b);
/* structs are private, but we need them for the macros above */

struct _heap_node {
	heap_key_t  key;
	heap_data_t data;
	gint        index;   /* backpointer for delete */
}; 

typedef struct _heap_node heap_node;

/* The main heap data structure
 * Note that we do not use any locks to protect heap.
 * The reason is that if needed, we can embed the heap in
 * a larger data structure and use locks to protect the 
 * larger data structure as a whole.
 */

struct _heap_s {
	heap_key_comp_func key_comp;
	gint          size;
	gint          capacity;
	heap_node **nodes; /*  an array of pointers to heap nodes */
}; 

typedef struct _heap_s    heap_t;

/* All the operations possible on heap */

extern heap_t *create_heap (heap_key_comp_func key_gt, int initial_capacity);

extern int heap_ensure_capacity(heap_t *heap, int capacity);

extern heap_node *heap_insertt (heap_t *heap, heap_key_t key, heap_data_t data);
extern heap_node *heap_insert_nogrow (heap_t *heap, heap_key_t key, heap_data_t data);

extern heap_data_t  heap_delete_min     (heap_t *heap);
extern void         heap_delete         (heap_t *heap, heap_node *node);
extern void         destroy_heap        (heap_t *heap);

#endif /* _LINUX_HEAP_H */
