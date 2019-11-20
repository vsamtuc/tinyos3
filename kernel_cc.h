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

	This file provides routines for concurrency control and preemption management. 

	@{
*/


/* 
	Many of the header definitions for Mutexes and CondVars are in the 
   	tinyos.h file
*/
#include "kernel_sched.h"


/**
	@brief Try to lock a mutex.

	This function will try at most \c spins times to 
	acquire (set while unset) mutex \c lock. It returns a success flag.

	@param lock a pointer to the mutex
	@param spins the number of attempts to acquire `lock`
	@return 1 if `lock` was acquired, 0 on failure
  */
int spin_trylock(Mutex* lock, int spins);


/**
	@brief Lock a mutex by spinning.

	This function will loop until it acquires mutex `lock`.
	Internally, it calls @ref spin_trylock() repeatedly until it 
	succeeds.

	@param lock a pointer to the mutex
	@see spin_trylock()
  */
void spin_lock(Mutex* lock);


/**
	@brief Unock a mutex.

	@param lock a pointer to the mutex
	@see spin_lock()
  */
void spin_unlock(Mutex* unlock);



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
int kernel_timedwait(wait_queue* wq, TimerDuration timeout);

#define kernel_wait(wq) \
	kernel_timedwait((wq), NO_TIMEOUT)

/**
	@brief Signal a kernel condition to one waiter.

	This call must be made 
  */
void kernel_signal(wait_queue* wq);

/**
	@brief Signal a kernel condition to all waiters.
  */
void kernel_broadcast(wait_queue* wq);



/* @}  cc */

#endif


