#ifndef __KERNEL_MM_H
#define __KERNEL_MM_H

#include "util.h"

/**
	@bfile kernel_mm.h
	@brief Memory management in the kernel

	- SIGSEGV protection
	- double stacks

	- separate stack per thread
	- data segments

	Memory model:  A 39-bit address space for the whole machine,
	organized in  2^12-byte pages.

 */


typedef struct memseg
{	
	
	unsigned int refcount;	
	//PCB*   s_owner;
	void*  s_base;
	size_t s_size;

	rlnode s_node;
} memseg;




struct PTE
{
	unsigned page : 27;
	unsigned valid : 1;
	unsigned filler: 4;
};


_Static_assert(sizeof(struct PTE)==4, "The PTE is not 32-bit long");


/* syscalls */
int   MemMap(void** loc, size_t size);


void  MemUnmap(void* ptr, size_t size);


#endif