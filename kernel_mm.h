#include __KERNEL_MM_H
#define __KERNEL_MM_H

/**
	@bfile kernel_mm.h
	@brief Memory management in the kernel

	- SIGSEGV protection
	- double stacks

	- separate stack per thread
	- data segments

 */


struct memseg
{
	void*  s_start;
	size_t s_size;
};


#endif