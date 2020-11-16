
#include "coroutines.h"

CUSTOM_RLIST(taskq, struct co_task, task_node)



/*
 *
 * Support for c-calls
 *
 */

void* co_call_await_handler(struct co_awaitable* ccall, struct co_call* aw, void* ret, void* arg)
{
	return ccall;
}



static size_t total_call_frames = 0;
static size_t max_call_frames = 1<<20;

void* co_call_alloc(size_t frame_size, coroutine_t* co_func)
{
	/* 
		Die if the number of c-call frames has become too large.

		TODO: Make this configurable. Also, should probably
		monitor total size, but malloc()/free() does not make this
		easy...
	 */

	if(++total_call_frames > max_call_frames) 
		FATAL("Exceeded maximum number of call frames");	

	struct co_call* c = (struct co_call*) xmalloc(frame_size);

	c->await_handler = co_call_await_handler;
	c->co_func = co_func;
	c->cloc = NULL;
	c->stack = NULL;
	return c;
}



void co_call_free(struct co_call* call)
{
	free(call);
}



/*
 *
 * Support for tasks
 *
 */



struct co_task* co_task_alloc()
{
	struct co_task* tsk =  xmalloc(sizeof(struct co_task));
	
	rlnode_new(&tsk->task_node)->obj = NULL;
	tsk->stack = NULL;
	tsk->refcount = 1;
	tsk->ready = 0;
	tsk->finished = 0;
	return tsk;
}

void co_task_free(struct co_task* tsk)
{
	free(tsk);
}


void co_task_incref(struct co_task* tsk)
{
	assert(tsk);
	tsk->refcount ++;
}

void co_task_decref(struct co_task* tsk)
{
	assert(tsk && tsk->refcount);
	if( (--tsk->refcount)==0 ){
		co_task_free(tsk);
	}
}


int co_wakeup(struct co_task* task)
{
	assert(task && task->task_node.obj);
	struct co_sched* sched = task->task_node.obj;

	if(! (task->ready || task->finished)) {
		/* remove it from any queue it is in ... */
		taskq_remove(task);
		task->ready = 1;
		taskq_push_back(&sched->queue, task);
		return 1;
	}
	return 0;
}


struct co_task* co_spawn(struct co_sched* sched, struct co_call* call)
{
	assert(sched && call);
	struct co_task* tsk = co_task_alloc();

	tsk->task_node.obj = sched;
	co_task_incref(tsk); /* the queue "owns" a reference to task */

	tsk->stack = call;
	call->stack = NULL;
	co_wakeup(tsk);
	return tsk;
}


/*
	This is the function where the c-call/couroutine protocol is defined.

	This call invokes one coroutine call in the given task.
 */

int co_task_invoke(struct co_task* curtask)
{
	assert(curtask->ready && !curtask->finished);

	struct co_call* curcall = curtask->stack;
	assert(curcall != NULL);


	/* 
		Copy return value to argument buffer.

		curtask->arg may contain the return value of a previous c-call.
		In any case, this is transfered to argbuf, so that it becomes the
		argument of the next call (in case the last call was a return).

		In theory we could make this transfer conditional, but I am not sure
		it is worth the overhead.
	 */
	argbuffer_t argbuf = curtask->arg;

	/*
		We save curcall's stack, to detect a return
	 */
	struct co_call* curcall_stack = curcall->stack;

	/* 
		We invoke with the call frame, the location for the return value and
		the location of a possible argument.
	 */

	struct co_call* nextcall = curcall->co_func(curcall, &curtask->arg, &argbuf);

 	/* 
 		We first check the case of return, since then curcall
 		has beed deleted. This is also the most frequent case.
 	 */
	if( nextcall == curcall_stack ) { 		/* A return to caller */

		/*
			NULL should only be returned when the task terminates, that is,
			its initial c-call has completed.
		 */		
		if(nextcall==NULL) {
			/* 
				This was the last item in the stack, we are done. 
				This is a return, so we update the task.
			 */
			curtask-> ready = 0;
			curtask-> finished = 1;
			curtask-> stack = NULL;
			co_task_decref(curtask);
			return 0;
		}

		curtask-> stack = nextcall;
		return 1;
	} 


	/* 
		The c-call returned itself, task suspended
	 */
	if( nextcall == curcall ) {
		curtask->ready = 0;
		return 0;
	}


	/* Final case: a new call. This is frequent but we got here by elimination... */

	nextcall->stack = curcall;
	curtask-> stack = nextcall;	

	return 1;
}




void co_run(struct co_sched* sched)
{
	/* 
		A trampoline implementing a simple round-robin strategy
	 */

	while(! is_rlist_empty(& sched->queue) ) {
		struct co_task* curtask = taskq_pop_front(& sched->queue);

		if( co_task_invoke(curtask) )
			taskq_push_back(& sched->queue, curtask);
	}
}


/*
 *
 * Events
 *
 */


static void* co_event_WAIT(struct co_awaitable* aw, struct co_call* frame, void* ret, void* arg)
{
	struct co_event* evt = containerof(aw, struct co_event, WAIT);

	if(evt->flag)
		return NULL;  /* no wait */
	else {
		struct co_task* tsk = co_call_get_task(frame, ret, arg);
		taskq_push_back(& evt->event_queue, tsk);
		return frame;
	}
}

void co_event_init(struct co_event* evt, int flag) 
{
	evt->WAIT.await_handler = co_event_WAIT;
	rlnode_init(&evt -> event_queue, NULL);
	evt->flag = flag;
}

void co_event_set(struct co_event* evt, int flag)
{
	assert( ! evt->flag || is_rlist_empty(&evt->event_queue));
	if(! evt->flag && flag) {
		/* wake everyone up */
		while(! is_rlist_empty(&evt->event_queue)) {
			co_wakeup( taskq_head(&evt->event_queue) );
		}
	}
	evt->flag = flag?1:0;
}



static void* co_sem_WAIT(struct co_awaitable* aw, struct co_call* frame, void* ret, void* arg)
{
	struct co_semaphore* sem = containerof(aw, struct co_semaphore, WAIT);
	if(sem->value > 0) {
		sem->value --;
		return NULL;
	} else {
		struct co_task* tsk = co_call_get_task(frame, ret, arg);
		taskq_push_back(& sem->queue, tsk);
		return frame;		
	}
}

void co_sem_init(struct co_semaphore* sem, int value)
{
	sem->WAIT.await_handler = co_sem_WAIT;
	rlnode_init(& sem->queue, NULL);
	sem->value = value;
}


void co_sem_post(struct co_semaphore* sem)
{
	assert(sem->value <= 0 || is_rlist_empty(& sem->queue));
	sem->value ++;
	while(sem->value > 0 && !is_rlist_empty(&sem->queue)) {
		sem->value--;
		co_wakeup( taskq_head(&sem->queue) );
	}
}



