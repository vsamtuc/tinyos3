#ifndef __KERNEL_IO_H
#define __KERNEL_IO_H

#include <stdint.h>

/**
  @brief The device-specific file operations table.

  This object contains pointers to device-specific functions for I/O. 
  Device drivers and other resource managers which expose a stream 
  interface, must implement all or some of these methods. 

  The first argument of each method is an object taken from the 'streamobj'
  field of the FCB.

  @see FCB
 */
typedef struct file_operations {


  /** @brief Read operation.

    Read up to 'size' bytes from stream 'this' into buffer 'buf'. 
    If no data is available, the thread will block, to wait for data.
    The Read function should return the number of bytes copied into buf, 
    or -1 on error. The call may return fewer bytes than 'size', 
    but at least 1. A value of 0 indicates "end of data".

    Possible errors are:
    - There was a I/O runtime problem.
  */
    int (*Read)(void* this, char *buf, unsigned int size);

  /** @brief Write operation.

    Write up to 'size' bytes from 'buf' to the stream 'this'.
    If it is not possible to write any data (e.g., a buffer is full),
    the thread will block. 
    The write function should return the number of bytes copied from buf, 
    or -1 on error. 

    Possible errors are:
    - There was a I/O runtime problem.
  */
    int (*Write)(void* this, const char* buf, unsigned int size);

    /** @brief Close operation.

      Release the stream object, deallocating any resources held by it.
      This function returns 0 is it was successful and -1 if not.
      Although the value in case of failure is passed to the calling process,
      the stream should still be destroyed.

    Possible errors are:
    - There was a I/O runtime problem.
     */
    int (*Release)(void* this);

	/**
		@brief Change the size of the stream.

		Change the size of the underlying object. For example, in case of a disk file,
		this function will change the size of the file.

		On success 0 is returned. On error, -1 is returned.
	*/
	int (*Truncate)(void* this, intptr_t size);


	/**
		@brief Move the current position of the stream.

		Some types of streams allow for random access. This call moves the current position 
		of the stream by `offset`, starting from the position indicated by `whence`.

		The `whence` parameter can be one of:
		- 0  means __set the current position to `offset`__.
		- 1  means __change the current position by adding `offset`__. Note that `offset` can be
		  negative
		- 2  means __set the current position to the size of the file plus `offset`__.

		The function returns the current position, which is always non-negative.

		On error, -1 is returned.
	*/
	intptr_t (*Seek)(void* this, intptr_t offset, int whence);



} file_ops;




#endif