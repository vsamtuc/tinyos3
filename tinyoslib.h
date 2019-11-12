
#ifndef __TINYOSLIB_H
#define __TINYOSLIB_H

#include <stdio.h>
#include "tinyos.h"

/**
	@file tinyoslib.h
	@brief TinyOS standard library header file.

	Small non-kernel routines for wrapping tinyos functionality go here.
  */


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

	The underlying implementation uses the Exec system call, to
	create a new process. 

	By convention, argument argv[0] is assumed to be the program name, 
	although any string is actually acceptable.
  */
int Execute(Program prog, size_t argc, const char** argv);


/**
	@brief Try to reclaim the arguments of a process.

	Given a @ref procinfo object returned by a @ref OpenInfo stream, 
	if this process was executed by @ref Execute, try to reclaim the
	arguments passed to it.

	If the process was not executed by @ref Execute, or if its arguments 
	were too long, this effort may fail, in which case -1 is returned. 

	Else, the @c pinfo argument buffer defined by `(pinfo->argl, pinfo->args)` 
	is parsed. Let it contain @c N arguments. Then,
	- if @c prog is non-NULL, `*prog` is filled with a pointer to the function
	  executed by @c Execute
	- if @c argv is non-NULL, it is assumed that it points to an array of strings,
	  of size argc. Then, min(argc, N) locations of this array are initialized
	  from the arguments of @c pinfo.
	- finally, N is returned.

	@param pinfo the procinfo object to process.
	@param prog if not NULL, the location to hold the program's address.
	@param argc the length of parameter @c argv
	@param argv if not NULL, a string vector of size argc
	@returns if @c pinfo is successfully parsed, the number of arguments is returned,
	    else -1 is returned.
*/
int ParseProcInfo(procinfo* pinfo, Program* prog, int argc, const char** argv );



typedef struct barrier {
	Mutex mx;
	CondVar cv;
	unsigned int count, epoch;
} barrier;

#define BARRIER_INIT  ((barrier){ MUTEX_INIT, COND_INIT, 0, 0})


void BarrierSync(barrier* bar, unsigned int n);


#endif
