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


