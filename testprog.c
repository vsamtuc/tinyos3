#include "tinyoslib.h"

TOS_REGISTRY

int test_program(size_t argc, const char** argv)
{
	printf("Hello world from process %d\n", GetPid());
	return 0; 
}
REGISTER_PROGRAM(test_program)


int echo_args(size_t argc, const char** argv)
{
	printf("echo_args:\n");
	for(int i=0; i<argc; i++) 
		printf("\t argv[%d]=%s\n", i, argv[i]);
	return 0;
}
REGISTER_PROGRAM(echo_args)

