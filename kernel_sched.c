
#include <assert.h>
#include <sys/mman.h>

#include "tinyos.h"
#include "kernel_cc.h"
#include "kernel_sched.h"
#include "kernel_proc.h"

#ifndef NVALGRIND
#include <valgrind/valgrind.h>
#endif


/*
   The thread layout.
  --------------------

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
 */


/* 
  A counter for active threads. By "active", we mean 'existing', 
  with the exception of idle threads (they don't count).
 */
volatile unsigned int active_threads = 0;
Mutex active_threads_spinlock = MUTEX_INIT;


/* This is specific to Intel Pentium! */
#define SYSTEM_PAGE_SIZE  (1<<12)

/* The memory allocated for the TCB must be a multiple of SYSTEM_PAGE_SIZE */
#define THREAD_TCB_SIZE   (((sizeof(TCB)+SYSTEM_PAGE_SIZE-1)/SYSTEM_PAGE_SIZE)*SYSTEM_PAGE_SIZE)

#define THREAD_SIZE  (THREAD_TCB_SIZE+THREAD_STACK_SIZE)

//#define MMAPPED_THREAD_MEM 
#ifdef MMAPPED_THREAD_MEM 

/*
  Use mmap to allocate a thread. A more detailed implementation can allocate a
  "sentinel page", and change access to PROT_NONE, so that a stack overflow
  is detected as seg.fault.
 */
void free_thread(void* ptr, size_t size)
{
  CHECK(munmap(ptr, size));
}

void* allocate_thread(size_t size)
{
  void* ptr = mmap(NULL, size, 
      PROT_READ|PROT_WRITE|PROT_EXEC,  
      MAP_ANONYMOUS  | MAP_PRIVATE 
      , -1,0);
  
  CHECK((ptr==MAP_FAILED)?-1:0);

  return ptr;
}
#else
/*
  Use malloc to allocate a thread. This is probably faster than  mmap, but cannot
  be made easily to 'detect' stack overflow.
 */
void free_thread(void* ptr, size_t size)
{
  free(ptr);
}

void* allocate_thread(size_t size)
{
  void* ptr = aligned_alloc(SYSTEM_PAGE_SIZE, size);
  CHECK((ptr==NULL)?-1:0);
  return ptr;
}
#endif


/*
  Initialize the thread context. This is done in a platform-specific
  way, using the ucontext library.
*/
void initialize_context(ucontext_t* ctx, stack_t stack, void (*ctx_func)())
{
  /* Init the context from this context! */
  getcontext(ctx);
  ctx->uc_link = NULL;

  /* initialize the context stack */
  ctx->uc_stack = stack;

  pthread_sigmask(0, NULL, & ctx->uc_sigmask);  /* We don't want any signals changed */
  makecontext(ctx, (void*) ctx_func, 0);
}



/*
  This is the function that is used to start normal threads.
*/

void gain(int preempt); /* forward */

static void thread_start()
{
  gain(1);
  CURTHREAD->thread_func();

  /* We are not supposed to get here! */
  assert(0);
}


/*
  Initialize and return a new TCB
*/

TCB* spawn_thread(PCB* pcb, void (*func)())
{
  /* The allocated thread size must be a multiple of page size */
  TCB* tcb = (TCB*) allocate_thread(THREAD_SIZE);

  /* Set the owner */
  tcb->owner_pcb = pcb;

  /* Initialize the other attributes */
  tcb->type = NORMAL_THREAD;
  tcb->state = INIT;
  tcb->phase = CTX_CLEAN;
  tcb->state_spinlock = MUTEX_INIT;
  tcb->thread_func = func;
  rlnode_init(& tcb->sched_node, tcb);  /* Intrusive list node */


  /* Prepare the stack */
  stack_t stack = {
    .ss_sp = ((void*)tcb) + THREAD_TCB_SIZE,
    .ss_size = THREAD_STACK_SIZE,
    .ss_flags = 0
  };

  /* Init the context */
  initialize_context(& tcb->context, stack, thread_start);

#ifndef NVALGRIND
  tcb->valgrind_stack_id = 
    VALGRIND_STACK_REGISTER(stack.ss_sp, stack.ss_sp+THREAD_STACK_SIZE);
#endif

  /* increase the count of active threads */
  Mutex_Lock(&active_threads_spinlock);
  active_threads++;
  Mutex_Unlock(&active_threads_spinlock);
 
  return tcb;
}


/*
  This is called with tcb->state_spinlock locked !
 */
void release_TCB(TCB* tcb)
{
#ifndef NVALGRIND
  VALGRIND_STACK_DEREGISTER(tcb->valgrind_stack_id);    
#endif

  free_thread(tcb, THREAD_SIZE);

  Mutex_Lock(&active_threads_spinlock);
  active_threads--;
  Mutex_Unlock(&active_threads_spinlock);
}


/*
 *
 * Scheduler
 *
 */


/*
 *  Note: the scheduler routines are all in the non-preemptive domain.
 */


/* Core control blocks */
CCB cctx[MAX_CORES];


/*
  The scheduler queue is implemented as a doubly linked list. The
  head and tail of this list are stored in  SCHED.
*/


rlnode SCHED;                         /* The scheduler queue */
Mutex sched_spinlock = MUTEX_INIT;    /* spinlock for scheduler queue */


/* Interrupt handler for ALARM */
void yield_handler()
{
  yield();
}

/* Interrupt handle for inter-core interrupts */
void ici_handler() 
{
  /* noop for now... */
}



/*
  Add PCB to the end of the scheduler list.
*/
void sched_queue_add(TCB* tcb)
{
  /* Insert at the end of the scheduling list */
  Mutex_Lock(& sched_spinlock);
  rlist_push_back(& SCHED, & tcb->sched_node);
  Mutex_Unlock(& sched_spinlock);

  /* Restart possibly halted cores */
  cpu_core_restart_one();
}


/*
  Remove the head of the scheduler list, if any, and
  return it. Return NULL if the list is empty.
*/
TCB* sched_queue_select()
{
  Mutex_Lock(& sched_spinlock);
  rlnode * sel = rlist_pop_front(& SCHED);
  Mutex_Unlock(& sched_spinlock);

  return sel->tcb;  /* When the list is empty, this is NULL */
} 


/*
  Make the process ready. 
 */
void wakeup(TCB* tcb)
{
  /* Preemption off */
  int oldpre = preempt_off;

  /* To touch tcb->state, we must get the spinlock. */
  Mutex_Lock(& tcb->state_spinlock);
  assert(tcb->state==STOPPED || tcb->state==INIT); 

  tcb->state = READY;

  /* Possibly add to the scheduler queue */
  if(tcb->phase == CTX_CLEAN) 
    sched_queue_add(tcb);

  Mutex_Unlock(& tcb->state_spinlock);

  /* Restore preemption state */
  if(oldpre) preempt_on;
}


/*
  Atomically put the current process to sleep, after unlocking mx.
 */
void sleep_releasing(Thread_state state, Mutex* mx)
{
  assert(state==STOPPED || state==EXITED);

  TCB* tcb = CURTHREAD;
  
  /* 
    The tcb->state_spinlock guarantees atomic sleep-and-release.
    But, to access it safely, we need to go into the non-preemptive
    domain.
   */
  int preempt = preempt_off;
  Mutex_Lock(& tcb->state_spinlock);

  /* mark the process as stopped */
  tcb->state = state;

  /* Release mx */
  if(mx!=NULL) Mutex_Unlock(mx);

  Mutex_Unlock(& tcb->state_spinlock);
  
  /* call this to schedule someone else */
  yield();

  /* Restore preemption state */
  if(preempt) preempt_on;
}


/* This function is the entry point to the scheduler's context switching */

void yield()
{ 
  /* Reset the timer, so that we are not interrupted by ALARM */
  bios_cancel_timer();

  /* We must stop preemption but save it! */
  int preempt = preempt_off;

  TCB* current = CURTHREAD;  /* Make a local copy of current process, for speed */

  int current_ready = 0;

  Mutex_Lock(& current->state_spinlock);
  switch(current->state)
  {
    case RUNNING:
      current->state = READY;
    case READY: /* We were awakened before we managed to sleep! */
      current_ready = 1;
      break;

    case STOPPED:
    case EXITED:
      break; 

    default:
      fprintf(stderr, "BAD STATE for current thread %p in yield: %d\n", current, current->state);
      assert(0);  /* It should not be READY or EXITED ! */
  }
  Mutex_Unlock(& current->state_spinlock);

  /* Get next */
  TCB* next = sched_queue_select();

  /* Maybe there was nothing ready in the scheduler queue ? */
  if(next==NULL) {
    if(current_ready)
      next = current;
    else
      next = & CURCORE.idle_thread;
  }

  /* ok, link the current and next TCB, for the gain phase */
  current->next = next;
  next->prev = current;

  /* Switch contexts */
  if(current!=next) {
    CURTHREAD = next;
    swapcontext( & current->context , & next->context );
  }

  /* This is where we get after we are switched back on! A long time 
     may have passed. Start a new timeslice... 
   */
  gain(preempt);
}


/*
  This function must be called at the beginning of each new timeslice.
  This is done mostly from inside yield(). 
  However, for threads that are executed for the first time, this 
  has to happen in thread_start.

  The 'preempt' argument determines whether preemption is turned on
  in the new timeslice. When returning to threads in the non-preemptive
  domain (e.g., waiting at some driver), we need not to turn preemption
  on!
*/

void gain(int preempt)
{
  TCB* current = CURTHREAD; 
  TCB* prev = current->prev;

  /* Mark current state */
  Mutex_Lock(& current->state_spinlock);
  current->state = RUNNING;
  current->phase = CTX_DIRTY;
  Mutex_Unlock(& current->state_spinlock);

  /* Take care of the previous thread */
  if(current != prev) {
    int prev_exit = 0;
    Mutex_Lock(& prev->state_spinlock);
    prev->phase = CTX_CLEAN;
    switch(prev->state) 
    {
      case READY:
        if(prev->type != IDLE_THREAD) sched_queue_add(prev);
        break;
      case EXITED:
        prev_exit = 1; /* We cannot release here, because of the mutex */
        break;
      case STOPPED:
        break;

      default:
        fprintf(stderr, "BAD STATE for current thread %p in gain: %d\n", current, current->state);
        assert(0);  /* It should not be READY or EXITED ! */
    }
    Mutex_Unlock(& prev->state_spinlock);
    if(prev_exit) release_TCB(prev);
  }

  /* Reset preemption as needed */
  if(preempt) preempt_on;

  /* Set a 1-quantum alarm */
  bios_set_timer(QUANTUM);
}


static void idle_thread()
{
  /* When we first start the idle thread */
  yield();

  /* We come here whenever we cannot find a ready thread for our core */
  while(active_threads>0) {
    cpu_core_halt();
    yield();
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
  rlnode_init(&SCHED, NULL);
}



void run_scheduler()
{
  CCB * curcore = & CURCORE;

  /* Initialize current CCB */
  curcore->id = cpu_core_id;

  curcore->current_thread = & curcore->idle_thread;

  curcore->idle_thread.owner_pcb = get_pcb(0);
  curcore->idle_thread.type = IDLE_THREAD;
  curcore->idle_thread.state = RUNNING;
  curcore->idle_thread.phase = CTX_DIRTY;
  curcore->idle_thread.state_spinlock = MUTEX_INIT;
  rlnode_init(& curcore->idle_thread.sched_node, & curcore->idle_thread);

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


