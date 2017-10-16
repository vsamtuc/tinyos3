

#include <assert.h>

#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"


/**
	@file kernel_cc.c

	@brief The implementation for concurrency control .
  */

/*
 	Pre-emption aware mutex.
 	-------------------------

 	This mutex will act as a spinlock if preemption is off, and a
 	yielding mutex if it is on.

 */
void Mutex_Lock(Mutex* lock)
{
#define MUTEX_SPINS 1000

  while(__atomic_test_and_set(lock,__ATOMIC_ACQUIRE)) {
    int spin=MUTEX_SPINS;
    while(__atomic_load_n(lock, __ATOMIC_RELAXED)) {
      __builtin_ia32_pause();      
      if(spin>0) 
      	spin--; 
      else { 
      	spin=MUTEX_SPINS; 
      	if(get_core_preemption())
      		yield(SCHED_MUTEX); 
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
typedef struct __cv_waiter {
	rlnode node;				/* become part of a ring */
	TCB* thread;				/* thread to wait */
	sig_atomic_t signalled;		/* this is set if the thread is signalled */
	sig_atomic_t removed;		/* this is set if the waiter is removed 
								   from the ring */
} __cv_waiter;
/** \endcond */

/*
	Condition variables.	
*/

static inline void remove_from_ring(CondVar* cv, __cv_waiter* w)
{
	if(cv->waitset == w) {
		/* Make cv->waitset safe */
		__cv_waiter * nextw = w->node.next->obj;
		cv->waitset =  (nextw == w) ? NULL : nextw;
	}
	rlist_remove(& w->node);
}


int cv_wait(Mutex* mutex, CondVar* cv, 
		enum SCHED_CAUSE cause, TimerDuration timeout)
{
	__cv_waiter waiter = { .thread=CURTHREAD, .signalled = 0, .removed=0 };
	rlnode_init(& waiter.node, &waiter);

	Mutex_Lock(&(cv->waitset_lock));
	/* We just push the current thread to the back of the list */
	if(cv->waitset) {
		__cv_waiter* wset = cv->waitset;
		rlist_push_back(& wset->node, & waiter.node);
	} else {
		cv->waitset = &waiter;
	}
	/* Now atomically release mutex and sleep */
	Mutex_Unlock(mutex);

	sleep_releasing(STOPPED, &(cv->waitset_lock), cause, timeout);

	/* We must check wether we were signaled, and tidy up */
	Mutex_Lock(&(cv->waitset_lock));
	if(! waiter.removed) {
		assert(! waiter.signalled);

		/* We must remove ourselves from the ring! */
		remove_from_ring(cv, &waiter);
	}
	Mutex_Unlock(&(cv->waitset_lock));

	Mutex_Lock(mutex);
	return waiter.signalled;
}


int Cond_Wait(Mutex* mutex, CondVar* cv)
{
	return cv_wait(mutex, cv, SCHED_USER, NO_TIMEOUT);
}




/**
  @internal
  Helper for Cond_Signal and Cond_Broadcast. This method 
  will actually find a waiter to signal, if one exists. 
  Else, it leaves the cv->waitset == NULL.
 */
static inline void cv_signal(CondVar* cv)
{
	/* Wakeup first process in the waiters' queue, if it exists. */
	while(cv->waitset) {
		__cv_waiter* waiter = cv->waitset;
		remove_from_ring(cv, waiter);
		waiter->removed = 1;
		if(wakeup(waiter->thread)) {
			waiter->signalled = 1;
			return;
		}
	}
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
  while(cv->waitset) cv_signal(cv);
  Mutex_Unlock(&(cv->waitset_lock));
}





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

/**
 * @brief The kernel lock.
 *
 * This mutex is used to protect most of the resources in kernel-space (the preemptive domain
 * of the kernel). 
 */
Mutex kernel_mutex = MUTEX_INIT;          /* lock for resource tables */

/*
	We provide two implementations of kernel locking, one based on a mutex
	and one based on semaphore. 

	In timing tests, they are, more or less similar.
 */


/* 
	Semaphore-based implementation
 */


/* Semaphore counter */
int kernel_sem = 1;

/* Semaphore condition */
CondVar kernel_sem_cv = COND_INIT;

void kernel_lock()
{
	Mutex_Lock(& kernel_mutex);
	while(kernel_sem<=0) {
		Cond_Wait(& kernel_mutex, &kernel_sem_cv);
	}
	kernel_sem--;
	Mutex_Unlock(& kernel_mutex);
}

void kernel_unlock()
{
	Mutex_Lock(& kernel_mutex);
	kernel_sem++;
	Cond_Signal(&kernel_sem_cv);
	Mutex_Unlock(& kernel_mutex);
}

int kernel_wait_wchan(CondVar* cv, enum SCHED_CAUSE cause, 
	const char* wchan_name, TimerDuration timeout)
{
	/* Atomically release kernel semaphore */
	Mutex_Lock(& kernel_mutex);
	kernel_sem++;
	Cond_Signal(&kernel_sem_cv);	

	int ret = cv_wait(&kernel_mutex, cv, cause, timeout);

	/* Reacquire kernel semaphore */
	while(kernel_sem<=0)
		Cond_Wait(& kernel_mutex, &kernel_sem_cv);
	kernel_sem--;
	Mutex_Unlock(& kernel_mutex);		

	return ret;
}

void kernel_signal(CondVar* cv) 
{ 
	Cond_Signal(cv); 
}

void kernel_broadcast(CondVar* cv) 
{ 
	Cond_Broadcast(cv); 
}

void kernel_sleep(Thread_state newstate, enum SCHED_CAUSE cause)
{
	Mutex_Lock(& kernel_mutex);
	kernel_sem++;
	Cond_Signal(&kernel_sem_cv);
	sleep_releasing(newstate, &kernel_mutex, cause, NO_TIMEOUT);
}




