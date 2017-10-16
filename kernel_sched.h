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

#include "util.h"
#include "bios.h"
#include "tinyos.h"

/*****************************
 *
 *  The Thread Control Block
 *
 *****************************/ 

/** @brief Thread state. 

  A value of this type, together with a @c Thread_phase value, completely
  determines the state of the current API. 

  @see Thread_phase
*/
typedef enum { 
    INIT,       /**< TCB initialising */
    READY,      /**< A thread ready to be scheduled.   */
    RUNNING,    /**< A thread running on some core   */
    STOPPED,    /**< A blocked thread   */
    EXITED      /**< A terminated thread   */
  } Thread_state;

/** @brief Thread phase. 

  @see Thread_state
*/
typedef enum { 
    CTX_CLEAN,   /**< Means that, the context stored in the TCB is up-to-date. */
    CTX_DIRTY    /**< Means that, the context stored in the TCN is garbage. */
  } Thread_phase;

/** @brief Thread type. */
typedef enum { 
  IDLE_THREAD,    /**< Marks an idle thread. */
  NORMAL_THREAD   /**< Marks a normal thread */
} Thread_type;

/**
  @brief Designate different origins of scheduler invocation.

  This is used in the scheduler heuristics to determine how to
  adjust the dynamic priority of the current thread.
 */
enum SCHED_CAUSE {
  SCHED_QUANTUM,  /**< The quantum has expired */
  SCHED_IO,       /**< The thread is waiting for I/O */
  SCHED_MUTEX,    /**< Mutex_Lock yielded on contention */
  SCHED_PIPE,     /**< Sleep at a pipe or socket */
  SCHED_POLL,     /**< The thread is polling a device */
  SCHED_IDLE,     /**< The idle thread called yield */
  SCHED_USER      /**< User-space code called yield */
};



/**
  @brief The thread control block

  An object of this type is associated to every thread. In this object
  are stored all the metadata that relate to the thread.
*/
typedef struct thread_control_block
{
  PCB* owner_pcb;       /**< This is null for a free TCB */

  cpu_context_t context;     /**< The thread context */

#ifndef NVALGRIND
  unsigned valgrind_stack_id; /**< This is useful in order to register the thread stack to valgrind */
#endif

  Thread_type type;       /**< The type of thread */
  Thread_state state;    /**< The state of the thread */
  Thread_phase phase;    /**< The phase of the thread */

  void (*thread_func)();   /**< The function executed by this thread */

  TimerDuration wakeup_time; /**< The time this thread will be woken up by the scheduler */
  rlnode sched_node;      /**< node to use when queueing in the scheduler lists */

  struct thread_control_block * prev;  /**< previous context */
  struct thread_control_block * next;  /**< next context */
  
} TCB;



/** Thread stack size */
#define THREAD_STACK_SIZE  (128*1024)


/************************
 *
 *      Scheduler
 *
 ************************/


/** @brief Core control block.

  Per-core info in memory (basically scheduler-related)
 */
typedef struct core_control_block {
  uint id;                    /**< The core id */

  TCB* current_thread;        /**< Points to the thread currently owning the core */
  TCB idle_thread;            /**< Used by the scheduler to handle the core's idle thread */
  sig_atomic_t preemption;    /**< Marks preemption, used by the locking code */

} CCB;
 

/** @brief the array of Core Control Blocks (CCB) for the kernel */
extern CCB cctx[MAX_CORES];


/** @brief The current core's CCB */
#define CURCORE  (cctx[cpu_core_id])

/** 
  @brief The current thread.

  This is a pointer to the TCB of the thread currently executing on this core.
*/
#define CURTHREAD  (CURCORE.current_thread)

/** 
  @brief The current thread.

  This is a pointer to the PCB of the owner process of the current thread, 
  i.e., the thread currently executing on this core.
*/
#define CURPROC  (CURTHREAD->owner_pcb)


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

    @param newstate the new state for the thread
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

