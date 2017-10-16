
#include "tinyos.h"
#include "kernel_sys.h"
#include "kernel_cc.h"

#ifndef NVALGRIND
#include <valgrind/valgrind.h>
#endif

/*
	Define all the syscalls 
 */


#define PRE_CALL \
kernel_lock();\



#define POST_CALL \
kernel_unlock();\


/* with return */
#define SYSCALL(NAME, RET, SIG, ARGS)\
RET NAME SIG \
{\
	RET __ret;\
	PRE_CALL\
	__ret = sys_##NAME ARGS;\
	POST_CALL\
	return __ret;\
}\

/* without return */
#define SYSCALLV(NAME, SIG, ARGS)\
void NAME SIG \
{\
	PRE_CALL\
	sys_##NAME ARGS;\
	POST_CALL\
}\


SYSCALLS

