#ifndef _LINUX_GLIB_H
#define _LINUX_GLIB_H

/* This is a set of workarounds that can be 
 * used to port glib type definitions into kernel for 
 * codes already using glib definitions
 */


#include <linux/kernel.h>
#include <linux/compiler.h>
#include <linux/slab.h>

#if !defined(gboolean)  

#define gboolean bool

#if !defined(boolean)
#define boolean bool
#endif


#endif

typedef void* gpointer;
typedef int gint;
typedef const void *gconstpointer;
typedef char   gchar;
typedef unsigned char   guchar;
typedef unsigned int    guint;
typedef short  gshort;
typedef unsigned short  gushort;
typedef long   glong;
typedef unsigned long   gulong;
typedef signed char gint8;
typedef unsigned char guint8;
typedef signed short gint16;
typedef unsigned short guint16;
typedef signed int gint32;
typedef unsigned int guint32;
typedef float   gfloat;
typedef double  gdouble;
typedef unsigned int gsize;
typedef signed int gssize;


#define g_assert(expr)						    \
  if(unlikely(!(expr))) {					    \
    printk(KERN_ERR "Assertion failed! %s,%s,%s,line=%d\n",	    \
	   #expr,__FILE__,__FUNCTION__,__LINE__);		    \
    dump_stack();						    \
    BUG();							    \
  }


#define g_free(pointer)                      kfree(pointer);

#define g_new0(struct_type, n_structs)				\
	kcalloc((n_structs), sizeof(struct_type), GFP_KERNEL);	
      
#define g_new(n_bytes)				\
	kmalloc(n_bytes, GFP_KERNEL);
	
#define g_new_atomic(n_bytes)			\
	kmalloc(n_bytes, GFP_ATOMIC);


/* g_realloc: 
 * This function allocates a contigious chunk of kernel memory 
 * pages of given size. Then copies the contents of old memory to the new location. 
 * @mem: pointer to old memory location; if null, the call is equivalent to 
 * a call to kmalloc with memory chunk = n_bytes. It then returns the pointer 
 * to the newly allocated location
 * @old_n_bytes: number of bytes allocated to the old pointer. Is there a way to get 
 * this information from within the kernel? 
 * @n_bytes: new number of bytes to allocate. if this is zero, it frees the
 * memory location pointed to by mem and returns NULL.
 * After all this is done, it frees the old reserved memory chunk
 * Returns pointer to the newly allocated memory location
 * on all other cases.
 * ===================================================================================
 * Warning: For reallocating a large chunk of memory, use of this function 
 * is not adviced.
 *
 */

static inline gpointer g_realloc (gpointer mem, gulong old_n_bytes, gulong n_bytes)
{
	void *new_mem;

	/* if mem is null, allocate new chunk using kmalloc */

	if (!mem && n_bytes) {
		new_mem=g_new(n_bytes);
		return new_mem;
	}

	if(mem && (!n_bytes)) {
		kfree(mem);
		return 0;
	}

	if(!mem && !n_bytes) return NULL;
	
	new_mem = g_new(n_bytes);

	if (unlikely(!new_mem)) {
		printk(KERN_ERR "%s, %s, line:%d : unable to alloc buffer\n",__FILE__,  __FUNCTION__, __LINE__);
		dump_stack();
		return NULL;
	}
	
	if (old_n_bytes < n_bytes )
		memcpy(new_mem,mem,(size_t) old_n_bytes);
	else 
		memcpy(new_mem,mem,(size_t) n_bytes);

	kfree(mem);
	return new_mem;
}

#endif

