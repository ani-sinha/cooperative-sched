
/* *************************************************************
 * This file contains declarations and function definitions 
 * that are extracted from some
 * other part of the kernel and hence needs cleanup
 * *************************************************************
 */

#ifndef __LINUX_PRINT_DEBUG_H
#define __LINUX_PRINT_DEBUG_H 

#include <linux/time.h>
#include <linux/types.h>
#include <asm/current.h>
#include <linux/kernel.h>
#include <linux/sched.h>

extern struct timezone sys_tz;

/* The following declarations are shamelessly taken from 
 * the declaration of the structure kernel_timestamp in fs/udf/ecma_167.h
 * There has to be a global standard definition of this data type for 
 * use by several other subsystems in the kernel.
 */

typedef struct
{
	uint16_t	typeAndTimezone;
        uint16_t	year;
	uint8_t		month;
	uint8_t		day;
	uint8_t		hour;
	uint8_t		minute;
	uint8_t		second;
	uint8_t		milliseconds;
	uint8_t		microseconds;
} __attribute__ ((packed)) tm;


/* The following declarations and macros are shamelessly copied from 
 * fs/udf/udftime.c. 
 * In future, there has to be one single definition 
 * for a global use somewhere in the kernel.
 */

#define EPOCH_YEAR 1970
#define SECS_PER_HOUR	(60 * 60)
#define SECS_PER_DAY	(SECS_PER_HOUR * 24)

#ifndef __isleap
/* Nonzero if YEAR is a leap year (every 4 years,
   except every 100th isn't, and every 400th is).  */
#define	__isleap(year)	\
  ((year) % 4 == 0 && ((year) % 100 != 0 || (year) % 400 == 0))
#endif


/* How many days come before each month (0-12).  */
static const unsigned short int __mon_yday[2][13] =
{
	/* Normal years.  */
	{ 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 },
	/* Leap years.  */
	{ 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366 }
};


static void gmtime(tm *dest, struct timespec *ts)
{

	long int days, rem, y;
	const unsigned short int *ip;
	int16_t offset;

	offset = -sys_tz.tz_minuteswest;

	if (!dest)
		return;

	dest->typeAndTimezone = 0x1000 | (offset & 0x0FFF);

	ts->tv_sec += offset * 60;
	days = ts->tv_sec / SECS_PER_DAY;
	rem = ts->tv_sec % SECS_PER_DAY;
	dest->hour = rem / SECS_PER_HOUR;
	rem %= SECS_PER_HOUR;
	dest->minute = rem / 60;
	dest->second = rem % 60;
	y = EPOCH_YEAR;

#define DIV(a,b) ((a) / (b) - ((a) % (b) < 0))
#define LEAPS_THRU_END_OF(y) (DIV (y, 4) - DIV (y, 100) + DIV (y, 400))

	while (days < 0 || days >= (__isleap(y) ? 366 : 365))
	{
		long int yg = y + days / 365 - (days % 365 < 0);

		/* Adjust DAYS and Y to match the guessed year.  */
		days -= ((yg - y) * 365
			+ LEAPS_THRU_END_OF (yg - 1)
			- LEAPS_THRU_END_OF (y - 1));
		y = yg;
	}
	dest->year = y;
	ip = __mon_yday[__isleap(y)];
	for (y = 11; days < (long int) ip[y]; --y)
		continue;
	days -= ip[y];
	dest->month = y + 1;
	dest->day = days + 1;

	dest->milliseconds = ts->tv_nsec / 1000000;
	dest->microseconds = (ts->tv_nsec / 1000 - dest->milliseconds * 1000);
}

#define print_macro(flag, fmt,arg...) do {				\
		printk(KERN_NOTICE "%s %u  " fmt "\n",flag,		\
		       current->pid, ##arg);				\
	}while(0);

/* print_debug: the function to print debugging messages onto the
 * kernel ring buffer .
 * note that this function is critical-section and interrupt handler safe.
 * as it allocates memory using GFP_ATOMIC flag.
 */


static void print_debug(char* id, const char* fmt, ...)
{
	struct timeval   tv_now;
	struct timespec  ts_now;
	tm               t;
	va_list          args;

	char *buff = kmalloc(128, GFP_ATOMIC);
	
	va_start(args, fmt);
	vsnprintf(buff, 128, fmt, args);
	va_end(args);

	preempt_disable();

	do_gettimeofday(&tv_now);

	set_normalized_timespec(&ts_now,tv_now.tv_sec, tv_now.tv_usec * NSEC_PER_USEC);
	
	gmtime(&t, &ts_now);

	print_macro(id, "%02u:%02u:%02u.%06u.%06u     %s",
		    t.hour, 
		    t.minute, 
		    t.second, 
		    t.milliseconds, 
		    t.microseconds, 
		    buff); 			
	
	preempt_enable_no_resched();

	kfree(buff);
}

#endif
