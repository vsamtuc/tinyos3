/*
 *  Concurrency Control API
 *
 */


#ifndef __KERNEL_CC_H
#define __KERNEL_CC_H


/**
	@file kernel_cc.h
	@brief Concurrency and preemption control API.

	@defgroup cc Concurrency control.
	@ingroup kernel
	@brief Concurrency and preemption control API.

	This file provides routines for concurrency control and preemption management. 
*/




/* 
	Many of the header definitions for Mutexes and CondVars are in the 
   	tinyos.h file
*/
#include "kernel_sys.h"
#include "kernel_sched.h"




/*
 * Kernel preemption control.
 * These are wrappers for the kernel monitor.
 */

/**
	@brief Lock the kernel.
 */
void kernel_lock();

/**
	@brief Unlock the kernel.
 */
void kernel_unlock();

/**
	@brief Wait on a condition variable using the kernel lock.
	@returns 1 if signalled, 0 if not
  */
int kernel_wait_wchan(CondVar* cv, enum SCHED_CAUSE cause, 
	const char* wchan, TimerDuration timeout);

#define kernel_wait(cv, cause) \
	kernel_wait_wchan((cv),(cause),__FUNCTION__, NO_TIMEOUT)
#define kernel_timedwait(cv, cause, timeout) \
	kernel_wait_wchan((cv),(cause),__FUNCTION__, (timeout))

/**
	@brief Signal a kernel condition to one waiter.

	This call must be made 
  */
void kernel_signal(CondVar* cv);

/**
	@brief Signal a kernel condition to all waiters.
  */
void kernel_broadcast(CondVar* cv);


/**
	@brief Put thread to sleep, unlocking the kernel.

	System calls should call this function instead of @c sleep_releasing,
	as the kernel lock is not a mutex.
  */
void kernel_sleep(Thread_state state, enum SCHED_CAUSE cause);



/** @brief Set the preemption status for the current thread.

 	Depending on the value of the argument, this function will set preemption on 
 	or off. 
 	Preemption is disabled by disabling interrupts. This function is usually called 
 	via the convenience macros @c preempt_on and @c preempt_off.
	A typical non-preemptive section is declared as
	@code
	int preempt = preempt_off;
	..
	    // do stuff without preemption 
	...
	if(preempt) preempt_on;
	@endcode

	@param preempt  the new preemption status 
 	@returns the previous preemption status, where 0 means that preemption was previously off,
 	and 1 means that it was on.


 	@see preempt_off
 	@see preempt_on
*/
int set_core_preemption(int preempt);

/** @brief Get the current preemption status.

	@return the current preemption status for this core, 0 means no preemption and 1 means
	preemption.
	@see set_core_preemption
 */
int get_core_preemption();

/** @brief Easily turn preemption off.
	@see set_core_preemption
 */
#define preempt_off  (set_core_preemption(0))

/** @brief Easily turn preemption off.
	@see set_core_preemption
 */
#define preempt_on  (set_core_preemption(1))


#endif


