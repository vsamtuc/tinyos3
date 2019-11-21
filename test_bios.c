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


BARE_TEST(test_sched_sizes,"Just print some info about the scheduler")
{
	MSG("Size of TCB=%lu\n", sizeof(TCB));
	MSG("Size of ctx=%lu\n", sizeof(cpu_context_t));
}

TEST_SUITE(sched_tests, "Scheduler tests")
{
	&test_sched_sizes,
	&bios_all_tests,
	NULL
};

int main(int argc, char** argv)
{

        return register_test(&sched_tests) || 
                run_program(argc, argv, &sched_tests);
}
