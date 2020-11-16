#pragma once
#include "util.h"


struct co_awaitable;
struct co_call;
struct co_task;
struct co_sched;
struct co_event;

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


static inline void co_sched_init(struct co_sched* sched)
{
	rlnode_new(&sched->queue);
}



/*
	Memory management
 */


/* 
	Allocate a call frame with co_call at the top.
 */
void* co_call_alloc(size_t frame_size, coroutine_t* co_func);

/*
	Free a call frame
 */
void co_call_free(struct co_call* call);



/*
	Get an owned reference to a task. Note that tasks are created by co_spawn()
 */
void co_task_incref(struct co_task* tsk);

/*
	Release a reference to a task.
 */
void co_task_decref(struct co_task* tsk);



/*
	The scheduler

*/


/*
	Make a task runnable again, if possible. Return 1 on success
	and 0 on failure.
 */
int co_wakeup(struct co_task* task);


/*
	Spawn a new task with the given initialized co_call.
	A pointer to the task is returned. This is a borrowed reference.
 */
struct co_task* co_spawn(struct co_sched* sched, struct co_call* call);

/*
	Execute one step of a task.
	Return true if, after the invocation, the task is 
	ready, else false.
 */

int co_task_invoke(struct co_task* curtask);

/*
	Run the main loop of the coroutine task scheduler
 */
void co_run(struct co_sched* sched);


/*
	Task handling
 */


static inline struct co_sched* co_task_sched(struct co_task* task)
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
	co_call_free(frame);
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

#define co_resume   { if( __frame->__call.cloc) goto * (__frame->__call.cloc); }

#define co_task_return(Type, tskptr)  (*((Type*) &(tskptr)->arg))



#define __CO_YIELD_TO0(NN)    __cloc_##NN
#define __CO_YIELD_TO1(next_ccall, Label) \
{ \
	co_call_set_loc((struct co_call*)__frame, && __CO_YIELD_TO0(Label) );  \
	return (next_ccall); \
	__CO_YIELD_TO0(Label): ; \
}


/*
	Record the location to the frame and return the next c-call.

	This, together with 'co_resume', implement our concept of continuation
*/
#define CO_YIELD_TO(next_ccall)  __CO_YIELD_TO1(next_ccall, __COUNTER__)


/*
	We need to handle the return type of coroutines, since the compiler
	cannot be used directly.

	ISSUE 1:
	We cannot use void, so we use an empty struct Void. This is 
	another GCC extension.

	ISSUE 2:
	The return type size must not exceed sizeof(argbuffer_t)
	(which is currently 32 bytes, i.e. 4 64-bit words).

	This is checked by some static asserts, mainly through CO_CHECK_RETTYPE(T)

	ISSUE 3:
	We declare our  c-call  ___constructors___  (not the coroutines) 
	as returning type tags (actually pointer to suitable union), 

	For example, the tag for a return type of 'int' is declared as follows, 

	CO_TYPEDEF_TAG(int, co_int_t)

	Then, declare a coro-constructor as

	co_int_t  do_my_coro(...);

	(c) We can access the inverse type of a tag, by CO_TAG_TYPE(tag), for example

	CO_TAG_TYPE(co_int_t)  x;

	means
	int x;

	ISSUE 4:
	(d) Declare the actual coroutine by e.g.

	CO_DEFINE(my_coro, int) {
	    co_resume

		// ... here goes the code

		// use co_await type-safely.
	}


	------------------------------------------------------------------
	All this machinery allows us to be relatively type-safe and still
	use nice expressions like

	int z = 1 + co_await(my_coro(...));

 */


#define CO_CHECK_RETTYPE(Type) \
 _Static_assert( sizeof(Type) <= sizeof(argbuffer_t), "Return type too large" )


/*
	Tag definition (a typedef)

	Rationale: by making the tag a pointer to a union of struct co_call and Type,
	the standard guarantees that a tag can be cast to  "struct co_call *".

	Also, we can declare variables of tag type, and retrieve the frame via 
	the __frame field.

	co_int_t x;
	return x->__frame;
 */


#define CO_TYPEDEF_TAG(Type, Name) \
 typedef union Name##_async_tag { \
 	struct co_call __frame; \
 	Type __return; \
 	CO_CHECK_RETTYPE(Type); \
 }*  Name; \



/*
	Reclaiming the type from a type tag
 */
#define CO_TAG_TYPE(RetTag)  __typeof__(((RetTag)NULL)->__return)


/* Used for the definition of co_void_t */
typedef struct Void {} Void;
extern Void Nothing;


/*
	Some standard predefined tags
 */
CO_TYPEDEF_TAG(Void, co_void_t)
CO_TYPEDEF_TAG(int, co_int_t)



/*
	Used to declare the frame object.
 */
#define CO_FRAME_HEADER  struct co_call __call;


/*
	Used to define frame constructors
 */
#define CO_FRAME_BEGIN(CoName) \
 struct CoName *__frame = co_call_alloc(sizeof(struct CoName), (coroutine_t*)& CoName);
#define CO_FRAME_END  return (void*)__frame;


/*
	Used to define coroutine bodies
 */
#define  CO_DEFINE(CoName, RetType)  \
 CO_CHECK_RETTYPE(RetType); \
 void* CoName(struct CoName* __frame, RetType* __return, void* __arg)




#define co_return(v)  \
{ 	\
	/*
		Assign result to the return buffer
	 */ \
	(*__return) = (v); \
	CO_YIELD_TO( (void*) co_call_return((struct co_call*)__frame ) ) \
	/* 
		Abort if called after co_return !! 
	 */  \
	/* abort() through util.h */ \
	FATAL("Resumed coroutine after it has returned");  \
}


#define co_await_void(awaitable)  \
{\
	/* 
		compute the yield target by calling co_call_await()  
	*/ \
	struct co_call* target = \
	  co_call_await((struct co_awaitable*)(awaitable), \
	    (struct co_call*)__frame, __return, __arg ); \
    /* 
    	yield if target is not null 
	 */  \
	if(target) \
		CO_YIELD_TO( (void*) target ) ; \
}


#define co_await_as(Type, Awaitable) \
({\
	co_await_void( Awaitable ) \
	/*  
		cast the return argument (may be junk,  but still...!)  
	 */\
	typedef  Type __aw_type; \
	*((  __aw_type * ) __arg ); \
})


#define co_await(awaitable)  co_await_as( __typeof__((awaitable)->__return) , awaitable)




/******************************
 *
 *	Awaitable objects
 *
 ******************************/


/*
	An event is a queue of suspended tasks and a flag.
	
	When 'set' (flag==1), calls to  AWAIT(co_event_wait) return immediately,
	and any suspended tasks also resume.

	When 'clear' (flag==0), call to AWAIT(co_event_wait) suspend.
 */


struct co_event
{
	struct co_awaitable WAIT;
	rlnode event_queue;
	int flag;
};

/*
	Returns an awaitable for the given event
 */
static inline co_void_t co_event_wait(struct co_event* evt)
{
	return (void*) & evt->WAIT;
}

/*
	Initialize with initial flag value
 */
void co_event_init(struct co_event* evt, int flag);

/*
	Set the flag of the event. If set to 1, any suspended tasks
	awaken.
 */
void co_event_set(struct co_event* evt, int flag);


/*
	A semaphore contains an integer value, where 
	tasks make two operations:

	(a) co_sem_post(s)  (same as Dijkstra's V()) or,
	(b) CO_AWAIT(co_sem_wait(s))  (same as Dijkstra's P()).

	The semaphore counter is in the 'value' field.
 */

struct co_semaphore
{
	struct co_awaitable WAIT;
	rlnode queue;
	int value;
};


/*
	Returns an awaitable for the given event
 */
static inline co_void_t co_sem_wait(struct co_semaphore* sem)
{
	return (void*) & sem->WAIT;
}

/*
	Initialize with initial value.
 */
void co_sem_init(struct co_semaphore* sem, int value);


/*
	Set the flag of the event. If set to 1, any suspended tasks
	awaken.
 */
void co_sem_post(struct co_semaphore* sem);



