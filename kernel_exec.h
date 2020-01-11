#ifndef KERNEL_EXEC_H
#define KERNEL_EXEC_H

#include "tinyos.h"

/**
	@file kernel_exec.h
	@brief Executable formats

	@defgroup binfmt Executables
	@ingroup kernel
	@brief The executable formats

	An executable format determines the manner in
	which an executable file can be executed. Each
	executable format is described by an instance of 
	`struct binfmt`.

	The fundamental executable format of \c tinyos is that
	of a \c Task function, with arguments `argl, args`.
	This is the native form that the kernel supports.

	Other possibilities include:
	- \c Program with `argc,argv`, close to the traditional C/Unix form
	- __shebang__ execution of scripts, where the first line starts with #!
	- etc.

	Each binfmt has its own execution function, and can execute files of
	a certain format. The format of a file is determined by reading the
	first two bytes of the file, the so-called `magic number`.

	Function \c exec_program looks at the magic number of a file and 
	invokes the right \c binfmt. Note that a format can call \c exec_program 
	recursively. For example, consider a Unix shell script, say \c myscript.sh, 
	that looks like
	\code
	#! /bin/sh
	echo Hello world
	\endcode

	When `exec_program("myscript.sh", NULL, NULL, 0)` is called, 
	the "shebang" magic number (#!) leads to the shebang format. This format
	will in its turn call `exec_program("/bin/sh", ["myscript.sh"], NULL,1)`
	(where the second argument represents a 1-element string array).
	
	Depending on the magic number of /bin/sh, another recursive call to `exec_program`
	may be made. The limit of this recursion is \c BINFMT_MAX_DEPTH.

	@{
 */

/** @brief Limit to the number of recursive calls to @ref exec_program */
#define BINFMT_MAX_DEPTH 5

/**
	@brief A structure holding a task and execution arguments.
 */
struct exec_args
{
	Task task;
	int argl;
	void* args;
};

/**
	@brief Execute a program defined by a file system object.
 */
int exec_program(struct exec_args* xargs, const char* progpathname, const char* const argv[], char* const envp[], int iter);


/**
	@brief Binary format
 */
struct binfmt
{
	/** @brief Magic number for this format */
	char magic[2];

	/** 
		@brief Executor function for a binary format.

		@param xargs structure that is filled by this call
		@param progfid an open readable stream on \c progpathname
		@param progpathname the program pathname
		@param argv vector of program arguments
		@param envp vector of environment variables
		@param iter iteration level
	 */
	int (*exec_func)(struct exec_args* xargs, Fid_t progfid, const char* progpathname, const char* const argv[], char* const envp[], int iter);
};


/**< @brief The table of binary formats */
extern struct binfmt* binfmt_table[];

/**< @brief Find a binary format for magic cookie */
struct binfmt* get_binfmt(const char magic[2]);


/**< @brief Initialize binary formats */
void initialize_binfmt();


/** @} */

#endif
