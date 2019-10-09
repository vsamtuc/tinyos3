/*
 *  Scheduler API and implementation 
 *
 */

#ifndef __KERNEL_SCHED_H
#define __KERNEL_SCHED_H

/**
  @file kernel_sched.h
  @brief TinyOS kernel: The Scheduler API

  @defgroup scheduler Scheduler
  @ingroup kernel
  @brief The Scheduler API

  This file contains the definition of the scheduler API, exported to other modules
  of the kernel.

  @{
*/

#include "bios.h"
#include "tinyos.h"
#include "util.h"

/*****************************
 *
 *  The Thread Control Block
 *
 *****************************/

/** @brief Thread state. 

  A value of this type, together with a @c Thread_phase value, completely
  determines the state of a thread. 

  @see Thread_phase
*/
typedef enum {
	INIT, /**< @brief TCB initialising */
	READY, /**< @brief A thread ready to be scheduled.   */
	RUNNING, /**< @brief A thread running on some core   */
	STOPPED, /**< @brief A blocked thread   */
	EXITED /**< @brief A terminated thread   */
} Thread_state;

/** @brief Thread phase. 

  The phase of a thread denotes the state of its context stored in the @c TCB of the
  thread. 
  A @c CTX_CLEAN thread means that, the context stored in the TCB is up-to-date. 
  In this case, it is legal to swap context to this thread.
  A @c CTX_DIRTY thread marks the case when the thread was, or still is, being executed 
  at some core, therefore its stored context should not be used.

  The following **invariant** of the scheduler guarantees 
  correctness:  

  > A TCB is in the scheduler
  > queue, if and only if, its @c Thread_state is @c READY and the @c Thread_phase 
  > is @c CTX_CLEAN.

  @see Thread_state
*/
typedef enum {
	CTX_CLEAN, /**< @brief Context is clean. */

	CTX_DIRTY /**< @brief Context is dirty. */
} Thread_phase;

/** @brief Thread type. */
typedef enum {
	IDLE_THREAD, /**< @brief Marks an idle thread. */
	NORMAL_THREAD /**< @brief Marks a normal thread */
} Thread_type;

/**
  @brief Designate different origins of scheduler invocation.

  This is used in the scheduler heuristics to determine how to
  adjust the dynamic priority of the current thread.
 */
enum SCHED_CAUSE {
	SCHED_QUANTUM, /**< @brief The quantum has expired */
	SCHED_IO, /**< @brief The thread is waiting for I/O */
	SCHED_MUTEX, /**< @brief @c Mutex_Lock yielded on contention */
	SCHED_PIPE, /**< @brief Sleep at a pipe or socket */
	SCHED_POLL, /**< @brief The thread is polling a device */
	SCHED_IDLE, /**< @brief The idle thread called yield */
	SCHED_USER /**< @brief User-space code called yield */
};

/**
  @brief The thread control block

  An object of this type is associated to every thread. In this object
  are stored all the metadata that relate to the thread.
*/
typedef struct thread_control_block {
	PCB* owner_pcb; /**< @brief This is null for a free TCB */

	cpu_context_t context; /**< @brief The thread context */

#ifndef NVALGRIND
	unsigned valgrind_stack_id; /**< @brief Valgrind helper for stacks. 

	  This is useful in order to register the thread stack to the valgrind memory profiler. 
	  Valgrind needs to know which parts of memory are used as stacks, in order to return
	  meaningful error information. 

	  This field is not relevant to anything in the TinyOS logic and can be ignored.
	  */
#endif

	Thread_type type; /**< @brief The type of thread */
	Thread_state state; /**< @brief The state of the thread */
	Thread_phase phase; /**< @brief The phase of the thread */

	void (*thread_func)(); /**< @brief The initial function executed by this thread */

	TimerDuration wakeup_time; /**< @brief The time this thread will be woken up by the scheduler */

	rlnode sched_node; /**< @brief Node to use when queueing in the scheduler lists */
	TimerDuration its; /**< @brief Initial time-slice for this thread */
	TimerDuration rts; /**< @brief Remaining time-slice for this thread */

	enum SCHED_CAUSE curr_cause; /**< @brief The endcause for the current time-slice */
	enum SCHED_CAUSE last_cause; /**< @brief The endcause for the last time-slice */

} TCB;

/** @brief Thread stack size.

  The default thread stack size in TinyOS is 128 kbytes.
 */
#define THREAD_STACK_SIZE (128 * 1024)

/************************
 *
 *      Scheduler
 *
 ************************/

/** @brief Core control block.

  Per-core info in memory (basically scheduler-related). 
 */
typedef struct core_control_block {
	uint id; /**< @brief The core id */

	TCB* current_thread; /**< @brief Points to the thread currently owning the core */
	TCB* previous_thread; /**< @brief Points to the thread that previously owned the core */
	TCB idle_thread; /**< @brief Used by the scheduler to handle the core's idle thread */
	sig_atomic_t preemption; /**< @brief Marks preemption, used by the locking code */

} CCB;

/** @brief the array of Core Control Blocks (CCB) for the kernel */
extern CCB cctx[MAX_CORES];

/** @brief The current core's CCB */
#define CURCORE (cctx[cpu_core_id])

/** 
  @brief The current thread.

  This is a pointer to the TCB of the thread currently executing on this core.
*/
#define CURTHREAD (CURCORE.current_thread)

/** 
  @brief The current thread.

  This is a pointer to the PCB of the owner process of the current thread, 
  i.e., the thread currently executing on this core.
*/
#define CURPROC (CURTHREAD->owner_pcb)

/**
  @brief A timeout constant, denoting no timeout for sleep.
*/
#define NO_TIMEOUT ((TimerDuration)-1)

/**
	@brief Create a new thread.

	This call creates a new thread, initializing and returning its TCB.
	The thread will belong to process @c pcb and execute @c func.
    Note that, the new thread is returned in the @c INIT state.
    The caller must use @c wakeup() to start it.

    @param pcb  The process control block of the owning process. The
                scheduler simply stores this value in the new TCB, and
                otherwise ignores it

    @param func The function to execute in the new thread.
    @returns  A pointer to the TCB of the new thread, in the @c INIT state.
*/
TCB* spawn_thread(PCB* pcb, void (*func)());

/**
  @brief Wakeup a blocked thread.

  This call will change the state of a thread from @c STOPPED or @c INIT (where the
  thread is blocked) to @c READY. 

  @param tcb the thread to be made @c READY.
  @returns 1 if the thread state was @c STOPPED or @c INIT, 0 otherwise

*/
int wakeup(TCB* tcb);

/** 
  @brief Block the current thread.

	This call will block the current thread, changing its state to @c STOPPED
	or @c EXITED. Also, the mutex @c mx, if not `NULL`, will be unlocked, atomically
	with the blocking of the thread. 

	In particular, what is meant by 'atomically' is that the thread state will change
	to @c newstate atomically with the mutex unlocking. Note that, the state of
	the current thread is @c RUNNING. 
	Therefore, no other state change (such as a wakeup, a yield, another sleep etc) 
	can happen "between" the thread's state change and the unlocking.
  
	If the @c newstate is @c EXITED, the thread will block and also will eventually be
	cleaned-up by the scheduler. Its TCB should not be accessed in any way after this
	call.

	A cause for the sleep must be provided; this parameter indicates to the scheduler the
	source of the sleeping operation, and can be used in scheduler heuristics to adjust 
	scheduling decisions.

	A timeout can also be provided. If the timeout is not @c NO_TIMEOUT, then the thread will
	be made ready by the scheduler after the timeout duration has passed, even without a call to
	@c wakeup() by another thread.

	@param newstate the new state for the current thread, which must be either stopped or exited
	@param mx the mutex to unlock.
	@param cause the cause of the sleep
	@param timeout a timeout for the sleep, or 
   */
void sleep_releasing(Thread_state newstate, Mutex* mx, enum SCHED_CAUSE cause, TimerDuration timeout);

/**
  @brief Give up the CPU.

  This call asks the scheduler to terminate the quantum of the current thread
  and possibly switch to a different thread. The scheduler may decide that 
  it will renew the quantum for the current thread.
 */
void yield(enum SCHED_CAUSE cause);

/**
  @brief Enter the scheduler.

  This function is called at kernel initialization, by each core,
  to enter the scheduler. When this function returns, the scheduler
  has stopped (there are no more active threads) and the 
*/
void run_scheduler(void);

/**
  @brief Initialize the scheduler.

   This function is called during kernel initialization.
 */
void initialize_scheduler(void);

/**
  @brief Quantum (in microseconds) 

  This is the default quantum for each thread, in microseconds.
  */
#define QUANTUM (10000L)

/** @} */

#endif
