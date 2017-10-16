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


/** @brief Wait on a condition variable, specifying the cause. 

  This function must be called only while we have locked the mutex that 
  is associated with this call. It will put the calling thread to sleep, 
  unlocking the mutex. These operations happen atomically.  

  When the thread is woken up later (by another thread that calls @c 
  Cond_Signal or @c Cond_Broadcast, or because the timeout has expired, or
  because the thread was awoken by another kernel routine), 
  it first re-locks the mutex and then returns.  

  @param mx The mutex to be unlocked as the thread sleeps.
  @param cv The condition variable to sleep on.
  @param cause A cause provided to the kernel scheduler.
  @param timeout The time to sleep, or @c NO_TIMEOUT to sleep for ever.

  @returns 1 if this thread was woken up by signal/broadcast, 0 otherwise

  @see Cond_Signal
  @see Cond_Broadcast
  */
int cv_wait(Mutex* mx, CondVar* cv, 
	enum SCHED_CAUSE cause, TimerDuration timeout);


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
  */
void kernel_signal(CondVar* cv);

/**
	@brief Signal a kernel condition to all waiters.
  */
void kernel_broadcast(CondVar* cv);


/**
	@brief Put thread to sleep, unlocking the kernel.
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


