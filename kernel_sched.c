
#include <assert.h>
#include <sys/mman.h>

#include "tinyos.h"
#include "kernel_cc.h"
#include "kernel_proc.h"
#include "kernel_sched.h"
#include "kernel_sig.h"

#ifndef NVALGRIND
#include <valgrind/valgrind.h>
#endif

/* Forward declarations */

static void sched_gain();
static void sched_wakeup(TCB*);


/*===========================================================

	Core preemption support and scheduler protection
	-------------------------------------------------
	
	The following routines switch the preemption of a core off (disable interrupts)
	and on.	

============================================================ */ 

/* Core control blocks */
CCB cctx[MAX_CORES];



int set_core_preemption(int preempt)
{
	/* 
	   Just ensure that CURCORE.preemption is changed only with preemption off! 
	   That is, after cpu_disable_interrupts() and before cpu_enable_interrupts().
	 */
	sig_atomic_t old_preempt;
	if(preempt) {
		old_preempt = __atomic_exchange_n(& CURCORE.preemption, preempt, __ATOMIC_RELAXED);
		cpu_enable_interrupts();
	} 
	else {				
		cpu_disable_interrupts();
		old_preempt = __atomic_exchange_n(& CURCORE.preemption, preempt, __ATOMIC_RELAXED);
	}

	return old_preempt;
}


int get_core_preemption()
{
	return CURCORE.preemption;
}


/*===========================================================

	Scheduler concurrency control
	-----------------------------

	The scheduler spinlock protects all scheduler data, i.e.:
	- the scheduler queue
	- most of the fields of the TCB (owner_pcb being an exception)
	- the wait queue data
	- ...

	Because there are several entry points into the scheduler,
	we define the routines (lock/unlock)_scheduler, which work
	as a recursive mutex.

============================================================ */ 

 /* spinlock for scheduler queue */
Mutex sched_spinlock = MUTEX_INIT;

static inline void sched_lock()
{
	spin_lock(&sched_spinlock);
}

static inline void sched_unlock()
{
	spin_unlock(&sched_spinlock);
}


/*============================================================================
	Thread creation   
  --------------------


  ## The thread layout.

  On the x86 (Pentium) architecture, the stack grows upward. Therefore, we
  can allocate the TCB at the top of the memory block used as the stack.

  +-------------+
  |   TCB       |
  +-------------+
  |             |
  |    stack    |
  |             |
  |      ^      |
  |      |      |
  +-------------+
  | first frame |
  +-------------+

  Advantages: (a) unified memory area for stack and TCB (b) stack overrun will
  crash own thread, before it affects other threads (which may make debugging
  easier).

  Disadvantages: The stack cannot grow unless we move the whole TCB. Of course,
  we do not support stack growth anyway!
============================================================================== */

/*
  A counter for active threads. By "active", we mean 'existing',
  with the exception of idle threads (they don't count).
 */
volatile unsigned int active_threads = 0;
Mutex active_threads_spinlock = MUTEX_INIT;

/* The memory allocated for the TCB must be a multiple of SYSTEM_PAGE_SIZE */
#define THREAD_TCB_SIZE \
	(((sizeof(TCB) + SYSTEM_PAGE_SIZE - 1) / SYSTEM_PAGE_SIZE) * SYSTEM_PAGE_SIZE)

#define THREAD_SIZE (THREAD_TCB_SIZE + THREAD_STACK_SIZE)

/*
  Use malloc to allocate a thread. This is probably faster than  mmap, but
  cannot be made easily to 'detect' stack overflow.
 */
void* allocate_thread(size_t size)
{
	void* ptr = aligned_alloc(SYSTEM_PAGE_SIZE, size);
	CHECK((ptr == NULL) ? -1 : 0);
	return ptr;
}


void free_thread(void* ptr, size_t size) { free(ptr); }


/*
  This is the function that is executed by a thread the first time it is
  scheduled.
*/
static _Noreturn void thread_start()
{
	/* Exit the kernel to user land */
	sched_gain();
	sched_unlock();
	preempt_on;

	/* Execute the function */
	CURTHREAD->thread_func();

	/* If we belong to a process, we are not supposed to get here! */
	assert(CURTHREAD->type != NORMAL_THREAD);
	exit_thread();
}


/*
	Initialize all fields of a TCB, except the context.
	This is called both from spawn_thread() and from 
	initialize_scheduler(), to init the idle threads
 */
void initialize_TCB(TCB* tcb, PCB* pcb, Thread_type type)
{
	/* Set the passed attributes */
	tcb->owner_pcb = pcb;
	tcb->type = type;

	/* Init other attributes */
	tcb->wakeup_time = NO_TIMEOUT;
	tcb->cancel = 0;
	rlnode_init(&tcb->sched_node, tcb);
	rlnode_init(& tcb->wqueue_node, tcb);
	tcb->wqueue = NULL;
	tcb->wait_signalled = 0;
	tcb->its = QUANTUM;
	tcb->rts = QUANTUM;
	tcb->last_cause = SCHED_IDLE;
	tcb->curr_cause = SCHED_IDLE;
	
	/* Init the state */
	if(type==IDLE_THREAD) {
		tcb->state = RUNNING;
		tcb->phase = CTX_DIRTY;
	} else {
		tcb->state = INIT;
		tcb->phase = CTX_CLEAN;
	}

	sched_queue_init_tcb(tcb);
}


/*
	Allocate, initialize and return a new TCB, ready to be executed
	after a call to wakeup().
*/
TCB* spawn_thread(PCB* pcb, void (*func)())
{
	/* The allocated thread size must be a multiple of page size */
	TCB* tcb = (TCB*)allocate_thread(THREAD_SIZE);

	/* Initialize attributes */
	initialize_TCB(tcb, pcb, NORMAL_THREAD);

	/* Compute the stack segment address and size */
	void* sp = ((void*)tcb) + THREAD_TCB_SIZE;

	/* Init the context */
	tcb->thread_func = func;
	cpu_initialize_context(&tcb->context, sp, THREAD_STACK_SIZE, thread_start);

	tcb->ss_sp = sp;
	tcb->ss_size = THREAD_STACK_SIZE;

#ifndef NVALGRIND
	tcb->valgrind_stack_id = VALGRIND_STACK_REGISTER(sp, sp + THREAD_STACK_SIZE);
#endif

	/* increase the count of active threads */
	spin_lock(&active_threads_spinlock);
	active_threads++;
	spin_unlock(&active_threads_spinlock);

	return tcb;
}

/*
  This is called with sched_spinlock locked !
 */
void release_thread(TCB* tcb)
{
#ifndef NVALGRIND
	VALGRIND_STACK_DEREGISTER(tcb->valgrind_stack_id);
#endif

	free_thread(tcb, THREAD_SIZE);
	spin_lock(&active_threads_spinlock);
	active_threads--;
	spin_unlock(&active_threads_spinlock);
}




/*=================================================================
  Scheduler timeout list
 
  The scheduler maintains a linked list of all the sleeping
  threads with a timeout, sorted by wakeup_time.

  This list is updated once each time the scheduler is called.
 =================================================================*/


rlnode TIMEOUT_LIST; /* The list of threads with a timeout */

/*
    Scan timeout list and wake up all threads whose timeout is less 
    than or equal to curtime. 
    Possibly add tcb to timeout list.

    *** MUST BE CALLED WITH sched_spinlock HELD ***
*/
static void sched_update_timeouts(TCB* curtcb, TimerDuration timeout)
{
	TimerDuration curtime = bios_clock();

	/* Wake up threads with expired timeouts */
	while (!is_rlist_empty(&TIMEOUT_LIST)) {
		TCB* tcb = TIMEOUT_LIST.next->tcb;
		if (tcb->wakeup_time > curtime)
			break;
		sched_wakeup(tcb);
	}	

	if (timeout != NO_TIMEOUT) {
		/* set the wakeup time */
		curtcb->wakeup_time = curtime + timeout;

		/* add to the TIMEOUT_LIST in sorted order */
		rlnode* n = TIMEOUT_LIST.next;
		for (; n != &TIMEOUT_LIST; n = n->next)
			/* skip earlier entries */
			if (curtcb->wakeup_time < n->tcb->wakeup_time)
				break;
		/* insert before n */
		rl_splice(n->prev, &curtcb->sched_node);
	}
}




/*===========================================================

	Interior scheduler API
	-------------------------------
	
	sched_wakeup()
	sched_yield()
	sched_gain()

	These methods must only be called with preemption off
	and the scheduler spinlock held. They contain the
	actual implementation of the public scheduler API

============================================================ */ 

static inline void sched_enqueue(TCB* tcb)
{
	/* Add to queue */
	sched_queue_add(tcb);

	/* Restart halted core (if any) */
	cpu_core_restart_one();	
}


/*
	Adjust the state of a thread to make it READY.
 */
static void sched_wakeup(TCB* tcb)
{
	assert(tcb->state == STOPPED || tcb->state == INIT);

	/* Possibly remove from TIMEOUT_LIST */
	if (tcb->wakeup_time != NO_TIMEOUT) {
		/* tcb is in TIMEOUT_LIST, remove it */
		assert(tcb->sched_node.next != &(tcb->sched_node) && tcb->state == STOPPED);
		rlist_remove(&tcb->sched_node);
		tcb->wakeup_time = NO_TIMEOUT;
	}

	/* Possibly remove from wqueue */
	if(tcb->wqueue != NULL) {
		rlist_remove(& tcb->wqueue_node);
		tcb->wqueue = NULL;
	}

	/* Mark as ready */
	tcb->state = READY;

	/* Possibly add to the scheduler queue */
	if (tcb->phase == CTX_CLEAN)
		sched_enqueue(tcb);
}


/* This function is the entry point to the scheduler's context switching */
void sched_yield(Thread_state newstate, enum SCHED_CAUSE cause, TimerDuration timeout)
{
	/* Reset the timer, so that we are not interrupted by ALARM */
	TimerDuration remaining = bios_cancel_timer();

	TCB* current = CURTHREAD; /* Make a local copy of current process, for speed */

	/* Update CURTHREAD state */
	current->state = newstate;

	/* Update CURTHREAD scheduler data */
	current->rts = remaining;
	current->last_cause = current->curr_cause;
	current->curr_cause = cause;

	/* Wake up threads whose sleep timeout has expired, possibly add current to list */
	sched_update_timeouts(current, timeout);

	/* Get next */
	TCB* next = sched_queue_select(current);
	assert(next != NULL);

	/* Save the current TCB for the gain phase */
	CURCORE.previous_thread = current;

	/* Switch contexts */
	if (current != next) {
		CURTHREAD = next;
		cpu_swap_context(&current->context, &next->context);
	}

	/* This is where we get after we are switched back on! A long time
	   may have passed. Start a new timeslice...
	  */
	sched_gain();
}


/*
  This function must be called at the beginning of each new timeslice.
  This is done from inside sched_yield(). In addition, this has to happen in thread_start(),
  To ensure this for the first timeslice of a new thread.
*/
void sched_gain()
{
	TCB* current = CURTHREAD;

	/* Mark current state */
	current->state = RUNNING;
	current->phase = CTX_DIRTY;
	current->rts = current->its;

	/* Take care of the previous thread */
	TCB* prev = CURCORE.previous_thread;
	if (current != prev) {
		prev->phase = CTX_CLEAN;

		if(prev->state==READY && prev->type != IDLE_THREAD)
			sched_enqueue(prev);
		else if(prev->state==EXITED)
			release_thread(prev);
	}

	/* Set a 1-quantum alarm */
	bios_set_timer(current->rts);
}




/*===========================================================

	Public scheduler API
	-------------------------------
	
	wakeup()
	yield()
	exit_thread()

	These methods must only be called from outside the 
	scheduler, with the scheduler spinlock free. Preemption
	may be on or off at call time, and remains unchanged 
	at return.

============================================================ */ 



/*
  Make the process ready.
 */
int wakeup(TCB* tcb)
{
	int was_blocked = 0;

	/* Preemption off */
	int oldpre = preempt_off;
	sched_lock();

	if (tcb->state == STOPPED || tcb->state == INIT) {
		sched_wakeup(tcb);
		was_blocked = 1;
	}

	sched_unlock();
	if (oldpre) preempt_on;

	return was_blocked;
}


_Noreturn void exit_thread()
{
	assert(CURTHREAD->type != IDLE_THREAD); /* Idle threads don't exit! */

	preempt_off;
	sched_lock();

	/* call this to schedule someone else */
	sched_yield(EXITED, SCHED_EXIT, NO_TIMEOUT);

	/* We should not get here! */
	assert(0);	
	abort();
}


void yield(enum SCHED_CAUSE cause)
{
	/* Stop preemption  */
	int preempt = preempt_off;
	sched_lock();

	sched_yield(READY, cause, NO_TIMEOUT);
	
	sched_unlock();	
	if (preempt) preempt_on;
}



/*===========================================================

	Wait queues
	-------------------------------
	
============================================================ */ 



void wqueue_init(wait_queue* wqueue, const wait_channel* wchan) 
{
	rlnode_init(& wqueue->thread_list, NULL);
	wqueue->wchan = wchan;
}


int wqueue_wait(wait_queue* wqueue, Mutex* wmx, TimerDuration timeout)
{
	TCB* current = CURTHREAD;
	assert(current->type != IDLE_THREAD);

	int signalled;
	int preempt = preempt_off;
    sched_lock();

	/* Update the wait data in the TCB */
	assert(current->wqueue==NULL);
	current->wqueue = wqueue;
	current->wait_signalled = 0;
	rlist_push_back(& wqueue->thread_list, &current->wqueue_node);

	/* Release wmx */
	if (wmx != NULL)  spin_unlock(wmx);

	/* call this to schedule someone else */
	sched_yield(STOPPED, wqueue->wchan->cause, timeout);

	/* Return the wait_signalled flag */
	signalled = current->wait_signalled;

	sched_unlock();
	if(preempt) preempt_on;

	/* Reacquire mutex */
	if(wmx) spin_lock(wmx);

	return signalled;
}


void wqueue_signal(wait_queue* wqueue)
{
	int preempt = preempt_off;
	sched_lock();

	if(! is_rlist_empty(& wqueue->thread_list )) {
		TCB* head = wqueue->thread_list.next->tcb;
		assert(head->wqueue == wqueue);
		head->wait_signalled = 1;
		sched_wakeup(head);
	}

	sched_unlock();
	if(preempt) preempt_on;
}


void wqueue_broadcast(wait_queue* wqueue)
{
	int preempt = preempt_off;
	sched_lock();

	while(! is_rlist_empty(& wqueue->thread_list )) {
		TCB* head = wqueue->thread_list.next->tcb;
		assert(head->wqueue == wqueue);
		head->wait_signalled = 1;
		sched_wakeup(head);
	}

	sched_unlock();
	if(preempt) preempt_on;
}



/*===========================================================

	Initialization and idle thread
	-------------------------------
	Each core has a thread that is executed to idle the core.

	- initialize_scheduler() is called by core 0 to 
	  initialize globals at boot time.
	
	- run_scheduler() is called by every core, to enter the
	  scheduler
	
============================================================ */ 

/* Interrupt handler for ALARM */
void yield_handler() 
{
	yield(SCHED_QUANTUM); 
	if(get_core_preemption()==1) check_sigs();
}

/* Interrupt handle for inter-core interrupts */
void ici_handler()
{ /* noop for now... */
	if(get_core_preemption()==1) check_sigs();
}


static void idle_thread()
{
	/* When we first start the idle thread */
	yield(SCHED_IDLE);

	/* We come here whenever we cannot find a ready thread for our core */
	while (active_threads > 0) {
		cpu_core_halt();
		yield(SCHED_IDLE);
	}

	/* If the idle thread exits here, we are leaving the scheduler! */
	bios_cancel_timer();
	cpu_core_restart_all();
}

/*
  Initialize the scheduler queue
 */
void initialize_scheduler()
{
	/* Innitialize CCB */	
	for(uint c=0; c<cpu_cores(); c++) {
		cctx[c].id = c;
		initialize_TCB(&cctx[c].idle_thread, get_pcb(0), IDLE_THREAD);
		cctx[c].current_thread = & cctx[c].idle_thread;
	}

	/* Initialize scheduler and timeout queue */
	initialize_sched_queue();
	rlnode_init(&TIMEOUT_LIST, NULL);
}

/*
	This is called by every core, to enter the 
	scheduler.
 */
void run_scheduler()
{
	/* Initialize interrupt handler */
	cpu_interrupt_handler(ALARM, yield_handler);
	cpu_interrupt_handler(ICI, ici_handler);

	/* Run idle thread */
	preempt_on;
	idle_thread();

	/* Finished scheduling */
	assert(CURTHREAD == &CURCORE.idle_thread);
	cpu_interrupt_handler(ALARM, NULL);
	cpu_interrupt_handler(ICI, NULL);
}



/*
	Save some info into a thread_info object
 */
void get_thread_info(TCB* tcb, thread_info* tinfo)
{
	assert(tcb != NULL);
	assert(tinfo != NULL);

	int preempt = preempt_off;
	sched_lock();

	tinfo->tcb = tcb;
	tinfo->owner_pcb = tcb->owner_pcb;
	tinfo->type = tcb->type;
	tinfo->state = tcb->state;
	tinfo->wchan = (tcb->wqueue) ? tcb->wqueue->wchan : NULL;

	sched_unlock();
	if(preempt) preempt_on;
}

