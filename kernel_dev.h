#ifndef __KERNEL_DEV_H
#define __KERNEL_DEV_H

/*****************************
 *
 *  The Device Table    
 *
 *****************************/ 

#include "util.h"
#include "bios.h"

/**
  @file kernel_dev.h
  @brief Device management.

  @defgroup dev Devices
  @ingroup kernel
  @brief Device management.

  The device model of tinyos3 is similar to that of Unix.
  Each device is designated by a pair of numbers (Major,Minor).
  The Major number determines the driver routines related to
  the device. The Minor number is used to specify one among
  several devices of the same Major number. For example,
  device (DEV_SERIAL,2) is the 3rd serial terminal.

  The device table lists the devices by major number, and gives
  the number of devices for this type. It also contains 
  a pointer to a file_ops object, which contains driver routines
  for this device.

  @{ 
*/


/**
  @brief The device-specific file operations table.

  This object contains pointers to device-specific functions for I/O. 
  Device drivers and other resource managers which expose a stream 
  interface, must implement these methods. 

  The first argument of each method is taken from the 'streamobj'
  field of the FCB.
  @see FCB
 */
typedef struct file_operations {

	/**
		@brief Return a stream object on which the other methods will operate.

		This function is passed the minor number of the device to be accessed.
	*/
  	void* (*Open)(uint minor);


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

      Close the stream object, deallocating any resources held by it.
      This function returns 0 is it was successful and -1 if not.
      Although the value in case of failure is passed to the calling process,
      the stream should still be destroyed.

    Possible errors are:
    - There was a I/O runtime problem.
     */
    int (*Close)(void* this);
} file_ops;



/**
  @brief The device type.
	
  The device type of a device determines the driver used.
*/
typedef enum { 
	DEV_NULL,    /**< @brief Null device */
	DEV_SERIAL,  /**< @brief Serial device */
	DEV_MAX      /**< @brief placeholder for maximum device number */
}  Device_type;


/**
  @brief Device control block.

  These objects hold the information that is needed to 
  access a particular device.
*/
typedef struct device_control_block
{
  Device_type type;     /**< @brief Device type. 

                            Much like 'major number' in Unix, determines the driver. */
  
  uint devnum;           /**< @brief Number of devices for this major number.
                          */

  file_ops dev_fops;	/**< @brief Device operations

  							This structure is provided by the device driver. */
} DCB;


/** 
  @brief Initialization for devices.

  This function is called at kernel startup.
 */
void initialize_devices();


/**
  @brief Open a device.

  This function opens a device by major and minor number, and
  returns a stream object, storing its pointer in @c obj, and a
  @c file_ops record (storing it in @c ops).

  It returns 0 on success and -1 on failure.
  */
int device_open(Device_type major, uint minor, void** obj, file_ops** ops);

/**
  @brief Get the number of devices of a particular major number.

  The number of devices M determines the legal range of minor numbers,
  namely 0<= minor < M.
  */
uint device_no(Device_type major);

/** @} */

#endif
