
#ifndef __TINYOSLIB_H
#define __TINYOSLIB_H

#include <stdio.h>
#include "tinyos.h"

/**
	@file tinyoslib.h
	@brief TinyOS standard library header file.

	Non-kernel routines for wrapping tinyos functionality.
  */


/**
	@brief Print an error message to standard output

	@param msg A prefix to the error message.
 */
void PError(const char* msg);


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
	@brief Execute a new process, passing it the given arguments.

	The underlying implementation uses the @ref Spawn system call, to
	create a new process. 

	By convention, argument argv[0] is assumed to be the program name, 
	although any string is actually acceptable.
  */
int Execute(Program prog, size_t argc, const char** argv);

/**
	@brief Duplicate a file id to a new file id.

	Return a new fid that points to the same file id as \c oldfid.
	In case of error, \c NOFILE is returned and \c GetError() returns
	the error code.

	@param oldfid the file id to be duplicated
	@returns the new file id, or NOFILE
 */
Fid_t Dup(Fid_t oldfid);


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



#endif
