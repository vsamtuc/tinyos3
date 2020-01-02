#ifndef __KERNEL_FS_H
#define __KERNEL_FS_H

#include "util.h"
#include "tinyos.h"
#include "kernel_io.h"



/*----------------------------------
 *
 * Path management
 *
 *----------------------------------*/

typedef char pathcomp_t[MAX_NAME_LENGTH+1];




/* These two types are just placeholders for actual void pointers */

#if !defined(MOUNT)
struct GENERIC_MOUNT { void* ptr; };
#define MOUNT struct GENERIC_MOUNT
#endif


/* A bunch of flags for the fields of interest in a call to Status */
#define STAT_DEV	(1<<0)
#define STAT_INO	(1<<1)
#define STAT_TYPE	(1<<2)
#define STAT_NLINK	(1<<3)
#define STAT_RDEV	(1<<4)
#define STAT_SIZE	(1<<5)
#define STAT_BLKSZ	(1<<6)
#define STAT_BLKNO	(1<<7)
#define STAT_ALL	((1<<8)-1)
#define STAT_NAME	(1<<8)


/**
	@brief A filesystem driver.

	This object contains the Virtual File System APi that is not part of the
	file_ops structure. For each type of file system, there exists a unique such object,
	which is usually static.
	
	FSystem objects are retrieved by name using function @ref get_fsys().
	The @ref REGISTER_FSYS() macro can be used to register file systems from different
	files.	

	### i-nodes ###

	A file system is a linked network of i-nodes. An i-node is an object that represents
	a file system entity, such as a file, a directory, etc. An i-node is normally stored 
	in non-volatile memory (e.g., a disk), but some i-nodes are loaded in RAM. Every i-node
	is identified by an integer id of type @c inode_t.

	I-nodes are created with a specific @c Fse_type and this type cannot change. However, 
	if an i-node is deleted, the i-node id of the deleted i-node may be given to a new i-node.
	This may cause difficult race conditions. To manage this, an i-node can be  __pinned__.
	The file system should never delete a pinned i-node; however, it may update the i-node
	so that it will be deleted once it is __unpinned__. Also, even though it is pinned, an i-node
	may be disconnected from the rest of the file system (i.e., become unlinked).

	### Mounting a file system ###

	To instantiate a file system in the kernel, we say we __mount__ the file system. 
	To mount a file system, it is necessary to provide a file system driver, that is, a pointer
	to @c FSystem. Also, if the file system is stored on some disk or other non-volatile memory,
	a device needs to be provided.

	When a file system is mounted, an object is created in RAM. A pointer to this object is
	returned by the @c Mount method. Every other file system operation requires this pointer to
	be provided as the first argument. When the file system is no longer needed, it __must be__
	__unmounted__ using the @c Unmount method. If a file system is not unmounted properly, data
	corruption may ensue, probably losing some data and possibly even making the non-volatile
	storage unusable.

	### Opening streams on i-nodes ###

	The \c Open method can be used to obtain a stream on a file system entity. Depending on the type
	of entity, this stream can be read and/or written using the @c file_ops stream API. An opened stream
	on an i-node has the effect of pinning the i-node; the i-node can be unlinked but will only be
	deleted if all the stream handles are closed.

	### Scanning directories ###

	A read-only stream can be obtained on a directory. This stream will return the names contained
	in the directory. The stream will be a sequence of bytes obtained by concatenating an encoded
	name for each directory entry.  Each name will be prefixed by two hexadecimal ASCII digits, denoting
	the length of the subsequent name and will be suffixed by the byte 0. 

	For example, assume that a directory contains three entries, named 'foo', 'subdir' and 'Big Text File'.
	Then, the contents of the stream will be
	\code	
	"01.*02..*03foo*06subdir*0DBig Text File*"
	\endcode  
	where '*' in the above stands for the 0 byte.

	Note that the 

	### API notes ###

	All file system operations return an integer. If the operation succeeded, the return is 0.
	If the operation failed, the return is an error code that can be passed to @c set_errcode.


	@see enum Fse_type
	@see inode_t
	@see set_errcode
  */
struct FSystem_type
{
	const char* name;

	/** @brief Mount a file system of this type. 

		This call will mount a file system of this type, storing a pointer to the mount object
		in @c mnt. Storage for the file system is provided by @c dev, unless the file system
		type does not use devices, in which case @c dev should be @c NO_DEVICE. A number of
		parameters may be passed in the @c param_vec array (of size @c param_no).


		@param mnt a pointer to the mount object pointer, that will be set to point to a new
		object on success
		@param dev a device id providing storage for the file system. For some file system types
         this must be @c NO_DEVICE
        @param param_no the number of mount parameters
        @param param_vec an array of mount parameters of size @c param_no
		@return 0 on success, or an error code. Possible errors:
		 - @c ENXIO the major number of the provided device is illegal or out of range.
		 - @c EBUSY the device is already mounted
		 - @c EINVAL the device does not have the right format
		 - @c ENODEV the file system type is not supported
	*/
	int (*Mount)(MOUNT* mnt, Dev_t dev, 
				unsigned int param_no, mount_param* param_vec);


	/** @brief Unmount a particular mount.

		This call will unmount a particular mounted file system, updating non-volatile
		memory before it returns. In order for this call to succeed, the mounted file
		system must not have any pinned i-nodes. 

		@param mnt the mount object
		@return 0 on success, or an error code. Possible errors:
		- @c EBUSY cannot unmount because the file system is busy (has pinned i-nodes)
	 */
	int (*Unmount)(MOUNT mnt);


	/**
		@brief Return the status of the file system. 

		Returns information about a mounted file system. In
		particular, this call returns the \c inode_t  of the
		file system root.

		@param mnt the mounted file system
		@param statfs buffer to fill in the mount information.
		@return 0 on success, this call never fails
	 */
	int (*StatFs)(MOUNT mnt, struct StatFs* statfs);


	/**
		@brief Pin i-node.

		Mark @c ino as pinned. A pinned i-node cannot be deleted, therefore the i-node id
		will refer to the same object until the i-node is unpinned. 
		The i-node may be already pinned, in which case the call has no effect.

		@param mnt the mount object
		@param ino the i-node to pin
		@return 0 on success, or an error code. Possible errors:
		- @c ENOENT the i-node does not exist
	  */
	int (*Pin)(MOUNT mnt, inode_t ino);


	/**
		@brief Unpin an i-node.

		Mark i-node @c ino as unpinned. An unpinned i-node may be deleted and its i-node
		id can be reused. @ino may be already unpinned, in which case the call
		has no effect

		@param mnt the mount object
		@param ino the i-node to unpin
		@return 0 on success, or an error code. Possible errors:
		- @c ENOENT the i-node does not exist
	  */
	int (*Unpin)(MOUNT mnt, inode_t ino);


	/**
		@brief Save the i-node and related data to disk.

		The entity referred to by the inode will be saved to storage. This includes the
		inode itself as well as any data represented by this i-node (e.g., a file's content).

		The thread calling this may block until the whole data is saved on disk. Note that,
		unless @c ino is pinned, by the time this call returns, @c ino may be deleted.

		@param mnt the mount object
		@param ino the i-node to flush
		@return 0 on success, or an error code. Possible errors:
		- @c ENOENT the i-node does not exist
		- @c EINTR the operation was interrupted by a signal
		- @c EIO an I/O error occurred while accessing the file system
	 */
	int (*Flush)(MOUNT mnt, inode_t ino);


	/** @brief Create a new file system entity in a directory.

		Create a new object in @c dir, called @c name. If successful, store
		the inode of the new object in @c newdir. The new i-node will be unpinned.

		File system objects include the following:
		- directories
		- files
		- devices
		- symbolic links
		- named pipes
		- named sockets

		Currently, only files, directories and devices are supported by the API.

		The @c data parameter is only used for special files, currently devices. In
		this case, it must point to an object of type @c Dev_t. 

		@param mnt the mount object
		@param dir a directory i-node which contains the new object
		@param name the name of the new object
		@param type the file system entity type for the new object
		@param newino location where the new object i-node will be stored
		@param data data that is used to define the new special object
		@return 0 on success, or an error code. Possible errors:
		- @c ENOENT @c dir i-node does not exist
		- @c ENOTDIR @c dir is not a directory i-node
		- @c EEXIST there is already an entry in @c dir called @c name
		- @c EINVAL the @c name contains illegal characters or is empty
		- @c ENOSPC the device has no space for a new directory
		- @c EROFS the file system is read-only 
		- @c EPERM the file system does not support this type of special file
		- @c EIO     An I/O error occurred while accessing the file system
 	*/
	int (*Create)(MOUNT mnt, inode_t dir, const pathcomp_t name, Fse_type type, inode_t* newino, void* data);


	/**
		@brief Search a directory for a particular entry by name, possibly creating it.

		If successful, this call stores an i-node id into @c ino, which corresponds to a
		directory entry in @c dir, called @c name. The @c createflag argument can be used to 
		create such an entry atomically, if one did not exist at the time of the call.
		The new entry will be a regular file; other file system entities cannot be created.
		If the fetched i-node was created, it will be unpinned.

		@param mnt the mount object
		@return 0 on success, or an error code. Possible errors:
		- @c ENOENT @c dir i-node does not exist, or @c createflag is 0 and @c dir
		    does not have an entry called @c name.
		- @c ENOTDIR @c dir is not a directory i-node
		- @c EINVAL the @c name contains illegal characters or is empty
		- @c ENOSPC the device has no space for a new file but one was to be created
		- @c EROFS the file system is read-only but a file was to be created
		- @c EIO     An I/O error occurred while accessing the file system
	 */
	int (*Fetch)(MOUNT mnt, inode_t dir, const pathcomp_t name, inode_t* ino, int createflag);


	/**
		@brief Open a stream on an inode. 
	
		Return a stream object and a @c file_ops object that can be used to initialize
		a File Control Block (FCB) so that standard stream operations (e.g., @c Read(),
		@c Write(), etc) can be performed.

		The @c flags argument is a bitwise-OR of @c Open_flags enumeration constants. Only
		the status flags are supported.

		It is not necessary for @c ino to be pinned for this call to succeed. Once a stream
		is opened on an i-node, it retains the i-node (effectively pinning it) until the file
		handle is closed.

		@see enum Open_flags
		@param mnt the mount object
		@param ino the i-node to open a stream on
		@param flags 
		@return 0 on success, or an error code. Possible errors:
		- @c EISDIR  The pathname is a directory and the flags was not equal to \c OPEN_RDONLY.
   		- @c ENODEV  The pathname is a device special file and the actual device it describes does not exist.
		- @c EROFS   The filesystem is read-only and write access was requested.
		- @c ENOMEM  The kernel memory limit has been exceeded.
		- @c ENOSPC  The filesystem has no more roof for a new file
		- @c EIO     An I/O error occurred while accessing the file system
	*/
	int (*Open)(MOUNT mnt, inode_t ino, int flags, void** obj, file_ops** ops);

	
	/**
		@brief Add a hard link to an i-node.

		Add a new directory entry to @c dir, called @c name, pointing to 
		i-node @c ino. The @c ino i-node must not be a directory. 

		@param mnt the mount object
		@param dir the directory in which the link is created
		@param name the name of the new link
		@param ino the linked i-node
		@return 0 on success, or an error code. Possible errors:
		- @c ENOTDIR @c dir does not refer to a directory
		- @c ENOENT @c dir is not a valid i-node or @c ino is not a valid i-node
		- @c EEXIST @c name already exists in @c dir
		- @c EINVAL @c name contains illegal characters or is empty
		- @c EIO  an I/O error occurred while accessing the file system
		- @c EPERM the file system does not support hard links or @c ino is a directory
		- @c EROFS the file system is read-only
		- @c ENOSPC there is not enough space on the device for the new directory entry
	 */
	int (*Link)(MOUNT mnt, inode_t dir, const pathcomp_t name, inode_t ino);

	/**
		@brief Remove a hard link from an entity.
		
		Remove a link to the entity called @c name in directory @c dir. This reduces
		the number of links to this entity. If the number of links drops to zero, the
		entity will be deleted, unless it is pinned. If it is pinned, it will not be deleted
		and can in fact be linked to a new entry.

		If @c name refers to a directory, then the directory must be empty, or the call fails.
		If the directory is empty, it will be deleted immediately, unless it is pinned.

		@param mnt the mount object
		@param dir the directory containing the removed link
		@param name the name of the link
		@return 0 on success, or an error code. Possible errors:
		- @c ENOENT @c dir is an invalid inode
		- @c ENOTDIR @c dir does not refer to a directory
		- @c EIO an I/O error occurred while accessing the file system
		- @c EBUSY the entity cannot be unlinked because it is busy (e.g., a mount point, 
		        or the root directory)
		- @c ENOTEMPTY the entity is a non-empty directory
		- @c EROFS the file system is read-only
	 */
	int (*Unlink)(MOUNT mnt, inode_t dir, const pathcomp_t name);


	/**
		@brief Truncate a file to a specified length.

		This call will cause the regular file @c fino to be truncated to a size of
		exactly @c length bytes.

		@param mnt the mount object
		@param fino the regular file to be truncated
		@param length the new length of the file
		@return 0 on success, or an error code. Possible errors:
		- @c ENOENT @c fino is an invalid inode
		- @c EISDIR @c fino does not refer to a regular file
		- @c EINVAL @c length is negative or larger than the maximum file size
		- @c EIO an I/O error occurred while accessing the file system
		- @c EROFS the file system is read-only		
	 */
	int (*Truncate)(MOUNT mnt, inode_t fino, intptr_t length);

	/**
		@brief Return the status information of an i-node.

		Return status information on an i-node. The information is returned in
		the fields of object \c status. The \c which flag indicates the information
		actually requested.

		For some file systems, actually returning some status information may be
		expensive. Therefore, the \c which flag designates that only some information
		may be needed.

		The constant \c STAT_ALL returns the full \c struct Stat information, to the best of the
		file system's ability.

		In addition, if the \c STAT_NAME flag is provided  (note: it is not included 
		in \c STAT_ALL), and @c ino is a directory, then the name of this directory is
		stored in @c name. 

		@param mnt the mount object
		@param ino the i-node whose status is returned
		@param status the object where the status will be stored
		@param name a path where the name of the directory will be stored, or @c NULL
		@param which a bitwise-OR of flags 
		@return 0 on success, or an error code. Possible errors:
		- @c ENOENT @c ino is not a valid i-node
		- @c ENOTDIR @c ino is not a directory but @c STAT_NAME was included in @c which
		- @c EINVAL an invalid flag was specified in flags
	 */
	int (*Status)(MOUNT mnt, inode_t ino, struct Stat* status, pathcomp_t name, int which);

};



/*----------------------------------
 *
 * Virtual File system declarations
 *
 *----------------------------------*/

typedef struct InodeHandle Inode;
typedef struct FsMount FsMount;
typedef struct FSystem_type FSystem;

#define FSYS_MAX 16

void register_file_system(FSystem*);
extern FSystem* file_system_table[FSYS_MAX];

#define REGISTER_FSYS(fsys) \
__attribute__((constructor)) \
static void __add_ ## fsys ##_to_file_system_table() { register_file_system(&fsys); }


/**
	@brief Return a all file system names.

	This function returns a `NULL`-terminated array of strings containing
	all file system names.
 */
const char* const * get_filesystems();


/**
	@brief Return a filesystem by name, or NULL if not found.
*/
FSystem* get_fsys(const char* fsys_name);


/*----------------------------------
 *
 * Mount declarations.
 *
 *----------------------------------*/

#define MOUNT_MAX 32

struct FsMount
{
	/* Counts users of this mount */
	unsigned int refcount;

	/* The file system */
	FSystem* fsys;

	/* The underlying device */
	Dev_t device;

	/* Inode on top of which we mounted. This is NULL for the root mount only. */
	Inode* mount_point;

	/* Inode of our root directory */
	inode_t root_dir;

	/* List of all submounts */
	rlnode submount_list;
	rlnode submount_node;  /* Intrusive node */

	/* This is data returned by the file system for this mount */
	MOUNT fsmount;
};


extern FsMount mount_table[MOUNT_MAX];
FsMount* mount_acquire();
void mount_incref(FsMount* mnt);
void mount_decref(FsMount* mnt);



/*----------------------------------
 *
 * Inodes
 *
 *----------------------------------*/


/**
	@brief Handle to an i-node.

	I-node handles make operations on i-nodes more convenient. The main
	benefits of @c Inode handles are:
	- Each i-node is kept together with the mounted file system it belong to
	- Each @c Inode handle holds its i-node pinned, therefore it is assurred that
	  the i-node will not be deleted as long as a handle exists on it. To 
	  implement this, the handle employs reference counting.

	It is important to ensure that there is a unique @c Inode handle for each i-node.
	This is ensured by using a hash table to register @c Inode handles. 

	To obtain an @c Inode handle we use the @ref pin_inode() call. Once a handle is
	obtained it can be re-pinned using @ref repin_inode() and unpinned using @ref unpin_inode().
	The handle keeps a count of the number of pins and is only released when the number
	of pins drops to zero.
*/
typedef struct InodeHandle
{
	unsigned int pincount;  /**< @brief Reference-counting uses to this inode handle */
	FsMount* ino_mnt;		/**< @brief Inode filesystem */
	inode_t ino_id;		    /**< @brief Inode number */
	rlnode inotab_node;   	/**< @brief Used to add this to the inode table */

	/** @brief Points to a mount whose mount point is this directory, or NULL */
	struct FsMount* mounted;
} Inode;


/**
	@brief Turn an i-node reference into a handle.
	
	This call returns a pointer to an \c Inode handle for the
	given i-node. The caller should call @ref unpin_inode when the 
	handle no longer needed.

	If there is an error, \c NULL is returned and an error code is stored
	with @ref set_errcode().

	@param mnt the mounted file system
	@param id the i-node to pin
	@return a pointer to a handle, or \c NULL in case of error.
*/
Inode* pin_inode(FsMount* mnt, inode_t id);


/**
	@brief Add a pin to an already pinned i-node.

	This call increases the number of holders of the `inode` handle.

	@param inode the \c Inode handle to repin
 */
void repin_inode(Inode* inode);


/**
	@brief Decrease the number of holders of an inode handle.

	This call decreases the number of holders of the `inode` handle.
	When the number of holders becomes zero, the inode handle is
	no longer valid, and the i-node will be unpinned, and possibly deleted.

	Return 0 on success. If an error occurred, return -1. However, the
	\c inode handle is still released and should not be used. This
	error will often be during a @ref Close() call, and will be reported
	to the user. In case of error, an error code is stored with
	@ref set_errcode().

	@param inode the \c Inode handle to unpin
 */
int unpin_inode(Inode* inode);


/**
	@brief Return the handle to an inode, if it exists.

	This method does not pin an i-node, it simply checks to see
	if it is already pinned, and returns the handle if so.

	Note that this handle should not be kept past the current operation,
	as it may be invalidated. To keep this handle, call @ref repin_inode()
	after this call.
 */
Inode* inode_if_pinned(FsMount* mnt, inode_t id);


/*---------------------------------------------
 *
 * Directory listing
 *
 * A dir_list is an object that can be used to
 * make the contents of a directory available
 * to the kernel in a standard format.
 *
 *-------------------------------------------*/

typedef struct dir_listing 
{
	char* buffer;
	size_t buflen;
	union {
		intptr_t pos;
		void* builder;
	};
} dir_list;

void dir_list_create(dir_list* dlist);
void dir_list_add(dir_list* dlist, const char* name);
void dir_list_open(dir_list* dlist);

int dir_list_read(dir_list* dlist, char* buf, unsigned int size);
int dir_list_close(dir_list* dlist);
intptr_t dir_list_seek(dir_list* dlist, intptr_t offset, int whence);



/*----------------------------------
 *
 * Initialization and finalization
 * These are called from kernel_init.c
 *
 *----------------------------------*/

/**
 	@brief Initialize the file system data structures during boot.
 */
void initialize_filesys();


/**
 	@brief Finalize the file system data structures during boot.
 */
void finalize_filesys();


#endif
