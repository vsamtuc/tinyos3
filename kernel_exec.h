#ifndef KERNEL_EXEC_H
#define KERNEL_EXEC_H

#include "tinyoslib.h"

/**
	@file kernel_exec.h
	@brief Binary execution

	@defgroup binfmt Executable management
	@ingroup kernel
	@brief The executable formats and `dlobj`

	### Executable Formats ###

	An executable format determines the manner in which an executable 
	file can be executed. Each executable format is described by an 
	instance of `struct binfmt`.

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

	### Binary formats ###

	Binary code can be loaded dynamically from the host machine (Linux), using the
	so-called `dlopen` library. In \c tinyos, a loaded shared object (DSO), say  in
	file `foo.so`, is represented by a `struct dlobj` control block. New `dlobj` can 
	be loaded using the @ref dlobj_load() call, and unloaded unsing the @ref 
	dlobj_unload call.

	If a thread executes code from some `dlobj` and while it executes the `dlobj` is
	unloaded, the thread will crash. Therefore executing processes must hold a
	lease on the `dlobj`, so that it does not unload while they are running.
	This is done with @ref dlobj_incref() and @ref dlobj_decref.

	### struct dlobj ###

	Each dlobj is actually a device of type `DEV_DLL`. Therefore, the currently active
	`dlobj` can be found in the `devfs` directory. They can be distinguished because
	their names end with `.so`.


	@{
 */


/**
	@brief Dynamically loaded object control block

	Each control block is a device instance
 */
struct dlobj {
	/** @brief Flags true if used, published */
	_Bool used, published;

	/** @brief Number of published programs (unpublished -> ==0) */
	unsigned int pub_count;

	/** @brief Number of users; >0 <-> busy */
	unsigned int use_count;

	/** @brief The dlopen() handle */
	void* handle;

	/** @brief The link map for this handle */
	struct link_map* lmap;

	/** @brief The filename on the host's current directory */
	char name[MAX_NAME_LENGTH];
};


/** @brief Limit to the number of recursive calls to @ref exec_program */
#define BINFMT_MAX_DEPTH 5

/**
	@brief A structure holding a task and execution arguments.
 */
struct exec_args
{
	/** @brief The task to start a new program. 

		This is typically a loader of some sort, 
		such as \c bf_program_task (defined in kernel_exec.c) */
	Task task;

	/** @brief The length of \c args */
	int argl;

	/** @brief The information for the new execution. 

		This is often some packed structure, like [program,argv] */
	void* args;

	/** @brief A lease on some dlobj, or NULL */
	struct dlobj* lease;
};

/** @brief Null for `struct exec_args` */
#define XARGS_NULL ((struct exec_args){NULL, 0, NULL, NULL})

/**
	@brief Execute a program defined by a file system object.
 */
int exec_program(struct exec_args* xargs, const char* progpathname, const char* argv[], const char* envp[], int iter);


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
	int (*exec_func)(struct exec_args* xargs, Fid_t progfid, const char* progpathname, const char* argv[], const char* envp[], int iter);
};


/**< @brief The table of binary formats */
extern struct binfmt* binfmt_table[];

/**< @brief Find a binary format for magic cookie */
struct binfmt* get_binfmt(const char magic[2]);

/**< @brief Initialize binary formats */
void initialize_binfmt();

/**< @brief Finalize binary formats */
void finalize_binfmt();



/** @brief Size of the dlobj table */
#define MAX_DLOBJ 64

/** @brief The dlobj table */
extern struct dlobj dl_table[MAX_DLOBJ];

/**
	@brief Load a shared object into a \c struct dlobj

	If successful, this call will load a new shared object
	from the Linux current directory and will publish its
	programs to the `binfs` directory. 

	@param name a filename in the current Linux directory pointing
	  to a shared object. It must not contain '/'
	@param dl points to a location where a newly loaded dlobj will be
	  stored
	@return 0 on success and an error code on failure
 */
int dlobj_load(const char* filename, struct dlobj** dl);


/**
	@brief Load a shared object into a \c struct dlobj

	Unload a `dlobj` if it is not busy. If busy, its programs
	are unpublished, but unloading may delay, until it is not
	used any more.

	@param dl the dlobj to unload
	@return 0 on success or an error code
 */
int dlobj_unload(struct dlobj* dl);


/**
	@brief Call a function on each `tos_element` of a dlobj
 */
void dlobj_apply(struct dlobj* dlobj, 
	void (*func)(struct dlobj*, struct tos_entity*));

/**
	@brief Get a lease on a `dlobj`
 */
void dlobj_incref(struct dlobj* dl);

/**
	@brief Release a lease on a `dlobj`

	@return 0 in success or an error code on failure
 */
int dlobj_decref(struct dlobj* dl);


/** 
	@brief Get the pointer to a symbol in a `dlobj` 

	@return pointer to the symbol's location or NULL for failure
*/
void* dlobj_sym(struct dlobj* dl, const char* symbol);

/** 
	@brief Try to find the `dlobj` where an address might be located

	@return pointer to the dlobj or NULL for failure
*/
struct dlobj* dlobj_addr(void* addr);



/** @} */

#endif
