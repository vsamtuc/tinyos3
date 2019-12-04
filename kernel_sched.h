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
#include "util.h"
#include "tinyos.h"
#include "kernel_sched_queue.h"

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

/** @brief Thread type. 
	
	The following types of threads are used
	- **idle threads** are threads which are not created by @c spawn_thread(). They
	  are the initial threads of the scheduler, and they are always @c READY.
	- **kernel threads** are threads that do not belong to a process. They are
	  used by drivers to perform asynchronous tasks
	- **normal threads** are threads that belong to processes
 */
typedef enum {
	IDLE_THREAD, /**< @brief Marks an idle thread. */
	KERNEL_THREAD, /**< @brief Marks a kernel thread */
	NORMAL_THREAD /**< @brief Marks a normal thread */
} Thread_type;

/**
  @brief Designate different origins of scheduler invocation.

  This is used in the scheduler heuristics to determine how to
  adjust the dynamic priority of the current thread.
 */
enum SCHED_CAUSE {
	SCHED_USER,    /**< @brief User-space code called yield (e.g., CondVar) */
	SCHED_QUANTUM, /**< @brief The quantum has expired */
	SCHED_MUTEX,   /**< @brief @c Mutex_Lock yielded on contention */
	SCHED_JOIN,	   /**< @brief The thread is waiting for another thread */
	SCHED_IO,      /**< @brief The thread is waiting for I/O */
	SCHED_PIPE,    /**< @brief Sleep at a pipe or socket */
	SCHED_POLL,    /**< @brief The thread is polling a device */
	SCHED_IDLE,    /**< @brief The idle thread called yield */
	SCHED_INIT,    /**< @brief Just an initializer */	
	SCHED_EXIT     /**< @brief An exiting thread called yield */
};


struct wait_queue;

/**
  @brief The thread control block

  An object of this type is associated to every thread. In this object
  are stored all the metadata that relate to the thread.
*/
typedef struct thread_control_block {

	/* These fields are PUBLIC and can be read/changed outside the scheduler */

	PCB* owner_pcb; /**< @brief This is null for a free TCB */

	/* All these fields are private and should only be accessed by the scheduler */

	cpu_context_t context; /**< @brief The thread context */
	Thread_type type;     /**< @brief The type of thread */
	Thread_state state;   /**< @brief The state of the thread */
	Thread_phase phase;   /**< @brief The phase of the thread */

	TimerDuration wakeup_time; 	/**< @brief The absolute time this thread will be woken up 
								by the scheduler after a timeout */
	int cancel;					/**< @brief Flag to request that the thread be cancelled. */

	rlnode sched_node; 	/**< @brief Node to use when queueing in the scheduler lists */
	rlnode wqueue_node;	/**< @brief Node to use when waiting on a wait_queue */
	struct wait_queue* wqueue;  /**< @brief A pointer to a wait_queue we are waiting on, or NULL */
	int wait_signalled;			/**< @brief Set when signalled in a wait_queue */

	/* Information related to the scheduler queue but maintained by the scheduler. */

	TimerDuration its;   /**< @brief Initial time-slice for this thread. This is determined by the scheduling
								algorithm, by returning from \c sched_queue_select(). This is an **output** of
								the scheduling algorithm, fed back to it as an input.
							*/
	TimerDuration rts;   /**< @brief For a thread is \c RUNNING, this attribute contains the remaining 
	                           time-slice for this thread, at the time that the scheduler is called. This
	                           is an **input** to the scheduling algorithm. */

	enum SCHED_CAUSE curr_cause; /**< @brief The endcause for the current time-slice.
	 				This is an **input** to the scheduling algorithm. */
	enum SCHED_CAUSE last_cause; /**< @brief The endcause for the last time-slice 
					This is an **input** to the scheduling algorithm. */

	struct sched_queue_tcb q;	/**< @brief Information maintained by and for the scheduler queue */

#ifndef NVALGRIND
	/** @brief Valgrind helper for stacks. 

	  This is used to register the thread stack to the valgrind memory profiler. 
	  Valgrind needs to know which parts of memory are used as stacks, in order to return
	  meaningful error information. 

	  This field is not relevant to the TinyOS functioning and can be ignored.
	  */	
	unsigned valgrind_stack_id; 
#endif
	  
} TCB;


/** @brief Thread stack size.

  The default thread stack size in TinyOS is 128 kbytes.
 */
#define THREAD_STACK_SIZE (128 * 1024)


/*********************************
 *
 *      Wait channels and queues
 *
 *********************************/


/** @brief A wait channel contains common traits for a set of wait queues.

	Each wait queue contains a pointer to a particular wait channel. Typically,
	wait_channel objects are constant and global.
  */
typedef struct wait_channel
{
	enum SCHED_CAUSE cause;	/**< @brief the scheduling cause when waiting on this channel */
	const char* name;  		/**< @brief a name for this wchan */
} wait_channel;



/** @brief A wait queue is a collection of STOPPED threads.

	A wait queue is very similar to a condition variable. It supports three
	operations "wait", "signal" and "broadcast". 

	Each wait queue is associated with a particular wait channel, which contains
	traits for the queue.
  */
typedef struct wait_queue
{
	rlnode thread_list;			/**< @ List of threads */
	const wait_channel* wchan;  /**< @ The wait channel */
} wait_queue;


/** @brief Initialize a wait_queue 

	This call must be used to initialize every wait queue.

	@param wqueue the wait queue to initialize
	@param wchan  the wait channel for this wait queue
*/
void wqueue_init(wait_queue* wqueue, const wait_channel* wchan);


/** @brief Block the calling thread by adding it to a wait queue

	The semantics of this call are very similar to a "wait" operation for a condition
	variable.

	This call will block the calling thread, changing its state to @c STOPPED.
	Also, the mutex @c wmx, if not `NULL`, will be unlocked, atomically
	with the blocking of the thread. When the waiting thread awakes, it re-locks
	@c wmx before returning.

	In particular, what is meant by 'atomically' is that the thread state will change
	to @c STOPPED atomically with the mutex unlocking. Note that, the state of
	the current thread is @c RUNNING. 
	Therefore, no other state change (such as a wakeup, a yield, another sleep etc) 
	can happen "between" the thread's state change and the unlocking.

	The calling thread will remain blocked until either it is signalled by another thread,
	or until a timeout (if provided) expires.

	If @c timeout is other than @c NO_TIMEOUT, then the scheduler will awaken the thread 
	after a time roughly equal to the timeout (in microseconds) has passed. 
	The term "roughly equal" means "depending on the resolution of the system clock and the quantum", 
	which is currently on the order of ten milliseconds. Note that the actual time of returning from 
	@c wqueue_wait may be several quantums later, depending on the scheduler.

	@param wqueue the queue on which the caling thread will wait
	@param wmx the mutex to unlock atomically, or NULL.
	@param timeout a timeout value to wait blocked, or @c NO_TIMEOUT if we want to wait forever.

	@return 1 if the thread returned as a result of being signalled, 0 if it returned 
			because the timeout expired
*/
int wqueue_wait(wait_queue* wqueue, Mutex* wmx, TimerDuration timeout);

/** @brief Signal a waiting thread on a wait queue

	This call will signal a single waiting thread in the wait queue (if any).
	The threads are signalled in the FIFO order of entering the queue.

	@param wqueue the queue to signal
 */
void wqueue_signal(wait_queue* wqueue);

/** @brief Signal a waiting thread on a wait queue

	This call will signal all waiting threads in the wait queue (if any).

	@param wqueue the queue to signal
 */
void wqueue_broadcast(wait_queue* wqueue);


/************************
 *
 *      CPU core related
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
	char preemption; /**< @brief Marks preemption, used by the locking code */
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


/************************
 *
 *      Scheduler API
 *
 ************************/


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
  @returns 1 if the thread was blocked, 0 otherwise

*/
int wakeup(TCB* tcb);


/**
	@brief Terminate the current thread.

	This call causes the calling thread to terminate. It does not return.
  */
_Noreturn void exit_thread();


/**
  @brief Give up the CPU.

  This call asks the scheduler to terminate the quantum of the current thread
  and possibly switch to a different thread. The scheduler may decide that 
  it will renew the quantum for the current thread.

  @param cause is the cause for the yield, see @ref SCHED_CAUSE
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

/**
	@brief An object containing some information copied from a TCB.

	This object can be used to obtain some information about a thread
	bet calling function @c get_thread_info().
  */
typedef struct thread_info
{
	TCB* tcb;		/**< @brief The TCB this info object is about. */
	PCB* owner_pcb; /**< @brief This is null for a free TCB */
	Thread_type type;   /**< @brief The type of thread */
	Thread_state state; /**< @brief The state of the thread */
	const wait_channel* wchan;  /**< @brief A pointer to a wait_queue we are waiting on, or NULL */
} thread_info;


/**
	@brief Obtain some information about a thread.

	This function copies information from a given @c TCB into a @c thread_info
	object.

	@param tcb the thread whose info will be returned
	@param tinfo the @c thread_info object to be filled
*/
void get_thread_info(TCB* tcb, thread_info* tinfo);


/** @}  scheduler */

#endif
