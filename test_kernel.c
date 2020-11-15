#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdarg.h>
#include "unit_testing.h"
#include "util.h"

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

struct co_awaitable;
struct co_call;
struct co_task;
struct co_sched;

typedef void* coroutine_t(struct co_call*, void* ret, void* arg);
typedef void*  co_location_t;

typedef max_align_t argbuffer_t;


/*
	An awaitable is any object that can be used to enforce ordering,
	like tasks, calls, events, locks, etc.

	Awaiting on awaitable objects cause c-calls to pass control.
	Each awaitable has an await handler as the first element.

	The await handler takes the awaitable and the context of a c-call
	and returns one of:
	(a) NULL, in which case the caller does not yield, or
	(b) the c-call to yield to.
 */
typedef void* await_handler_t(struct co_awaitable*, struct co_call*, void* ret, void* arg);


struct co_awaitable {
	await_handler_t* await_handler;	/* This is not executed as a co-routine! */
};



/*
	Continuable calls (c-calls) are calls of stackless co-routines.
	This object is at the header of a frame object.

	A c-call can be invoked multiple times, until it returns. When
	invoked, a c-call can yield to other c-calls. Therefore, c-calls are awaitable.
 */
struct co_call {
	await_handler_t* await_handler;	/* Handles the await expression */
	coroutine_t* co_func;		/* The co-routine function */
	co_location_t cloc;			/* Saved location in function */
	struct co_call* stack;		/* The call invoking this call (next in stack) */
};

void* co_call_await_handler(struct co_awaitable* ccall, struct co_call* aw, void* ret, void* arg)
{
	return ccall;
}



/*
	Coroutine-based tasks are thread-like stacks of c-calls, where each
	c-call in the stack is awaited by the c-call below it.

	When the task is ready, the top c-call of the task can be invoked.

	When the top c-call is invoked, it can respond in three ways:
	(a) yield to its parent in the stack; this is like return, i.e. the c-call is
	   popped (and probably destroyed)  TODO: generators?
	(b) yield to another c-call; then, the new c-call is pushed on the stack.
	(c) yield to itself; the task suspends until it is woken up by co_wakeup()

 */
struct co_task {
	rlnode task_node; 			/* Use to enter lists. Value points to scheduler. */
	struct co_call* stack;		/* The current top of the stack */
	struct {
		unsigned int refcount : 20;
		unsigned int ready    : 1;
		unsigned int finished : 1;
	};
	argbuffer_t arg;			/* Argument passed to stack top */
};


/*
	This is a queue with a simple simple round-robin scheduler among tasks.
 */
struct co_sched {
	rlnode queue;				/* The queue of ready tasks */
};


void co_sched_init(struct co_sched* sched)
{
	rlnode_new(&sched->queue);
}


/*
	Retrieve the co_task given pointer to its task node.

	i.e.  For task t

	&t  == co_task_from_node( & t.task_node )
 */
static inline struct co_task* co_task_from_node(rlnode* tn)
{
	return  ((void*)tn) - offsetof(struct co_task, task_node);
}


/*
	Memory management
 */


unsigned int no_ccalls = 0;

/* 
	Allocate a call frame with co_call at the top.
 */
void* co_call_alloc(size_t frame_size, coroutine_t* co_func)
{
	assert(++no_ccalls < 1<<20);		/* Naive protection against endless recursion */
	struct co_call* c = (struct co_call*) xmalloc(frame_size);
	c->await_handler = co_call_await_handler;
	c->co_func = co_func;
	c->cloc = NULL;
	c->stack = NULL;
	return c;
}

/*
	Free a call frame
 */
void co_call_free(struct co_call* call)
{
	free(call);
}


/*
	Allocate a co_task. This is usually called by co_spawn
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




/*
	The scheduler

*/

static inline int co_wakeup(struct co_task* task)
{
	assert(task && task->task_node.obj);
	struct co_sched* sched = task->task_node.obj;
	if(! (task->ready || task->finished)) {
		// remove it from any queue it is in
		rlist_remove(& task->task_node);
		task->ready = 1;
		rlist_push_back(&sched->queue, & task->task_node);
		return 1;
	}
	return 0;
}

/*
	Spawn a new task with the given initialized co_call.
	A pointer to the task is returned. This is a borrowed reference.
 */
struct co_task* co_spawn(struct co_sched* sched, struct co_call* call)
{
	assert(sched && call);
	struct co_task* tsk = co_task_alloc();

	tsk->task_node.obj = sched;
	co_task_incref(tsk); // the queue "owns" a reference to task

	tsk->stack = call;
	call->stack = NULL;
	co_wakeup(tsk);
	return tsk;
}


/*
	Execute one step of a task.
	Return true if, after the invocation, the task is 
	ready, else false.
 */

int co_task_invoke(struct co_task* curtask)
{
	assert(curtask->ready && !curtask->finished);

	struct co_call* curcall = curtask->stack;
	assert(curcall != NULL);

	// call it
	struct co_call* nextcall;

	// copy return value to argument buffer
	argbuffer_t argbuf = curtask->arg;

	/* Pass the return value of the previous call */
 	nextcall = curcall->co_func(curcall, &curtask->arg, &argbuf);


	if( nextcall == curcall ) {
		/* task suspended for event */
		curtask->ready = 0;
		return 0;
	}

	if(nextcall==NULL) {
		/* 
			This was the last item in the stack, we are done. 
			This is a return, 
		 */
		curtask-> ready = 0;
		curtask-> finished = 1;
		curtask-> stack = NULL;

		assert(curcall->stack == NULL);
		co_call_free(curcall);
		co_task_decref(curtask);
		return 0;
	}

	if( nextcall == curcall->stack ) {
		/* A return to caller */
		curtask-> stack = nextcall;
		co_call_free(curcall);		/* TODO: generators? */

	} else {
		/* A new call */
		nextcall->stack = curcall;
		curtask-> stack = nextcall;
	}

	return 1;
}


/*
	Run the main loop of the coroutine task scheduler
 */
void co_run(struct co_sched* sched)
{
	/* 

		A trampoline implementing a simple round-robin strategy
	 */

	while(! is_rlist_empty(& sched->queue) ) {
		struct co_task* curtask = co_task_from_node( rlist_pop_front(& sched->queue) );

		if( co_task_invoke(curtask) )
			rlist_push_back(& sched->queue, & curtask->task_node);
	}
}



/*
	Task handling
 */


struct co_sched* co_task_sched(struct co_task* task)
{
	return task->task_node.obj;
}




/*
	
	The untyped API.

	These are low-level routines that are called inside coroutine functions
	to interact with the run context. They don't normally do much ;-)
 */


/* Set its location and return */
static inline struct co_call* co_call_set_loc(struct co_call* c, co_location_t cloc)
{
	c->cloc = cloc;
	return c;
}

static inline struct co_task* co_call_get_task(void* frame, void* ret, void* arg)
{
	return ret - offsetof(struct co_task, arg);
}


/* This is currently empty, but useful later. */
static inline struct co_call* co_call_await(struct co_awaitable* awaitable, 
	struct co_call* frame, void* ret, void* arg)
{
	assert(awaitable);
	return awaitable->await_handler(awaitable, frame, ret, arg);
}



static inline struct co_call* co_call_return(struct co_call* frame)
{
	struct co_call* caller = frame->stack;
	return caller;
}



/*
	
	The typed API  (cpp-based)

	There are three important conventions in any co-routine using
	these macros:

	(a) The stack frame (1st argument) is called __frame

	(b) The return buffer (2nd argument) is a T* __return where T is the return 
	     type.

	(c) The task (3rd argument) is a void* __arg  pointing to the return value
	of an awaited future (returned by async call). It may be null

	E.g.

	void* my_coroutine(struct my_coroutine* __frame, int* __return, void* __arg)
 */



#define $(var)  (__frame->var)

#define CO_BEGIN  if( __frame->__call.cloc) goto * (__frame->__call.cloc);

#define __CO_YIELD_TO0(NN)    __cloc_##NN
#define __CO_YIELD_TO1(next_ccall, Label)  { \
	co_call_set_loc((struct co_call*)__frame, && __CO_YIELD_TO0(Label) );  \
	return (next_ccall); \
	__CO_YIELD_TO0(Label): ;}
#define CO_YIELD_TO(next_ccall)  __CO_YIELD_TO1(next_ccall, __COUNTER__)


/*
	We are trying to tell the compiler the return type of coroutines.

	(a) We cannot use void, so we use an empty struct Void. This is 
	another GCC extension.

	(b) We declare our  ***co_call constructors*** as returning type tags 
	(actually pointer to suitable unions), declared as follows, e.g.,

	CO_TYPEDEF_TAG(int, co_int_t)

	Then, declare a coro-constructor as
	co_int_t  do_my_coro(...);

	(c) We can access the inverse type of a tag, by CO_TAG_TYPE(tag), for example

	CO_TAG_TYPE(co_int_t)  x;

	means
	int x;


	(d) Declare the actual coroutine by e.g.

	CO_DEFINE(my_coro, int) {
	  CO_BEGIN

		// ... here goes the code

		// use CO_AWAIT and CO_RETURN type-safely.
	}

	------------------------------------------------------------------

	All this machinery allows us to be relatively type-safe and still
	use nice expressions like

	int z = 1 + CO_AWAIT(my_coro(...));

 */


/* Used for the definition of co_void_t */
typedef struct Void {} Void;
extern Void Nothing;


#define CO_TASK_RETURN(Type, tskptr)  (*((Type*) &(tskptr)->arg))

#define CO_ASYNC(T, CoName)  union CoName##_async_t { struct co_call __frame; T __return; }*
#define CO_ASYNC_TYPEOF(awaitable)  __typeof__(& (awaitable)->__return )



#define CO_TYPEDEF_TAG(Type, Name) \
typedef CO_ASYNC(Type, Name) Name;

CO_TYPEDEF_TAG(Void, co_void_t)
CO_TYPEDEF_TAG(int, co_int_t)


#define CO_TAG_TYPE(RetTag)  __typeof__(((RetTag)NULL)->__return)


#define CO_FRAME_BEGIN(CoName)  struct CoName *__frame = co_call_alloc(sizeof(struct CoName), (coroutine_t*)& CoName);
#define CO_FRAME_HEADER  struct co_call __call;
#define CO_FRAME_END  return (void*)__frame;

#define  CO_DEFINE(CoName, RetType)  void* CoName(struct CoName* __frame, RetType* __return, void* __arg)




#define CO_RETURN(v)  \
{ 	\
	(*__return) = (v); \
	CO_YIELD_TO( (void*) co_call_return((struct co_call*)__frame ) ) \
          return (void*) co_call_return((struct co_call*)__frame ) ; \
}


#define CO_AWAIT(awaitable) \
({ \
	struct co_call* target = \
	  co_call_await((struct co_awaitable*)(awaitable),  (struct co_call*)__frame, __return, __arg ); \
	if(target) \
		CO_YIELD_TO( (void*) target ) ; \
	*( (CO_ASYNC_TYPEOF(awaitable)) __arg ); \
}) 





/*
	Support for awaitables
	
	A synchronized object FOO can have a number of awaitable calls;


	If only a single awaitable exists, we can declare FOO as
	struct FOO {
		await_handler_t* ahandler;
		...
	};

	so that it is castable to awaitable.

	More generally, suppose we have a call, W. We
	can declare

	struct FOO {
		...
		struct co_awaitable W;
		...
	}

	and compensate into the respective await handler for the fact
	that they are called with the argument of  & foo.W  instead of & foo.

	In this manner, multiple awaitable fields can be declared.

	Furthermore, to call CO_AWAIT we would need to either:

	-  CO_AWAIT( (co_void_t) & foo->W );    // or

	-  CO_AWAIT(  FOO_W(foo) );    
	  with a suitably defined accessor function FOO_W(FOO*);
	
	co_void_t FOO_W(struct FOO* foo) { return (void*)&foo->W; }
 */


/*
	An event is a queue of suspended tasks and a flag.
	When 'set', calls to  AWAIT(co_event_wait) return immediately.
	When 'clear', call to AWAIT(co_event_wait) suspend.

	This is implemented as a call
 */


struct co_event
{
	struct co_awaitable WAIT;
	rlnode event_queue;
	int flag;
};

void* co_event_WAIT(struct co_awaitable* aw, struct co_call* frame, void* ret, void* arg)
{
	struct co_event* evt = container_of(aw, struct co_event, WAIT);

	if(evt->flag)
		return NULL;
	else {
		struct co_task* tsk = co_call_get_task(frame, ret, arg);
		rlist_push_back(& evt->event_queue, & tsk->task_node);
		return frame;
	}
}

static inline co_void_t co_event_wait(struct co_event* evt)
{
	return (void*) & evt->WAIT;
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
		while(! is_rlist_empty(&evt->event_queue))
			co_wakeup( co_task_from_node(evt->event_queue.next) );
	}
	evt->flag = flag?1:0;
}



/****************************************************************
 *
 *
 *
 *	Tests
 *
 *
 *
 ***************************************************************/

struct hello_world {
	CO_FRAME_HEADER
};

CO_DEFINE(hello_world, Void)
{
	printf("Hello world\n");
	CO_RETURN(Nothing);
}


BARE_TEST(test_hello_world, "Testing coroutine hello world")
{
	struct co_sched S; 
	co_sched_init(&S);

	__auto_type task = co_spawn(&S, co_call_alloc(sizeof(struct hello_world), (void*)hello_world)) ;
	ASSERT(task->refcount==2);
	ASSERT(co_task_sched(task) == &S);

	co_run(&S);
	ASSERT(task->finished);
	ASSERT(task->refcount==1);
	co_task_decref(task);
}



/*
	A trivial co-routine
*/




co_int_t do_read_char(char *s);

struct read_char {
	CO_FRAME_HEADER;
	char* s;
};


CO_DEFINE(read_char, CO_TAG_TYPE(co_int_t))
{
	CO_BEGIN
	CO_RETURN(strlen($(s)));
}

co_int_t do_read_char(char *s)
{
	CO_FRAME_BEGIN(read_char);
	$(s) = s;
	CO_FRAME_END;
}



struct read_char2_co {
	CO_FRAME_HEADER
	char* s;
};

//void*  read_char2_co(struct read_char2* __frame, int* __return, void* __arg);
CO_DEFINE(read_char2_co, int);

co_int_t read_char2(char* s)
{
	CO_FRAME_BEGIN(read_char2_co);
	$(s) = s;
	CO_FRAME_END
}

CO_DEFINE(read_char2_co, int)
{
	CO_BEGIN
	CO_RETURN(strlen($(s))); 
}




struct add_number_co {
	CO_FRAME_HEADER
	int m;		
	int s;
};


CO_DEFINE(add_number_co, int) 
{
	CO_BEGIN

	$(s) = CO_AWAIT(do_read_char("Hi there")); 

	$(s) += CO_AWAIT(read_char2("Hi"));
	$(s) -= 2;

	/* co_return */
	CO_RETURN($(m)+$(s)); 
}

co_int_t add_number(int m)
{
	CO_FRAME_BEGIN(add_number_co);
	$(m) = m;
	CO_FRAME_END
}



BARE_TEST(construct_co_call, "Test the correct size of a new co_call")
{
	struct co_call* cc = (void*) read_char2("Hello world");

	ASSERT(cc->co_func == (coroutine_t*)&read_char2_co);
	ASSERT(cc->cloc == NULL);	

	co_call_free(cc);
}


BARE_TEST(simple_task, "Spawn a simple task and check its output")
{
	struct co_sched S;
	co_sched_init(&S);

	struct co_task* tsk = co_spawn(&S, (void*)add_number(5));

	co_run(&S);

	int ret = CO_TASK_RETURN(int, tsk);
	MSG("task returned  %d\n", ret);
	ASSERT(  ret == 13  );
	ASSERT(tsk->refcount == 1);
	co_task_decref(tsk);
}


struct wfor_task_co
{
	CO_FRAME_HEADER;
	struct co_event* evt;
	int count;
};

CO_DEFINE(wfor_task_co, int)
{
	CO_BEGIN

	CO_AWAIT(co_event_wait($(evt)));
	$(count) ++;

	co_event_set($(evt), 0);
	CO_AWAIT( (co_void_t) $(evt) );

	$(count) ++;

	CO_RETURN($(count));
}

co_int_t wfor_task(struct co_event* evt)
{
	CO_FRAME_BEGIN(wfor_task_co)
	$(evt) = evt;
	$(count) = 0;
	CO_FRAME_END
}



BARE_TEST(test_simple_event, 
	"Test that a task behaves as expected on a simple event")
{
	struct co_sched S;
	co_sched_init(&S);

	struct co_event E;
	co_event_init(&E, 1);

	struct co_task* task = co_spawn(&S, (void*) wfor_task(&E));
	struct wfor_task_co* tcall = (void*)task->stack;

	co_run(&S);

	ASSERT(! task->finished);
	ASSERT(task->stack == (void*)tcall);
	ASSERT(tcall->count == 1);

	ASSERT(E.flag == 0);
	ASSERT(! is_rlist_empty(& E.event_queue));

	co_event_set(&E, 1);

	co_run(&S);

	ASSERT(task->finished);
	ASSERT(task->stack == NULL);
	ASSERT( CO_TASK_RETURN(int, task) == 2);

	ASSERT(task->refcount == 1);
	co_task_decref(task);
}




TEST_SUITE(all_tests, "These are coroutine tests")
{
	& test_hello_world,
	& construct_co_call,
	& simple_task,
	& test_simple_event,
	NULL
};


	
int main(int argc, char** argv)
{
	return register_test(&all_tests) ||
		run_program(argc, argv, &all_tests);
}

