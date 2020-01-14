#ifndef __KERNEL_DEV_H
#define __KERNEL_DEV_H

/*****************************
 *
 *  The Device Table    
 *
 *****************************/ 

#include "util.h"
#include "bios.h"
#include "tinyos.h"
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
  @brief Device control block.

  These objects hold the information that is needed to 
  access a particular device type. There is one such object for each
  major device number. The device table is just an array of pointers
  to such objects.

*/
typedef struct device_control_block
{

	/** @brief Device type. 
    
		Much like 'major number' in Unix. It determines the driver.
		This number is also the index of this object in @c device_table.
  	*/
  	devnum_t type;     


	/**
		@brief Initialize the device type at boot.

		This function is called at boot time to initialize the device type.
	*/
  	void (*Init)();

  	/**
  		@brief Finalize the device type at shutdown.

  		This function is called during shutdown to finalize the device type.
  	 */
  	void (*Fini)();

	/**
		@brief Return a stream object on which the other methods will operate.

		This function is passed the minor number of the device to be accessed.
		It returns a stream object suitable for this device.

		This stream object will be used with the routines stored in \c dev_fops.
		A \c NULL return is considered an error.

		@param minor the minor number of the device to open
		@return a stream object or \c NULL 
	*/
	void* (*Open)(devnum_t minor);

	/**
		@brief Create a new device of this type
		
		If successful, this call will create a new device under this driver.

		The values of `*minor` and \c name are suggestions to the call,
		that the created device have this minor number and that it will be 
		published under the given name. The use of this information is up to the
		device driver.

		If a device was successfully created, the minor number is stored in `*minor`.

		@param minor a suggested minor number to create
		@param name a suggested name under which the device will be published
		@param config an additional argument, whose meaning is device-specific
		@return 0 for success or an error code if non-zero.
	 */
	int (*Create)(devnum_t* minor, const char* name, void* config);

	/**
		@brief Destroy a given device of this type

		If successful, this call will destroy a device of this type. 
		For this to be possible, it must be that the device driver supports device
		destruction (e.g., perhaps this is a plug-and-play device).

		@param minor the minor number for the device to be destroyed
		@return 0 on success or an error code for failure
	 */
	int (*Destroy)(devnum_t minor);

	/** 
		@brief Number of devices for this major number.  
	*/
	devnum_t devnum;

	/** @brief File operations for this device.

		This structure contains methods provided by the device driver. 
	*/
	file_ops dev_fops;
} DCB;


/**
	@brief The device table.

	This is an array of device control blocks, one for each device type.
  */
extern DCB* device_table[MAX_DEV];

/**
	@brief Add a new device type to the device table.

	Note that this function is usually called implicitly in macro
	@ref REGISTER_DRIVER.

	@param dcb The device control block of the new device.
	@see REGISTER_DRIVER
 */
void register_driver(DCB* dcb);


/**
	@brief Publish a device to the device directory

	Make a device available via @c devname. The device file system
	will publish the device.

	The API does not check for name collisions; if multiple devices
	are published under the same name, the results are unpredictable
	(to say the least).

	@param devname the name of the device
	@param major the major device number
	@param minor the minor device number
 */
void device_publish(const char* devname, devnum_t major, devnum_t minor);


/**
	@brief Remove a device from the device directory

	Remove a device from the device directory

	@param devname the name of the device to retract
 */
void device_retract(const char* devname);


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
static void __add_ ## dcb ##_to_device_table() { register_driver(&dcb); }

/** 
  @brief Initialization for devices.

  This function is called at kernel boot.
 */
void initialize_devices();


/** 
  @brief Initialization for devices.

  This function is called at kernel shutdown.
 */
void finalize_devices();


/**
  @brief Open a device.

  This function opens a device by major and minor number, and
  returns a stream object, storing its pointer in @c obj, and a
  @c file_ops record (storing it in @c ops).


  @param major the device major number
  @param minor the device minor number
  @param obj location where a stream object is stored on success
  @param ops location where stream operations will be stored on success
  @return 0 on success and an error code on failure. 
  Standard error codes are:
   - \c ENXIO the device does not exist
   - \c ENODEV  the device does not support this operation
   Other error codes are device-specific
*/
int device_open(devnum_t major, devnum_t minor, void** obj, file_ops** ops);


/**
	@brief Create a new device.

	If successful, this call will create a new device of the given major number.

	The values of `*minor` and \c name are suggestions to the call,
	that the created device have this minor number and that it will be 
	published under the given name. The use of this information is up to the
	device driver.

	If a device was successfully created, the minor number is stored in `*minor`.

	@param major the major number of the device to create
	@param minor position to store the minor number of a suggested minor number to create
	@param name a suggested name under which the device will be published
	@param config an additional argument, whose meaning is device-specific
	@return 0 for success or an error code if non-zero.
		Standard error codes are:
		- \c ENXIO the device does not exist
		- \c ENODEV  the device does not support this operation
		Other error codes are device-specific
 */
int device_create(devnum_t major, devnum_t* minor, const char* name, void* config);


/**
	@brief Destroy a device.

	If successful, this call will destroy the given device.

	@param major the major number of the device to destroy
	@param minor the minor number of the device to destroy
	@return 0 for success or an error code if non-zero.
 */
int device_destroy(devnum_t major, devnum_t minor);


/**
  @brief Get the number of devices of a particular major number.

  The number of devices M determines the legal range of minor numbers,
  namely 0<= minor < M.
  */
uint device_no(devnum_t major);

/** @} */

#endif
