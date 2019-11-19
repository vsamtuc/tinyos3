
/*
 *  Scheduler queue implementation 
 *
 */

#ifndef __KERNEL_SCHED_QUEUE_H
#define __KERNEL_SCHED_QUEUE_H


/**
	@file kernel_sched_queue.h
	@brief Scheduler queue and algorithms

	@defgroup sched_queue Scheduler queue
	@ingroup kernel

	This file contains the API related to the scheduler queue, described below.

  	The TinyOS scheduler uses the scheduling queue API in order to __select__
  	the next thread that will acquire a timeslice.
   	The set of threads among which the selection will be made consists of all
   	candidate threads in the system, and in particular:
	- All threads in the queue. According to the semantics of scheduling, 
	  the queue must contain every thread that is @c READY and @c CTX_CLEAN. 
	- Possibly the current thread, it it is @c READY. The current thread is
	  not in the queue, because it is not @c CTX_CLEAN.
	- The idle thread of the correpsonding core, as a last resort.

	The scheduler queue algorithm has two responsibilities:
	1. identify the next thread to be given a quantum on the current core, and
	2. determine the length of this quantum, by setting attribute \c its on the
	   selected TCB.

 	The scheduling algorithm is able to utilize all the information that is available
 	through the TCB of each thread. In order to support this, the TCB includes an
 	attribute of type @ref sched_queue_tcb, which is maintained exclusively by the
 	queue.

  @{
*/


/**
	@brief A structure holding queue-related information inside each TCB.
  */
struct sched_queue_tcb
{

};

/**
	@brief Initialize the scheduler queue at boot time.
  */
void initialize_sched_queue();


/**
	@brief Initialize the TCB of every thread.

	This call is called to initialize every TCB in the system.
	Before the scheduler even starts running, this is called once
	for every idle thread. In addition, this is called every time
	a new thread is created by \c spawn_thread(). The passed TCB
	already has all other fields initialized.

	The scheduler mutex is **not** held during this call.
  */
void sched_queue_init_tcb(TCB* tcb);


/**
	@brief Add this thread to the scheduler list.

	The scheduler calls this function when it detects that \c tcb
	has become eligible for inclusion in the sceduler queue, i.e.,
	when it becomes \c READY and \c CTX_CLEAN.

	Note: the scheduler mutex is held during this call.

	@param tcb the tcb to add to the scheduler queue
*/
void sched_queue_add(TCB* tcb);


/**
	@brief Make the next scheduling decision.

	The scheduler calls this function in order to make the next scheduling
	decision for the calling core. It is the responsibility of this call to 
	make a decision maintaining all the proper invariants.

	The argument \c current is the \c TCB of the current thread, and its fields
	have been updated to reflect the status of the quantum that is about to end.
	In particular,
	- \c its contains the initial amount of time given to the thread in this quantum 
	- \c rts contains the remaining time of the thread's quantum that was unused,
	- \c curr_cause contains the cause of the current call to the scheduler
	- \c last_cause contains the cause of the previous call to the sceduler from
	   this thread.
	- \c q contains the information maintained by the scheduling algorithm itself

	This function should return its decision for the next thread scheduled on this core.
	The \c its field on the \c TCB should be set to the length of the quantum for the
	selected thread.

	Note: the scheduler mutex is held during this call.

	@param tcb the tcb currently holding the core, i.e., \c CURTHREAD.
	@return the selected tcb to be given the next quantum on this core.
	     The length of the quantum should be stored in the \c its field
	     of the returned TCB.
*/
TCB* sched_queue_select(TCB* current);



/** @}  sched_queue */

#endif