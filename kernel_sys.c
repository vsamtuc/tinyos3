
#include "tinyos.h"
#include "kernel_sys.h"
#include "kernel_cc.h"
#include "kernel_sig.h"

#ifndef NVALGRIND
#include <valgrind/valgrind.h>
#endif

/*
	Define all the syscalls 
 */


#define PRE_CALL \
assert(get_core_preemption()==1);\
check_sigs();\
kernel_lock();\


#define POST_CALL \
assert(get_core_preemption()==1);\
kernel_unlock();\
check_sigs();\


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

