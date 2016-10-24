

#include <assert.h>

#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"


/**
	@file kernel_cc.c

	@brief The implementation for concurrency control .
  */




/*
 *  Pre-emption control
 */ 
int set_core_preemption(int preempt)
{
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



/*  Locks for scheduler and device drivers. Because we support 
 *  multiple cores, we need to avoid race conditions
 *  with an interrupt handler on the same core, and also to
 *  avoid race conditions between cores.
 */

/*
 *
 * The kernel locks
 *
 */

Mutex kernel_mutex = MUTEX_INIT;          /* lock for resource tables */



/*
 	Pre-emption aware mutex.
 	-------------------------

 	This mutex will act as a spinlock if preemption is off, and a
 	yielding mutex if it is on.

 */
void Mutex_Lock(Mutex* lock)
{
#define MUTEX_SPINS 1000

  int spin=MUTEX_SPINS;
  while(__atomic_test_and_set(lock,__ATOMIC_ACQUIRE)) {
    while(__atomic_load_n(lock, __ATOMIC_RELAXED)) {
      __builtin_ia32_pause();      
      if(spin>0) 
      	spin--; 
      else { 
      	spin=MUTEX_SPINS; 
      	if(get_core_preemption())
      		yield(); 
      }
    }
  }
#undef MUTEX_SPINS
}


void Mutex_Unlock(Mutex* lock)
{
  __atomic_clear(lock, __ATOMIC_RELEASE);
}



/** \cond HELPER Helper structure for condition variables. */
typedef struct __cv_waitset_node {
  void* thread;
  struct __cv_waitset_node* next;
} __cv_waitset_node;
/** \endcond */



/*
	Condition variables.	
*/

int Cond_Wait(Mutex* mutex, CondVar* cv)
{
  __cv_waitset_node newnode;
  
  newnode.thread = CURTHREAD;

  Mutex_Lock(&(cv->waitset_lock));

  /* We just push the current thread to the head of the list */
  newnode.next = cv->waitset;
  cv->waitset = &newnode;

  /* Now atomically release mutex and sleep */
  Mutex_Unlock(mutex);
  sleep_releasing(STOPPED, &(cv->waitset_lock));

  /* Re-lock mutex before returning */
  Mutex_Lock(mutex);

  return 1;
}



/**
  @internal
  Helper for Cond_Signal and Cond_Broadcast
 */
static __cv_waitset_node* cv_signal(CondVar* cv)
{
  /* Wakeup first process in the waiters' queue, if it exists. */
  if(cv->waitset != NULL) {
    __cv_waitset_node *node = cv->waitset;
    cv->waitset = node->next;
    wakeup(node->thread);
  }
  return cv->waitset;
}



void Cond_Signal(CondVar* cv)
{
  Mutex_Lock(&(cv->waitset_lock));
  cv_signal(cv);
  Mutex_Unlock(&(cv->waitset_lock));
}


void Cond_Broadcast(CondVar* cv)
{
  Mutex_Lock(&(cv->waitset_lock));
  while( cv_signal(cv) )  /*loop*/;
  Mutex_Unlock(&(cv->waitset_lock));
}



