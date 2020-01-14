
#ifndef __TINYOSLIB_H
#define __TINYOSLIB_H

#include <stdio.h>
#include "tinyos.h"
#include "util.h"

/**
	@file tinyoslib.h
	@brief TinyOS standard library header file.

	Non-kernel routines for wrapping tinyos functionality.
  */


/**
	@brief Print an error message to standard output

	@param msg A prefix to the error message.
 */
void PError(const char* fmtmsg,...);


/**
    @brief Open a C stream on a tinyos file descriptor.

	This call returns a new FILE pointer on success and NULL
	on failure.
*/
FILE* fidopen(Fid_t fid, const char* mode);

void tinyos_replace_stdio();
void tinyos_restore_stdio();
void tinyos_pseudo_console();


/**
	@brief A declaration for C-like main programs.
  */
typedef int (*Program)(size_t argc, const char** argv);


/**
	@brief Execute a program in a new process, passing it the given arguments.

	The underlying implementation uses the @ref Spawn system call, to
	create a new process. 

	Note that this function is completely different from the \c Exec
	system call, which terminates the current program and executes
	a new one in the current process.

	By convention, argument argv[0] is assumed to be the program name, 
	although any string is actually acceptable.
  */
int SpawnProgram(const char* prog, size_t argc, const char* const * argv);


/**
	@brief Try to reclaim the arguments of a process.

	Given \c task and its arguments \c argl and \c args,
	if this process was executed by @ref Execute, try to reclaim the
	arguments passed to it.

	If the process was not executed by @ref Execute, this effort may fail, 
	in which case -1 is returned. 

	Else, the arguments are parsed:

	- If \c prog is non-NULL, the location of the executable function is
	stored in it.

	- The number of arguments, say N, is returned.

	- If \c argv is non-\c NULL, it must point to an array of strings 
	(const char*) of size \c argc. The first \c min(N,argc) elements of this
	array are set to positions inside args, corresponding to the arguments
	of the program.

	@param task the task executed by this program (is used to determine the format of the args)
	@param prog if not NULL, the location to hold the program's address.
	@param argc the length of parameter @c argv
	@param argv if not NULL, a string vector of size argc
	@returns if successfully parsed, the number of arguments is returned,
	    else -1 is returned.
*/
int ParseProgArgs(Task task, int argl, void* args, Program* prog, int argc, const char** argv );



/**
	@brief A synchronization barrier.

	This object implements barrier synchronization between a collection of threads.
	Instances are declared by code like this:
	\code
	barrier b = BARRIER_INIT;
	\endcode

	@see BarrierSync
  */
typedef struct barrier {
	Mutex mx;						/**< @brief A mutex */
	CondVar cv;						/**< @brief A condition variable */
	unsigned int count;				/**< @brief Barrier state */
	unsigned int epoch;				/**< @brief Barrie state */
} barrier;

/**< @brief Initializer for @ref barrier objects */
#define BARRIER_INIT  ((barrier){ MUTEX_INIT, COND_INIT, 0, 0})

/**
	@brief Synchronize with a set of threads.

	Assume that a set of \c N processes or threads wants to synchronize
	by assuring that all \c N processes have reached a certain point
	in their execution. This can be done by having each process call
	\code
	BarrierSync(br, N);
	\endocde
	where \c br is a pointer to a \c barrier.

	The exact semantics of this call are as follows: the \c barrier
	has an internal counter which is initialized to \c 0. 
	When a thread calls \c BarrierSync(bar,n) the counter is first incremented.
	If the counter is equal or larger than \c n, then the barrier resets.
	That is, all waiting threads are unblocked and the counter is reset to 0. 
	Else, the current caller is blocked.

	Once reset, the barrier can be used again.

	@param bar pointer to the \c barrier object to use
	@param n  the threshold for the current set of threads
 */
void BarrierSync(barrier* bar, unsigned int n);


/**
	@brief Read the next entry from a directory stream

	This call will return the next name from a directory stream
	into \c buffer. The size of \c buffer should be at least
	\c MAX_NAME_LENGTH+1. If it is smaller, then the call may
	still succeed, if the next name happens to fit in the space
	provided. If the next name does not fit, -1 is returned. The
	call should be repeated with a larger buffer.

	@param dirfid the fid of a directory
	@param buffer will contain the next directory name
	@param size  the size of the buffer
	@return the number of bytes returned, or 0 for the end of stream,
	    or -1 in the case of error.
 */
int ReadDir(int dirfid, char* buffer, unsigned int size);


/**
	@brief Return a timestamp for the current time
	
	Read the current timestamp from /dev/clock and store it in \c ts.

	@param ts the location where the timestamp is stored
	@return 0 on success and -1 on failure. In case of failure,
	  GetError() returns the error code.
 */
int GetTimestamp(timestamp_t* ts);


struct tm;
/**
	@brief Convert a timestamp to broken-down time.

	The conversion is made to local time.
	Since `timestamp_t` has a resolution of microseconds, the time
	in \c tm is the number of seconds, leaving some remainder.
	If \c usec is not \c NULL, the remaining micro-seconds not 
	included in \c tm are stored there.

	@param ts the timestamp to convert
	@param tm the location where the broken-down time is stored.
	@param usec the location where the microsecond remainder is stored, or \c NULL
 */
void LocalTime(timestamp_t ts, struct tm* tm, unsigned long* usec);

/**
	@brief Return broken-down current time
	
	Read the current time from /dev/clock and break it down into 
	a location. The current time is read using @ref GetTimeOfDay.
	Conversion is made via @ref LocalTime.

	@param tm the location where the broken-down time is stored.
	@param usec the location where the microsecond remainder is stored, or \c NULL
	@return 0 on success and -1 on failure. In case of failure,
	  GetError() returns the error code.
 */
int GetTimeOfDay(struct tm* tm, unsigned long* usec);


/* DLL device ioctls */

/**< 
	@brief ioctl request to load a shared object into tinyos 
	@see dll_load
 */	
#define DLL_LOAD   0x1
#define DLL_UNLOAD 0x2


int dll_load(const char* name);
int dll_unload(const char* name);


enum tos_type
{
	TOS_PROGRAM
};


/*
	The size of this must be a power of 2, so that ELF will
	pack the structures.
 */
struct tos_entity {
	void* data;
	size_t size;
	enum tos_type type;
	char name[MAX_NAME_LENGTH];
	rlnode dnode;
	void* __filler[2];
};


#define TOS_REGISTRY  								\
extern struct tos_entity __start_tinyos;			\
extern struct tos_entity __stop_tinyos;				\
struct tos_entity* __begin_tinyos = &__start_tinyos;\
struct tos_entity* __end_tinyos = &__stop_tinyos;	\


struct bf_program
{
	char magic[2];
	Program program;
};


#define REGISTER_PROGRAM(C)						\
static struct bf_program __program_##C = {{'#','P'}, &C}; \
static struct tos_entity __descriptor_##C 			\
    __attribute__((__section__("tinyos")))			\
    __attribute__((__used__)) 						\
 	= { .data = &__program_##C, .size = sizeof(__program_##C),	\
 		.type=TOS_PROGRAM, .name= #C}; 							\



#endif
