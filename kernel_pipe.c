
#include "kernel_proc.h"
#include "tinyos.h"


int sys_Pipe(pipe_t* pipe)
{
	set_errcode(ENOSYS);
	return -1;
}

