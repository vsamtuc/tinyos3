#include <string.h>

#include "unit_testing.h"
#include "coroutines.h"





/****************************************************************
 *
 *	Tests for coroutines
 *
 ***************************************************************/

struct hello_world {
	CO_FRAME_HEADER
};

CO_DEFINE(hello_world, Void)
{
	printf("Hello world\n");
	co_return(Nothing);
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
	co_resume
	co_return(strlen($(s)));
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
	co_resume
	co_return(strlen($(s))); 
}




struct add_number_co {
	CO_FRAME_HEADER
	int m;		
	int s;
};


CO_DEFINE(add_number_co, int) 
{
	co_resume

	MSG("Initially, m=%d and s=%d\n", $(m), $(s));

	$(s) = co_await(do_read_char("Hi there")); 

	MSG("Then, m=%d and s=%d\n", $(m), $(s));

	$(s) += co_await(read_char2("Hi"));

	MSG("Later, m=%d and s=%d\n", $(m), $(s));

	$(s) -= 2;

	MSG("About to return %d\n", $(m)+$(s));

	/* co_return */
	co_return($(m)+$(s)); 
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

	int ret = co_task_return(int, tsk);
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
	co_resume

	co_await(co_event_wait($(evt)));
	$(count) ++;

	co_event_set($(evt), 0);
	co_await( (co_void_t) $(evt) );

	$(count) ++;

	co_return($(count));
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
	ASSERT( co_task_return(int, task) == 2);

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

