#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <setjmp.h>
#include "util.h"
#include "bios.h"

#include "unit_testing.h"

#undef NDEBUG
#include <assert.h>
extern const Test bios_all_tests;

#include "kernel_sched.h"


BARE_TEST(test_system_sizes,"Just test and print some info about sizes")
{
	MSG("Size of TCB=%lu\n", sizeof(TCB));
	MSG("Size of ctx=%lu\n", sizeof(cpu_context_t));
	MSG("Min stack size=%d\n", MINSIGSTKSZ);
	MSG("Signal stack size=%d\n", SIGSTKSZ);
	MSG("Word size = %lu\n", sizeof(void*));
	ASSERT(sizeof(rlnode_key)==sizeof(void*));
}


int FooProgram(int argl, void* args)
{
	int z __attribute__((unused)) = 0;
	char foo[10] __attribute__((unused));

	auto void print_something();


	int mymain(int argl, void* args)
	{
		print_something();
		return 0;
	}

	void print_something()
	{
		printf("Hello world\n");
	}

	mymain(argl, args);
	return 0;
}



BARE_TEST(test_nested_funcs, "A simple test for gcc nested functions")
{
	FooProgram(0,NULL);
}


TEST_SUITE(sched_tests, "Scheduler tests")
{
	&test_nested_funcs,
	&test_system_sizes,
	&bios_all_tests,
	NULL
};

int main(int argc, char** argv)
{

        return register_test(&sched_tests) || 
                run_program(argc, argv, &sched_tests);
}
