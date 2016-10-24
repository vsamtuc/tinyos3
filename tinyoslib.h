
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


/**
	@brief A declaration for C-like main programs.
  */
typedef int (*Program)(size_t argc, const char** argv);


/**
	@brief Execute a new process, passing it the given arguments.

	The underlying implementation uses the Exec system call, to
	create a new process.
  */
int Execute(Program prog, size_t argc, const char** argv);




#endif