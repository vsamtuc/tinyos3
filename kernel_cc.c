

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


/************************
 *	Condition variables.
 ************************

	The semantics of condition variables are almost identical to those
	of wait_queue, so the implementation is straightforward.

 */


/* A WCHAN for condition variable queues */
static const wait_channel wchan_cv_wait = { SCHED_USER, "cv_wait" };

/* Lazy initialization of a CondVar */
static inline void cv_ensure_init(CondVar* cv) {
	assert(sizeof(CondVar)==sizeof(wait_queue));
	if(cv->filler[3]==NULL) {
		wqueue_init((wait_queue*) cv, &wchan_cv_wait);
	}
}


int Cond_Wait(Mutex* mutex, CondVar* cv)
{
	cv_ensure_init(cv);
	return wqueue_wait((wait_queue*) cv, mutex, NO_TIMEOUT);
}

int Cond_TimedWait(Mutex* mutex, CondVar* cv, timeout_t timeout)
{
	cv_ensure_init(cv);
	return wqueue_wait((wait_queue*) cv, mutex, timeout*1000ul);
}


void Cond_Signal(CondVar* cv)
{
	cv_ensure_init(cv);
	wqueue_signal((wait_queue*) cv);
}


void Cond_Broadcast(CondVar* cv)
{
	cv_ensure_init(cv);
	wqueue_broadcast((wait_queue*) cv);
}


/*
 *  Pre-emption control
 */ 
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
//static CondVar kernel_sem_cv = COND_INIT;
static wait_channel ksem_wchan = { SCHED_USER, "klock" };
static wait_queue ksem_queue = { RLIST(ksem_queue.thread_list), &ksem_wchan };

void kernel_lock()
{
	Mutex_Lock(& kernel_mutex);
	while(kernel_sem<=0) {
		wqueue_wait(&ksem_queue, &kernel_mutex, NO_TIMEOUT);
	}
	kernel_sem--;
	Mutex_Unlock(& kernel_mutex);
}

void kernel_unlock()
{
	Mutex_Lock(& kernel_mutex);
	kernel_sem++;
	wqueue_signal(&ksem_queue);
	Mutex_Unlock(& kernel_mutex);
}

int kernel_timedwait(wait_queue* wq, TimerDuration timeout)
{
	/* Atomically release kernel semaphore */
	int preempt = preempt_on;
	Mutex_Lock(& kernel_mutex);
	kernel_sem++;
	wqueue_signal(&ksem_queue);

	/* Enter the wait queue */
	int signalled = wqueue_wait(wq, &kernel_mutex, timeout);

	/* Reacquire kernel semaphore */
	while(kernel_sem<=0) wqueue_wait(&ksem_queue, &kernel_mutex, NO_TIMEOUT); 
	kernel_sem--;
	Mutex_Unlock(& kernel_mutex);
	if(!preempt) preempt_off;

	return signalled;
}

void kernel_signal(wait_queue* wq) 
{ 
	wqueue_signal(wq); 
}

void kernel_broadcast(wait_queue* wq) 
{ 
	wqueue_broadcast(wq); 
}




