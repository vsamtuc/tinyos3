/*
 *  Concurrency Control API
 *
 */


#ifndef __KERNEL_CC_H
#define __KERNEL_CC_H




/**
	@file kernel_cc.h
	@brief Concurrency and preemption control API.

*/


/*! @defgroup kernel  The TinyOS Kenrel.
 */


/**	@defgroup cc Concurrency control.
	@ingroup kernel
	@brief Concurrency and preemption control API.

	This file provides routines for concurrency control and preemption management.

	@{ 
*/



/* 
	Many of the header definitions for Mutexes and CondVars are in the 
   	tinyos.h file
*/
#include "tinyos.h"



/**
 * @brief The kernel lock.
 *
 * This mutex is used to protect most of the resources in kernel-space (the preemptive domain
 * of the kernel). 
 */

extern Mutex kernel_mutex;          /* lock for resource tables */


/*
 * Kernel preemption control
 */


/** @brief Set the preemption status for the current thread.

 	Depending on the value of the argument, this function will set preemption on or off. 
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


/** @} */

#endif


