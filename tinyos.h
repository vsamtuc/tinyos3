
#ifndef __TINYOS_H__
#define __TINYOS_H__

#include <stdint.h>

/**
  @file tinyos.h
  @brief Public kernel API
 
  @defgroup syscalls System calls.
  @ingroup kernel 
  @brief Public kernel API

  This file contains the system calls offered by TinyOS to the 
  applications. These calls are split into three groups:
  (a) process control  (b) concurrency control and (c) I/O

  @{
 */

/*******************************************
 * Processes types and constants.
 *******************************************/

/**
  @brief The type of a process ID.
  */
typedef int Pid_t;		/* The PID type  */

/**
  @brief An integer type for time intervals.

  The unit is milliseconds.
*/
typedef unsigned long timeout_t;


/** @brief The invalid PID */
#define NOPROC (-1)

/** @brief The maximum number of processes */
#define MAX_PROC 65536

/** @brief The type of a file ID. */
typedef int Fid_t;  

/** @brief The maximum number of open files per process. 
   Only values 0 to MAX_FILEID-1 are legal for file descriptors. */
#define MAX_FILEID 16

/** @brief The invalid file id. */
#define NOFILE  (-1)


/**
  @brief The type of a thread ID.
  */
typedef uintptr_t Tid_t;

/** @brief The invalid thread ID */
#define NOTHREAD ((Tid_t)0)


/**
  @brief The type of an i-node ID.
  */
typedef uintptr_t inode_t;


/*******************************************
 *      Error handling
 *******************************************/

/**
	@brief Return the error code of the last error.

	When a system call performed by a thread returns an error condition, the error
	code corresponding to the error is returned by this system call. The value returned
	by this system call stays the same up until the next system call that fails.

	Note that this system call never fails.

	The error codes are compatible with the standard POSIX codes, and 
	documentation for them can be found in 
	the POSIX standard: https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/errno.h.html

	@return the last error code.
*/
int GetError();


/*******************************************
 *      Concurrency control
 *******************************************/

/** @brief A mutex is used to provide mutual exclusion. 
  
    Mutexes are used extensively to surround critical sections. The TinyOS
    mutexes are suitable for use in user-space, as well as in the implementation 
    of the kernel.

    @see Mutex_Lock
    @see Mutex_Unlock
    @see MUTEX_INIT
*/
typedef volatile _Atomic char Mutex;

/**
  @brief This macro is used to initialize mutexes. 

   Always initialize a mutex as follows:
  @code
   Mutex my_mutex = MUTEX_INIT;
  @endcode
 */
#define MUTEX_INIT 0


/** @brief Lock a mutex.

  Lock a mutex, by waiting if necessary, as long as it takes. In user-space and
  in kernel-space (preemptive domain), the locking will yield after spinning for a few hundred times.
  In scheduler space (non-preemptive domain), the mutex lock operation is pure spinlock.

  @see Mutex
  @see Mutex_Unlock
  @see set_core_preemption
  */
void Mutex_Lock(Mutex*);

/** @brief Unlock a mutex that you locked. 
  
    This operation is non-blocking.
    @see Mutex
    @see Mutex_Lock
*/
void Mutex_Unlock(Mutex*);


/** @brief Condition variables.

  A condition variable is used for longer synchronization. This implementation
  can be used both in the pre-emptive and in the non-preemptive domain.

  @see Cond_Wait
  @see Cond_Signal
  @see Cond_Broadcast
  @see COND_INIT
 */
typedef struct {
	void* filler[4];	/**< @brief reserving space for 4 pointers */
} CondVar;


/** @brief  This macro is used to initialize condition variables. 

   It is used as follows:
  @code
  CondVar my_cv = COND_INIT;
  @endcode
 */
#define COND_INIT ((CondVar){ { NULL, NULL, NULL, NULL}  })


/** @brief Wait on a condition variable. 

  This must be called only while we have locked the mutex that is 
  associated with this call. It will put the calling thread to sleep, 
  unlocking the mutex. These operations happen atomically.  

  When the thread is woken up later, it
  first re-locks the mutex and then returns.  
  A thread may wake up if,
  - another thread called @c Cond_Signal or @c Cond_Broadcast
  - other reasons, not specified

  @param mx The mutex to be unlocked as the thread sleeps.
  @param cv The condition variable to sleep on.
  @returns 1 if this thread was woken up by signal/broadcast, 0 otherwise
  @see Cond_Signal
  @see Cond_Broadcast
  */
int Cond_Wait(Mutex* mx, CondVar* cv);


/** @brief Wait on a condition variable. 

  This must be called only while we have locked the mutex that is 
  associated with this call. It will put the calling thread to sleep, 
  unlocking the mutex. These operations happen atomically.  

  When the thread is woken up later, it
  first re-locks the mutex and then returns.  
  A thread may wake up if,
  - another thread called @c Cond_Signal or @c Cond_Broadcast
  - the timeout expired
  - other reasons, not specified

  @param mx The mutex to be unlocked as the thread sleeps.
  @param cv The condition variable to sleep on.
  @param timeout The time in milliseconds to wait blocked on the condition.
  @returns 1 if this thread was woken up by signal/broadcast, 0 otherwise
  @see Cond_Signal
  @see Cond_Broadcast
  */
int Cond_TimedWait(Mutex* mx, CondVar* cv, timeout_t timeout);



/** @brief Signal a condition variable. 
   
   This call wakes up exactly one thread sleeping on this condition
   variable (if any). Note that the woken thread does not preempt the
   calling thread; i.e., this is a Mesa-style implementation.
   @see Cond_Wait
   @see Cond_Broadcast
   */
void Cond_Signal(CondVar*);

/** @brief Notify all threads waiting at a condition variable.

  Broadcast wakes up all threads sleeping on this condition variable.
  The calling thread is not preempted by the awoken threads.

  @see Cond_Wait
  @see Cond_Signal
*/
void Cond_Broadcast(CondVar*); 


/*******************************************
 *
 * Process creation
 *
 *******************************************/

/** @brief The signature for the main function of a process. 

   New processes are created by calling a starting function, whose signature is Task. 
   @see Spawn
  */
typedef int (*Task)(int, void*);


/** @brief Create a new process.

  This call creates a new process by calling function @c task, 
  passing it a byte array. The byte array is described by a pair
  of  (int length,void* position), and is a _copy_ of the
  byte array defined by the (argl, args) pair of arguments to `Spawn`.
  
  
  - The new process inherits all file ids of the current process.
  - The new process is a child of the current process.
  - The new process is started with a thread executing @c task. When 
    this thread returns, with an integer value, the process terminates,
    yielding this exit status.

  @param task the main function  of the new process
  @param argl the length of byte array @c args
  @param args the byte array copied as argument to `task`
   @return On success, the pid of the new process is returned.
    On error, NOPROC is returned.
     Possible errors:
   -  **@c EAGAIN** The maximum number of processes has been reached.
  */
Pid_t Spawn(Task task, int argl, void* args);


Pid_t Vfork();


void Exec(Task task, int argl, void* args);


/** @brief Exit the current process.

  When this function is called by a process thread, the process terminates
  and sets its exit code to @c val. 

  @note Alternatively, the process may terminate by returning (with an integer 
  return value) from its main function, in which case the return value of the
  main function becomes the exit status.

  @param val the exit status of the process
  @see Spawn
   */
void Exit(int val);

/** @brief Wait on a terminating child.

   This function will return the exit status of a terminated 
   child process, waiting if necessary for a child process to end. 
   When parameter
   @c pid holds the value of a specific child process of this process,
   @c WaitChild will wait for this specific process to finish. If
   parameter @p pid is equal to @c NOPROC, then @c WaitChild will wait for
   *any* child process to exit. 

   If parameter @c exitval is a not null, the exit code of the child
   process will be stored in the variable pointed to by status.

    @param pid the process ID of the child to wait on, or @c NOPROC to
           designate waiting for any child.
    @param exitval a location whithin which the exit status of the terminates
   @return On success, @c WaitChild returns the pid of an exited child.
   On error, WaitChild returns @c NOPROC. Possible errors are:
	- **@c ESRCH** the specified pid is not a valid pid.
	- **@c ECHILD** the specified process is not a child of this process.
	- **@c ECHILD** the process has no child processes to wait on (when pid=NOPROC).
	- **@c EINTR** The call was interrupted.
*/
Pid_t WaitChild(Pid_t pid, int* exitval);

/** @brief Return the PID of the caller.

 This function returns the pid of the current process 
 */
Pid_t GetPid(void);

/** @brief Return the PID of the caller's parent.

 This function returns the pid of the parent of the current process.
 */
Pid_t GetPPid(void);


/**
	@brief Terminate a process.

	This system call will terminate another process.
	The only exception is that the INIT process cannot be terminated.
	The terminated process may be leave memory in an inconsistent state.

	@param pid the pid of the process to terminate
	@return 0 on success, or -1 if an error occurred. Possible errors include:
	- **@c EPERM** The \c init process was targeted (i.e., pid==1)
	- **@c EINVAL** The pid is not of a valid process
*/
int Kill(Pid_t pid);


/*******************************************
 *
 * Threads
 *
 *******************************************/


/** 
  @brief Create a new thread in the current process.

  The new thread is executed in the same process as 
  the calling thread. If this thread returns from function
  task, the return value is used as an argument to 
  `ThreadExit`.

  The new thread is created by executing function `task`,
  with the arguments of `argl` and `args` passed to it.
  Note that, unlike @ref Spawn(), where argl and args must define
  a byte buffer, here there is no such requirement! 
  The two arguments are passed to the new thread verbatim,
  and can be unrelated. It is the responsibility of the
  programmer to define their meaning.

  @param task a function to execute
  @param argl integer to pass to task on thread execution
  @param args pointer to pass to task on thread execution
  @returns thread id of the new thread, or NO_THREAD on error
     Possible errors are
     - **@c EINVAL** argument @c task is NULL
  */
Tid_t CreateThread(Task task, int argl, void* args);

/**
  @brief Return the Tid of the current thread.
 */
Tid_t ThreadSelf();

/**
  @brief Join the given thread.

  This function will wait for the thread with the given
  tid to exit, and return its exit status in `*exitval`.
  The tid must refer to a legal thread owned by the same
  process that owns the caller. Also, the thread must 
  be undetached, or an error is returned.

  After a call to join succeeds, subsequent calls will fail
  (unless tid was re-cycled to a new thread). 

  It is possible that multiple threads try to join the
  same thread. If these threads block, then all must return the
  exit status correctly.

  @param tid the thread to join
  @param exitval a location where to store the exit value of the joined 
              thread. If NULL, the exit status is not returned.
  @returns 0 on success and -1 on error. Possible errors are:
    - **@c ESRCH** there is no thread with the given tid in this process.
    - **@c EDEADLK** the tid corresponds to the current thread.
    - **@c EINVAL** the tid corresponds to a detached thread.
    - **@c EINTR** The call was interrupted
  */
int ThreadJoin(Tid_t tid, int* exitval);


/**
  @brief Detach the given thread.

  This function makes the thread tid a detached thread.
  A detached thread is not joinable (ThreadJoin returns an
  error). 

  Once a thread has exited, it cannot be detached. A thread
  can detach itself.

  @param tid the tid of the thread to detach
  @returns 0 on success, and -1 on error. Possibe errors are:
    - **@c ESRCH** there is no thread with the given tid in this process.
    - **@c EINVAL** the tid corresponds to an exited thread.
  */
int ThreadDetach(Tid_t tid);

/**
  @brief Terminate the current thread.

  Note that this call never returns.
  */
void ThreadExit(int exitval);


/*******************************************
 *
 * Low-level I/O
 *
 *******************************************/

/** @brief Return the number of terminal devices available. 

  Terminals are numbered starting from 0. 
 */
unsigned int GetTerminalDevices();

/** @brief Open a stream on terminal device 'termno'.

  @param termno the terminal number to open
  @return the file ID of the new descriptor
    On success, OpenTerminal returns the file id for a new file for this 
   terminal. On error, it returns @c NOFILE. Possible errors are:
   - **@c ENODEV** The terminal device does not exist.
   - **@c EMFILE** The maximum number of per-process file descriptors has been reached.
   - **@c ENFILE** The maximum number of system-wide files has been reached.

 */
Fid_t OpenTerminal(unsigned int termno);


/** @brief Open a stream on the null device.

  The null device is a virtual device representing an "infinite"
  sequence of 0 bytes.  Every read on this device returns the
  requested number of 0s. Also, every write to this device has
  no effecf.

  @return On success, OpenNull returns the file id for a new file for this 
  terminal. On error, it returns NOFILE. Possible errors are:
   - **@c EMFILE** The maximum number of per-process file descriptors has been reached.
   - **@c ENFILE** The maximum number of system-wide files has been reached.
*/
Fid_t OpenNull();


/*******************************************
 *
 * File system I/O
 *
 *******************************************/


/** @brief Maximum length of a directory entry name */
#define MAX_NAME_LENGTH 63

/** @brief Maximum length of a pathname. */
#define MAX_PATHNAME 512


/**
	@brief Flags to be passed to @ref Open().

	These flags fall in two categories:
	- creation flags are \c OPEN_CREATE, \c OPEN_EXCL and \c OPEN_TRUNC
	  and determine what happens at the beginning, when \c Open is called.
	- status flags determine the type of subsequent I/O operations, once
	  the stream is created
 */
enum Open_flags
{
	/* Status flags */
	OPEN_RDONLY = 001,	/**< @brief Open only for reading */
	OPEN_WRONLY = 002,	/**< @brief Open only for writing */
	OPEN_RDWR   = 003,	/**< @brief Open for both reading and writing */
	OPEN_APPEND = 012,	/**< @brief Open in "append mode" */

	/* Creation flags */
	OPEN_CREAT  = 100,	/**< @brief Create file if it does not exist */
	OPEN_EXCL   = 200,	/**< @brief Ensure the file does not exist, fail if it does. */
	OPEN_TRUNC  = 400	/**< @brief Truncate file (if opened for writing) */
};


/**
	@brief Open a file system entity specified by a path name.

	This call returns a file id representing a stream on the file system element
	specified by \c pathname. The returned Fid can then be used for normal stream I/O,
	in a mode specified at the time of creation by the \c flags parameter.

	The stream returned by \c Open supports  operations depending on the type of underlying
	file system element that was specified by \c pathname. The following rules apply.

	- **Directories:** A directory can be opened only for reading. It is not allowed to
	  create a directory via \c Open(). The stream returned by a directory is a sequence
	  of 0-terminated strings, each string being the name of one member of the directory.
	  Besides @ref Read(), the only other method callable on a directory stream is @ref Seek().

	- **Devices** Opening a device returns a stream sending and receiving from some device, such
	  as the serial device. The actual semantics of I/O operations depend onthe particular device.

	- **Files** Files are the main method of storing and retrieving data in the file system. Potentially
	  the full spectrum of I/O operations can be applied to a file. Note that some file system types
	  restrict the operations allowed on a file. For example, some filesystems are read-only.

	The flags parameter takes a bit-wise OR of of a number of flags, appearing in the
	@ref Open_flags enumeration. There are two types of flags, 	__status__ flags and 
	__creation__ flags.


	Status flags determine the semantics of the I/O operations of the returned stream.
	One of \c OPEN_RDONLY, \c OPEN_WRONLY and \c OPEN_RDWR must always be provided. 

	An additional status flag is \c OPEN_APPEND, which sets the stream in __append mode__.
	In this mode, every @ref Write() operation moves the position of the stream to the end
	of the stream.

	Creation flags determine the operations performed during the call to \c Open(). The
	\c OPEN_CREATE flag denotes that if the \c pathname does not exist, a regular file is
	created. 

	The \c OPEN_EXCL flag indicates that the pathname is not expected to exist, and
	should it exist, an error should be returned by \c Open.

	Finally, the \c OPEN_TRUNC flag indicates that, if the pathname already exists, 
	is a regular file and is opened so that writing is allowed, then the file will be 
	truncated to length 0 at the time of opening. In all other cases, the flag is ignored.

	@param pathname is the pathname to be opened
	@param flags is a bit-wise OR of values of @ref enum Open_flags
	@returns an fid of a new stream, or \c NOFILE in case of error. 
	   Possible errors are:	   
   - **@c EMFILE** The maximum number of per-process file descriptors has been reached.
   - **@c ENFILE** The maximum number of system-wide files has been reached.
   - **@c ENAMETOOLONG** The pathname exceeds the limits of the operating system.
   - **@c ENOENT ** The pathname does not exist and creation was not requested.
   - **@c EEXIST ** The pathname does exist and \c OPEN_EXCL|OPEN_CREATE was specified.
   - **@c EISDIR ** The pathname is a directory and the flags was not equal to \c OPEN_RDONLY.
   - **@c ENODEV ** The pathname is a device special file and the actual device it describes does not exist.
   - **@c EROFS ** The filesystem is read-only and write access was requested.
   - **@c ENOMEM ** The kernel memory limit has been exceeded.
   - **@c ENOSPC ** The filesystem has no more roof for a new file
 */
Fid_t Open(const char* pathname, int flags);


typedef uint64_t Dev_t;

#define NO_DEVICE ((Dev_t) -1)
#define DEV_MAJOR(dev)  ((uint32_t) ((dev)>>32))
#define DEV_MINOR(dev)  ((uint32_t) (dev))

/**
	@brief Types of File System Entity (FSE).

	This enumeration lists the types of entities that are contained
	in a file system. 

	Note that this is just an indication of which API (system calls)
	are applicable to an entity. Since we support virtual file systems,
	the actual type of an entity can be any crazy thing. For example,
	a file system exposing kernel entities may represent a  process as
	a directory. Such correspondences are documented in the file system
	driver.
*/
typedef enum {
	FSE_DIR,
	FSE_FILE,
	FSE_DEV
} Fse_type;




/**
	@brief Record of the status of a File System entity.

	An object of this type can be filled using the \ref Stat()
	system call, returning details of a particular File System
	Entity.
*/
struct Stat {
	Dev_t 			st_dev;			/**< @brief Device id for device containing file */
	inode_t 		st_ino;			/**< @brief I-node number */
	Fse_type 		st_type;		/**< @brief File system entity type */
	unsigned int	st_nlink;		/**< @brief Number of hard links */
	Dev_t 			st_rdev;		/**< @brief Device id for a device entity */
	uintptr_t 		st_size;		/**< @brief Total size in bytes */
	uintptr_t 		st_blksize;		/**< @brief Block size for file system I/O */
	uintptr_t 		st_blocks;		/**< @brief Number of blocks allocated */
};


/**
	@brief Retrieve the status of a File System entity.

	Return the status of the file system entity at \c pathname.
*/
int Stat(const char* pathname, struct Stat* statbuf);

/**
	@brief Create a new link to an existing file.

	Note that both paths must belong to the same mounted filesystem.
	Also, \c pathname may not specify a directory. Adding links to a
	directory is not allowed.

	@param pathname the entity that is to be linked to a new path
	@param newpath the new path name for the entity
	@returns 0 on success, -1 on failure.
 */
int Link(const char* pathname, const char* newpath);


/**
	@brief Remove a link to a file and possibly delete it

	Note that \c pathname should not point to a directory.

	@param pathname the path to the link to be deleted
 */
int Unlink(const char* pathname);


/**
	@brief Create a directory.
 */
int MkDir(const char* pathname);


/**
	@brief Delete a directory, which must be empty.
 */
int RmDir(const char* pathname);


/**
	@brief Get the current working directory.

	The absolute pathname of the current working directory is saved
	into the provided buffer, which must be large enough. If it is
	not, an \c ERANGE error is returned. The process should retry with a
	larger buffer.

	@param buffer where the pathname of the cwd will be stored
	@param size the size of \c buffer
	@returns 0 on sucess and -1 in case of error.
	  Possible errors are:
	  - **@c ERANGE**  The provided buffer is too small for the cwd.
	  - **@c ENOENT**  The current working directory has been unlinked.
 */
int GetCwd(char* buffer, unsigned int size);



/**
	@brief Change the current working directory.

	@param pathname the new working directory
	@returns 0 on sucess and -1 in case of error.
	  Possible errors are:
	  - **@c ENOENT**  \c pathname does not exist.
	  - **@c ENAMETOOLONG**  \c pathname is too long.
	  - **@c ENOTDIR** Some component of \c pathname is not a directory.
	  - **@c EIO**  	An I/O error occurred.
 */
int ChDir(const char* pathname);


typedef struct mount_param 
{
	const char* param_name;
	const char* param_string;
} mount_param;


/**
	@brief Mount a device onto the filesystem.
 */
int Mount(Dev_t device, const char* mount_point, const char* fstype, unsigned int paramc, mount_param* paramv);

/**
	@brief Unmount the filesystem at a mount point.
  */
int Umount(const char* mount_point);

/**
	@brief Structure containing information about a mounted file system.
 */
struct StatFs
{
	Dev_t 			fs_dev;			/**< @brief Device storing file system */
	inode_t 		fs_root;		/**< @brief Root i-node */
	const char*		fs_fsys;		/**< @brief File system type */
	uintptr_t 		fs_blocks;		/**< @brief Total number of blocks */
	uintptr_t		fs_bused;		/**< @brief Number of used blocks */
	uintptr_t		fs_files;		/**< @brief Total number of files */
	uintptr_t		fs_fused;		/**< @brief Number of files used */
};


/**
	@brief Get status of file system
 */
int StatFs(const char* mount_point, struct StatFs* statfs);


/*******************************************
 *
 * Generic stream I/O
 *
 *******************************************/


/** 
  @brief Read bytes from a stream. 

   The @c buf and @c size arguments are, respectively, a buffer into which 
   input data can be placed and the  size  of that  buffer. 

   As its function result, the @c Read function should return the number 
   of bytes copied into @c buf, or @c -1 on error. The call may return fewer 
   bytes than @c size, but at least 1. A value of 0 indicates "end of file".

  @param fd  the file ID of the stream to read from
  @param buf pointer to a byte buffer to receive the read data
  @param size maximum size of @c buf
  @return the number of bytes copied, 0 if we have reached EOF, or -1, indicating some error.
        Possible errors are:
         - **@c EBADF** The file id is invalid.
         - **@c EINVAL** The file id is not suitable for reading.
         - **@c EIO** There was an I/O runtime problem.
         - **@c EINTR** The call was interrupted
 */
int Read(Fid_t fd, char *buf, unsigned int size);


/** @brief Write bytes to a stream.

   The @c buf and @c size arguments are, respectively, a buffer into which 
   input data can be placed and the  size  of that  buffer. 

   As its function result, the @c Read function should return the number 
   of bytes copied into @c buf, or @c -1 on error. The call may return fewer 
   bytes than @c size, but at least 1. 

   For terminals, the number of bytes copied should be equal to size.

  @param fd  the file ID of the stream to read from
  @param buf pointer to a byte buffer to receive the read data
  @param size maximum size of @c buf
  @return As its function result, the @c Write function should return the 
   number of bytes copied from @c buf, or -1 on error. 
   Possible errors are:
	- **@c EBADF** The file id is invalid.
	- **@c EINVAL** The file id is not suitable for writing.
	- **@c EIO** There was a I/O runtime problem.
	- **@c EINTR** The call was interrupted
 */
int Write(Fid_t fd, const char* buf, unsigned int size);


/** @brief Close a file id.
   

  @param fd the file ID to close
  @return  This call returns 0 on success and -1 on failure.
   Note that it is not an error to call Close on a (valid)
   file id which is already closed.
   Possible reasons for failure:
	- **@c EBADF** The file id is invalid.
	- **@c EIO** There was a I/O runtime problem.   
	- **@c EINTR** The call was interrupted
 */
int Close(Fid_t fd);


/** @brief Make a copy of a stream to a new file ID.

  If @c newfd is already in use by another file, it is first
  closed (unless @c oldfd==newfd, in which case nothing happens). 

  After the successful call, both oldfd and newfd refer to
  the same file object.

  @param oldfd the file id to copy from
  @param newfd the new file id.
  @return This call returns 0 on success and -1 on failure.
  Possible reasons for failure:
  - **@c EBADF** Either oldfd or newfd is invalid.
  - **@c EBADF** oldfd is not an open file.
 */
int Dup2(Fid_t oldfd, Fid_t newfd);

/*******************************************
 *
 * Pipes
 *
 *******************************************/


/**
	@brief A pair of file ids, describing a pipe.

	This structure is initialized by the @c Pipe() system call
	with two file descriptors, for the read and write ends of
	the pipe respectively. 
	Writing bytes to the write end using @c Write() will make them
	available at the read end, unsing @c Read().
*/
typedef struct pipe_s {
	Fid_t read;			/**< The read end of the pipe */
	Fid_t write;		/**< The write end of the pipe */
} pipe_t;


/**
	@brief Construct and return a pipe.

	A pipe is a one-directional buffer accessed via two file ids,
	one for each end of the buffer. The size of the buffer is 
	implementation-specific, but can be assumed to be between 4 and 16 
	kbytes. 

	Once a pipe is constructed, it remains operational as long as both
	ends are open. If the read end is closed, the write end becomes 
	unusable: calls on @c Write to it return error. On the other hand,
	if the write end is closed, the read end continues to operate until
	the buffer is empty, at which point calls to @c Read return 0.

	@param pipe a pointer to a pipe_t structure for storing the file ids.
	@returns 0 on success, or -1 on error. Possible reasons for error:
        - **@c EMFILE** The maximum number of per-process file descriptors has been reached.
        - **@c ENFILE** The maximum number of system-wide files has been reached.
*/
int Pipe(pipe_t* pipe);

/*******************************************
 *
 * Sockets (local)
 *
 *******************************************/

/**
	@brief A type for socket ports.

	A socket port is an integer between 1 and @c MAX_PORT.
*/
typedef int16_t port_t;

/**
	@brief the maximum legal port 
*/
#define MAX_PORT 1023

/**
	@brief a null value for a port
*/
#define NOPORT ((port_t)0)


/**
	@brief Return a new socket bound on a port.

	This function returns a file descriptor for a new
	socket object.	If the @c port argument is @c NOPORT, then the 
	socket will not be bound to a port. Else, the socket
	will be bound to the specified port. 

	@param port the port the new socket will be bound to
	@returns a file id for the new socket, or NOFILE on error. Possible
		reasons for error:
		- **@c EINVAL** the port is iilegal
        - **@c EMFILE** The maximum number of per-process file descriptors has been reached.
        - **@c ENFILE** The maximum number of system-wide files has been reached.
*/
Fid_t Socket(port_t port);

/**
	@brief Initialize a socket as a listening socket.

	A listening socket is one which can be passed as an argument to
	@c Accept(). Once a socket becomes a listening socket, it is not
	possible to call any other functions on it except @c Accept(), 
    @c Close() and @c Dup2().

	The socket must be bound to a port, as a result of calling @c Socket.
	On each port there must be a unique listening socket (although any number
	of non-listening sockets are allowed).

	@param sock the socket to initialize as a listening socket
	@returns 0 on success, -1 on error. Possible reasons for error:
		- **@c EBADF** the file id is not legal
		- **@c ENOTSOCK** the file id is not of a socket
		- **@c EOPNOTSUPP** the socket is not bound to a port
		- **@c EOPNOTSUPP** the socket has already been initialized
		- **@c EADDRINUSE** the port bound to the socket is occupied by another listener
	@see Socket
 */
int Listen(Fid_t sock);


/**
	@brief Wait for a connection.

	With a listening socket as its sole argument, this call will block waiting
	for a single @c Connect() request on the socket's port. 
	one which can be passed as an argument to @c Accept. 

	It is possible (and desirable) to re-use the listening socket in multiple successive
	calls to Accept. This is a typical pattern: a thread blocks at Accept in a tight
	loop, where each iteration creates new a connection, 
	and then some thread takes over the connection for communication with the client.

	@param lsock the socket to initialize as a listening socket
	@returns a new socket file id on success, @c NOFILE on error. Possible reasons 
	    for error:
		- **@c EBADF** the file id is not legal
		- **@c ENOTSOCK** the file id is not of a socket
		- **@c EINVAL** the file id is not initialized by @c Listen()
        - **@c EMFILE** The maximum number of per-process file descriptors has been reached.
        - **@c ENFILE** The maximum number of system-wide files has been reached.
		- **@c EAGAIN** while waiting, the listening socket @c lsock was closed
        - **@c EINTR** The call was interrupted
	@see Connect
	@see Listen
 */
Fid_t Accept(Fid_t lsock);



/**
	@brief Create a connection to a listener at a specific port.

	Given a socket @c sock and @c port, this call will attempt to establish
	a connection to a listening socket on that port. If sucessful, the
	@c sock stream is connected to the new stream created by the listener.

	The two connected sockets communicate by virtue of two pipes of opposite directions, 
	but with one file descriptor servicing both pipes at each end.

	The Connect call will block for approximately the specified amount of time.
	The resolution of this timeout is implementation specific, but should be
	in the order of 100's of msec. Therefore, a timeout of at least 500 msec is
	reasonable. If a negative timeout is given, it means, "infinite timeout".

	@param sock the socket to connect to the other end
	@param port the port on which to seek a listening socket
	@param timeout the approximate amount of time to wait for a
	        connection.
	@returns 0 on success and -1 on error. Possible reasons for error:
		- **@c EBADF** the file id is not legal
		- **@c ENOTSOCK** the file id is not of a socket or is a listening socket		
		- **@c EISCONN** the file id @c sock is already connected		
	   	- **@c EINVAL** the given port is illegal.
	   	- **@c ECONNREFUSED** the port does not have a listening socket bound to it by @c Listen.
	   	- **@c ETIMEDOUT** the timeout has expired without a successful connection.
        - **@c EINTR** The call was interrupted
*/
int Connect(Fid_t sock, port_t port, timeout_t timeout);


/**
   @brief Socket shutdown modes.

   These constants define the legal values for passing the second argument to
   the @c ShutDown call.

   @see ShutDown
*/
typedef enum {
  SHUTDOWN_READ=1,    /**< Shut down the read direction. */
  SHUTDOWN_WRITE=2,   /**< Shut down the write direction. */
  SHUTDOWN_BOTH=3     /**< Shut down both directions. */
} shutdown_mode;



/**
   @brief Shut down one direction of socket communication.

   With a socket which is connected to another socket, this call will 
   shut down one or the other direction of communication. The shut down
   of a direction has implications similar to those of a pipe's end shutdown.
   More specifically, assume that this end is socket A, connected to socket
   B at the other end. Then,

   - if `ShutDown(A, SHUTDOWN_READ)` is called, any attempt to call `Write(B,...)`
     will fail with a code of -1.
   - if ShutDown(A, SHUTDOWN_WRITE)` is called, any attempt to call `Read(B,...)`
     will first exhaust the buffered data and then will return 0.
   - if ShutDown(A, SHUTDOWN_BOTH)` is called, it is equivalent to shutting down
     both read and write.

   After shutdown of socket A, the corresponding operation `Read(A,...)` or `Write(A,...)`
   will return -1.

   Shutting down multiple times is not an error.
   
   @param sock the file ID of the socket to shut down.
   @param how the type of shutdown requested
   @returns 0 on success and -1 on error. Possible reasons for error:
		- **@c EBADF** the file id @c sock is not legal
		- **@c ENOTSOCK** the file id @c sock is not of a socket or is a listening socket
		- **@c ENOTCONN** the file id @c sock is already connected
	   	- **@c EINVAL** the shutdown mode @c how is illegal.
*/
int ShutDown(Fid_t sock, shutdown_mode how);



/*******************************************
 *
 * System information
 *
 *******************************************/

/**
  @brief The max. size of args returned by a procinfo structure.
  */
#define PROCINFO_MAX_ARGS_SIZE (128)

/**
	@brief A struct containing process-related information for a non-free
	pid.

	This structure is returned by information streams.
	@see OpenInfo
  */
typedef struct procinfo
{
	Pid_t pid;	    /**< @brief The pid of the process. */
	Pid_t ppid;     /**< @brief The parent pid of the process.

                This is equal to NOPROC for parentless procs. */
  
  int alive;      /**< @brief Non-zero if process is alive, zero if process is zombie. */
	
  unsigned long thread_count; /**< Current no of threads. */
	
  Task main_task;  /**< @brief The main task of the process. */
	
  int argl;        /**< @brief Argument length of main task. 

            Note that this is the
            real argument length, not just the length of the @c args field, which is
            limited at @c PROCINFO_MAX_ARGS_SIZE. */
	char args[PROCINFO_MAX_ARGS_SIZE]; /**< @brief The first 
       @c PROCINFO_MAX_ARGS_SIZE bytes of the argument of the main task. 

    If the task's argument is longer (as designated by the @c argl field), the
    bytes contained in this field are just the prefix.  */
} procinfo;


/**
	@brief Open a kernel information stream.

	This is a read-only stream that returns a sequence of 
	@c procinfo structures,
	each packed into a block of size @c sizeof(procinfo).

	Each procinfo structure contains information pertaining to some
	used PCB (active or zombie) during the time of the stream. 

	There is no guarantee of the timeliness of the information.
	A best-effort approach to return relevant system information is
	made. 

	@returns a file id on success, or NOFILE on error. Possible reasons
		for error are:
        - **@c EMFILE** The maximum number of per-process file descriptors has been reached.
        - **@c ENFILE** The maximum number of system-wide files has been reached.

 */
Fid_t OpenInfo();




/*******************************************
 *
 * System boot
 *
 *******************************************/

/** @brief Boot tinyos3. 

   The function must initialize the simulated computer with the given number of
   cpu cores and terminals, initialize tinyos and then execute
   the initial process using function boot_task with parameters argl and args. 
   The boot_task execution can then create more processes.

   When the boot_task process finishes, this call halts and cleans up TinyOS structures 
   and then returns. 
   */
void boot(unsigned int ncores, unsigned int terminals, Task boot_task, int argl, void* args);


/** @} */

#endif
