#ifndef __KERNEL_DEV_H
#define __KERNEL_DEV_H

/*****************************
 *
 *  The Device Table    
 *
 *****************************/ 

#include "util.h"
#include "bios.h"
#include "kernel_io.h"

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

	The device table is an array of Device Control Block (`DCB`) pointers.
	Each DCB object corresponds to a type of device, thus to a unique major number.

	The `DCB` contains the number of devices for this type. It also contains 
	pointer to a constructor function for file objects, and a pointer to a `file_ops` object. 
	These routines are provided by the driver code for this device.

	To declare a new device type, one should define the `file_ops` routines for this device,
	as well as a constructor function and an initialization function. 
	Finally, one has to register the device type to the device table. An example of a trivial device
	is shown in the definition of the 'null device' in file `kernel_dev.c`.


  @{ 
*/


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

	/** @brief Device type. 
    
		Much like 'major number' in Unix, determines the driver. 
  	*/
  	Device_type type;     


	/**
		@brief Initialize the device type at boot.

		This function is called at boot time to initialize the device type.
	*/
  	void (*Init)();

	/**
		@brief Return a stream object on which the other methods will operate.

		This function is passed the minor number of the device to be accessed.
		It returns a stream object suitable for this device.
	*/
	void* (*Open)(uint minor);

	uint devnum;		/**< @brief Number of devices for this major number. */

	/** @brief File operations for this device.

		This structure contains methods provided by the device driver. 
	*/
	file_ops dev_fops;
} DCB;


/**
	@brief The device table.

	This is an array of device control blocks, one for each device type.
  */
extern DCB* device_table[DEV_MAX];

/**
	@brief Add a new device type to the device table.

	Note that this function is usually called implicitly in macro
	@ref REGISTER_DRIVER.

	@param dcb The device control block of the new device.
	@see REGISTER_DRIVER
 */
void register_device(DCB* dcb);

/**
	@brief Designate a static DCB object to add to the device table.

	Use this macro once for each static DCB object. The gcc compiler and
	linker will add a pointer to this DCB object into the right position
	in `device_table`.

	Note that at least the `type` field of the static DCB object must
	be initialized.
  */
#define REGISTER_DRIVER(dcb) \
__attribute__((constructor)) \
static void __add_ ## dcb ##_to_device_table() { register_device(&dcb); }

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
