#include "tinyoslib.h"

int test_program(size_t argc, const char** argv)
{
	printf("Second Hello world from process %d\n", GetPid());
	return 0; 
}


REGISTER_PROGRAM(test_program);

TOS_REGISTRY
