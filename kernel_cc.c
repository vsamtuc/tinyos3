

#include <assert.h>

#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"


/**
	@file kernel_cc.c

	@brief The implementation for concurrency control .

	Locks for scheduler and device drivers. Because we support 
    multiple cores, we need to avoid race conditions
    with an interrupt handler on the same core, and also to
    avoid race conditions between cores.
  */


/*
 	Pre-emption aware mutex.
 	-------------------------

 	This mutex will act as a spinlock if preemption is off, and a
 	yielding mutex if preemption is on.

 	Therefore, we can call the same function from both the preemptive and
 	the non-preemptive domain of the kernel.

 	The implementation is based on GCC atomics, as the standard C11 primitives
 	are not supported by all recent compilers. Eventually, this will change.
 */
void Mutex_Lock(Mutex* lock)
{
#define MUTEX_SPINS (cpu_cores()>1 ?  1000 : 10000)

  while(__atomic_test_and_set(lock,__ATOMIC_ACQUIRE)) {
    int spin=MUTEX_SPINS;
    while(__atomic_load_n(lock, __ATOMIC_RELAXED)) {
#if defined(__x86__) || defined(__x86_64__)
      __builtin_ia32_pause();
#endif
      if(spin>0) 
      	spin--; 
      else { 
      	spin=MUTEX_SPINS; 
      	if(cpu_interrupts_enabled())
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


/*
	Condition variables.	
*/


/** \cond HELPER Helper structure for condition variables. */
typedef struct __cv_waiter {
	rlnode node;				/* become part of a ring */
	TCB* thread;				/* thread to wait */
	sig_atomic_t signalled;		/* this is set if the thread is signalled */
	sig_atomic_t removed;		/* this is set if the waiter is removed 
								   from the ring */
} __cv_waiter;
/** \endcond */

/**
   @internal
   A helper routine to remove a condition waiter from the CondVar ring.
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


/** 
   @internal
   @brief Wait on a condition variable, specifying the cause. 

	This function is the basic implementation for the 'wait' operation on
	condition variables. It is used to implement the @c Cond_Wait and @c Cond_TimedWait
	system calls, as well as internal kernel 'wait' functionality.

  The function must be called only while we have locked the mutex that 
  is associated with this call. It will put the calling thread to sleep, 
  unlocking the mutex. These operations happen atomically.  

  When the thread is woken up later (by another thread that calls @c 
  Cond_Signal or @c Cond_Broadcast, or because the timeout has expired, or
  because the thread was awoken by another kernel routine), 
  it first re-locks the mutex and then returns.  

  @param mx The mutex to be unlocked as the thread sleeps.
  @param cv The condition variable to sleep on.
  @param cause A cause provided to the kernel scheduler.
  @param timeout The time to sleep, or @c NO_TIMEOUT to sleep for ever.

  @returns 1 if this thread was woken up by signal/broadcast, 0 otherwise

  @see Cond_Signal
  @see Cond_Broadcast
  */
static int cv_wait(Mutex* mutex, CondVar* cv, 
		enum SCHED_CAUSE cause, TimerDuration timeout)
{
	__cv_waiter waiter = { .thread=cur_thread(), .signalled = 0, .removed=0 };
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

	/* Woke up, we must check wether we were signaled, and tidy up */
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



int Cond_Wait(Mutex* mutex, CondVar* cv)
{
	return cv_wait(mutex, cv, SCHED_USER, NO_TIMEOUT);
}

int Cond_TimedWait(Mutex* mutex, CondVar* cv, timeout_t timeout)
{
	/* We have to translate timeout from msec to usec */
	return cv_wait(mutex, cv, SCHED_USER, timeout*1000ul);
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
 *
 * The kernel locks
 *
 */

/**
 * @brief The kernel lock.
 *
 * Kernel locking is provided by a semaphore, implemented as a monitor.
 * A semaphre for kernel locking has advantages over a simple mutex. 
 * The main advantage is that @c kernel_mutex is held for a very short time
 * regardless of contention. Thus, in multicore machines, it allows for cores
 * to be passed to other threads. 
 * 
 */

/* This mutex is used to implement the kernel semaphore as a monitor. */
static Mutex kernel_mutex = MUTEX_INIT;

/* Semaphore counter */
static int kernel_sem = 1;

/* Semaphore condition */
static CondVar kernel_sem_cv = COND_INIT;

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




