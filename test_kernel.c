#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include "unit_testing.h"


struct ccall;
typedef struct ccall* coroutine_t(struct ccall*, ...);


struct ccall {
	coroutine_t* callback;
	void* cc;
	struct ccall* caller;
	struct ccall* callee;
};



struct co_desc_t {
	coroutine_t* cofunc;
	size_t frame_size;
};

/* Allocate a continuation if argument not-null */
void* ccall_alloc(struct ccall* c, struct co_desc_t* desc)
{
	if(! c) {
		c = (struct ccall*) malloc(desc->frame_size);
		c->callback = desc->cofunc;
		c->cc = NULL;
	}
	return c;
}

void ccall_free(struct ccall* c)
{
	free(c);
}

/* Set its location and return */
struct ccall* ccall_continue(struct ccall* c, void* cc)
{
	c->cc = cc;
	return c;
}

/* 
	Mark c to have a continuation at address cc.
	Link future to this call and return future 
 */
struct ccall* ccall_await(struct ccall* caller, void* cc, struct ccall* future)
{
	ccall_continue(caller, cc);
	caller->callee = future;
	future->caller = caller;
	return future;
}

struct ccall* ccall_invoke(struct ccall* c)
{
	return c->callback(c);
}


#define CO_FUTURE(T)  struct { struct ccall __call; T __promise; }


#define CCALL_RETVAL(Type, C)  (((CO_FUTURE(Type) *)(C))->__promise)


#define $(var)  (__frame->var)
#define CO_FRAME(Ret, Frame)  struct { struct ccall ccall; Ret promise; Frame frame; }
#define CO_RETURN(v)  { __frame->__promise = (v); return (void*) __frame->__call.caller; }

#define CO_INIT(Cofunc, Frame_t) \
	static struct co_desc_t __desc = { .cofunc = (coroutine_t*)&(Cofunc), .frame_size = sizeof(Frame_t) }; \
	Frame_t * __frame = ccall_alloc(__call, &__desc); \
	if( __frame->__call.cc) goto * (__frame->__call.cc); \

#define CO_BEGIN 	return (void*)ccall_continue((struct ccall*)__frame, &&__start); __start:


#define __CO_AWAIT0(NN)   __cont_point_##NN

#define __CO_AWAIT1(FUTEXPR1, Label) \
	return (void*) ccall_await((struct ccall*)__frame, && __CO_AWAIT0(Label) , (struct ccall*)(FUTEXPR1)); \
	__CO_AWAIT0(Label) : 

#define __CO_AWAIT2(FUTEXPR2)  __CO_AWAIT1(FUTEXPR2, __COUNTER__ )


#define CO_AWAIT(future) \
({ \
	typedef __typeof__( future )  __future_type_t ; \
     __CO_AWAIT2(future) \
	((__future_type_t) __frame->__call.callee )->__promise; \
}) 




CO_FUTURE(int)* read_char2(struct ccall* __call, char* s)
{
	typedef struct {
		CO_FUTURE(int);
		char* s;
	} __frame_t;

	CO_INIT(read_char2, __frame_t);
	/* spill */
	$(s) = s;

	CO_BEGIN
	CO_RETURN(strlen($(s))); 
}


CO_FUTURE(int)* add_number(struct ccall* __call, int m)
{
	typedef struct {
		CO_FUTURE(int);
		int m;		
		int s;
	} __frame_t;

	CO_INIT(add_number, __frame_t);
	/* spill params */
	$(m) = m;
	CO_BEGIN

	/* await */
	$(s) = CO_AWAIT(read_char2(NULL, "Hi there")); 

	$(s) += CO_AWAIT(read_char2(NULL, "Hi"));
	$(s) -= 2;

	/* co_return */
	CO_RETURN($(m)+$(s)); 

}



BARE_TEST(my_test2, "This is a silly test")
{
	struct ccall* cc = (void*) read_char2(NULL, "Hello world");

	cc->caller = NULL;
	ccall_invoke((struct ccall*) cc);

	int len = CCALL_RETVAL(int, cc);
	ASSERT(len == 11);
}


BARE_TEST(my_test3, "This is a silly test")
{
	struct ccall* cc = (void*) add_number(NULL, 5);
	cc->caller = NULL;

	struct ccall* cur = cc;
	while(cur) cur = ccall_invoke(cur);

	int len = CCALL_RETVAL(int, cc);
	MSG("len=%d\n",len);
	ASSERT(len == 13);
}




TEST_SUITE(all_my_tests, "These are mine")
{
	&my_test2,
	&my_test3,
	NULL
};


	
int main(int argc, char** argv)
{
	return register_test(&all_my_tests) ||
		run_program(argc, argv, &all_my_tests);
}

