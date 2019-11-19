
#include "kernel_sched.h"


/*=================================================================
 *
 * Scheduler queue
 *
 =================================================================*/

/*
  The scheduler queue is implemented as a doubly linked list. The
  head and tail of this list are stored in  SCHED.
*/

static rlnode SCHED; /* The scheduler queue */


void initialize_sched_queue()
{
	rlnode_init(&SCHED, NULL);
}


void sched_queue_init_tcb(TCB* tcb)
{
	/* nothing to do for round-robin */
}


/*
  Add TCB to the end of the scheduler list.

  *** MUST BE CALLED WITH sched_spinlock HELD ***
*/
void sched_queue_add(TCB* tcb)
{
	/* Insert at the end of the scheduling list */
	rlist_push_back(&SCHED, &tcb->sched_node);
}


/*
  If the queue is not empty remove the head and select it. 
  If the queue is empty, and current is READY, select current.
  Else, select the idle thread.

  Initialize the its field and return it.

  *** MUST BE CALLED WITH sched_spinlock HELD ***
*/
TCB* sched_queue_select(TCB* current)
{
	TCB* next_thread;

	if(is_rlist_empty(&SCHED)) {
		next_thread = (current->state == READY) ? current : &CURCORE.idle_thread;
	} else {
		/* Get the head of the SCHED list */
		rlnode* sel = rlist_pop_front(&SCHED);
		next_thread = sel->tcb;
	}

	/* Initialize time slice */
	next_thread->its = QUANTUM;
	return next_thread;
}

