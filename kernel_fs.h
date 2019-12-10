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


/*----------------------------------
 *
 * Virtual File system declarations
 *
 *----------------------------------*/

typedef struct InodeHandle Inode;
typedef struct FsMount FsMount;
typedef struct FSystem_type FSystem;
typedef struct dir_entry dir_entry;

/* These two types are just placeholders for actual void pointers */
#if !defined(INODE)
struct GENERIC_INODE { void* ptr; };
#define INODE  struct GENERIC_INODE
#endif

#if !defined(MOUNT)
struct GENERIC_MOUNT { void* ptr; } MOUNT;
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
	which is statically allocated (although this is not mandatory).
	
	FSystem objects are retrieved by name using function @ref get_fsys().
	The @ref REGISTER_FSYS() macro can be used to register file systems from different
	files.	
  */
struct FSystem_type
{
	const char* name;

	/* Mount a file system of this type. */
	int (*Mount)(MOUNT* mnt, FSystem* this, Dev_t dev, 
				unsigned int param_no, mount_param* param_vec);


	/* Unmount a particular mount */
	int (*Unmount)(MOUNT mnt);


	/**
		@brief Return the status of the file system. 

		Returns information about a mounted file system. In
		particular, this call returns the \c inode_t  of the
		file system root.

		@param mnt the mounted file system
		@param statfs buffer to fill in the required information.
		@return 0 on success, -1 on failure
	 */
	void (*StatFs)(MOUNT mnt, struct StatFs* statfs);


	/* Create inodes */
	inode_t (*AllocateNode)(MOUNT mnt, Fse_type type, Dev_t dev);


	/**
		@brief Free an i-node.

		Release an i-node and the resources related to it.
		Note: Maybe this should not be exported to the system?
		Does any code outside of the fsys code ever call it?
	   */
	int (*FreeNode)(MOUNT mnt, inode_t id);

	/**
		@brief Pin i-node data for a new handle.

		When a new handle is created for some i-node, via a call to @ref pin_inode(),
		this function is called to inform the mount. Typically, this means that the 
		i-node data is loaded from disk, so that subsequent operations are done quickly.

		Returns 0 on success and -1 on failure.
	  */
	int (*PinInode)(Inode* inode);

	/**
		@brief Unload i-node data for a new handle.

		When the handle for an i-node is about to be released (because it is unpinned),
		this function is called.

		Returns 0 on success and -1 on failure.
	  */
	int (*UnpinInode)(Inode* inode);

	/**
		@brief Save the i-node and related data to disk.

		The entity referred to by the inode will be sent to storage. This includes the
		inode itself as well as any data represented by this i-node (e.g., a file's content).

		Returns 0 on success and -1 on failure.
	 */
	int (*FlushInode)(Inode* inode);

	/**
		@brief Return a stream object for this inode 
	
		The stream object is initialized for proper use by the kernel stream
		system calls.
	*/
	int (*OpenInode)(Inode* inode, int flags, void** obj, file_ops** ops);

	/**
		@brief Search a directory for a particular entry by name.

		If found, and \c id is not NULL, store the \c inode_t in it. Else, do not touch it.
		Return 0 on success and -1 on failure.
	 */
	int (*Lookup)(Inode* this, const pathcomp_t name, inode_t* id);

	/**
		@brief Add a new entry to a directory.
	 */
	int (*Link)(Inode* this, const pathcomp_t name, inode_t id);

	/**
		@brief Remove an entry from a directory
	 */
	int (*Unlink)(Inode* this, const pathcomp_t name);

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
		in \c STAT_ALL), then the 
	 */
	void (*Status)(Inode* this, struct Stat* status, pathcomp_t name, int which);

};


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
	@brief In-core handle to an i-node.

	I-nodes are the representation of all file system entities. However,
	the schema (contained fields) of an i-node are highly specific to
	particular file systems and object types.

	The following structure simply exposes some fields related to the
	filesystem-independent semantics of i-nodes. In particular, the
	main purpose of the fields below is to support the cross-filesystem
	coordination needed by mounting operations. 

	In addition, the fields below are used to determine the code paths for
	managing the lifecycle of an in-core i-node. 

	An inode can be "in-core", i.e. represented by an Inode object, or 
	"out-of-core", i.e., it resides in non-volatile memory (of some device).
	Naturally, to use the object described by an i-node, the i-node must
	be in-core.

	The main representation of an inode is the Inode_ref, which is a pair 
	of (Mount*, inode_t). The call @ref get_inode() makes the inode "in-core", 
	so that it can be read and modified.
	
	For consistency, it is important that there be **at most one Inode handle** 
	for every inode. To ensure that there is a unique Inode object for each 
	\c Inode_ref, we use a hash table.

*/
typedef struct InodeHandle
{
	unsigned int pincount;  /**< @brief Reference-counting uses to this inode handle */
	FsMount* ino_mnt;		/**< @brief Inode filesystem */
	inode_t ino_id;		/**< @brief Inode number */
	rlnode inotab_node;   	/**< @brief Used to add this to the inode table */

	/** @brief Points to a mount whose mount point is this directory, or NULL */
	struct FsMount* mounted;

	/** @brief This pointer is used by the mounted file system. */
	void* fsinode;			

} Inode;


/**
	@brief Turn an i-node reference into a handle.
	
	This call returns a pointer to an \c Inode handle for the
	given reference, performing I/O if needed to fetch the i-node
	from storage.

	The caller should call @ref unpin_inode when the handle no longer 
	needed.
*/
Inode* pin_inode(FsMount* mnt, inode_t id);

/**
	@brief Add a pin to an already pinned i-node.

	This call increases the number of holders of the `inode` handle.
 */
void repin_inode(Inode* inode);


/**
	@brief Decrease the number of holders of an inode handle.

	This call decreases the number of holders of the `inode` handle.
	When the number of holders becomes zero, the inode handle is
	no longer valid, and the i-node will be evicted from memory.

	Return 0 on success. If an error occurred, return -1. However, the
	\c inode handle is still released and should not be used. This
	error will often be during a @ref Close() call, and will be reported
	to the user.
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


/* --------------------------------------------
	Some convenience methods on Inode objects
 ---------------------------------------------- */

/**
	@brief Return the mount of this inode handle
 */
inline static FsMount* inode_mnt(Inode* inode)
{
	return inode->ino_mnt;
}


/**
	@brief Return the mount of this inode handle
 */
inline static inode_t inode_id(Inode* inode)
{
	return inode->ino_id;
}


/**
	@brief Return the file system of this inode handle
 */
inline static FSystem* inode_fsys(Inode* inode) 
{
	return inode_mnt(inode)->fsys;
}


/**
	@brief Open a stream on this i-node.
 */
inline static int inode_open(Inode* inode, int flags, void** obj, file_ops** ops)
{
	return inode_fsys(inode)->OpenInode(inode, flags, obj, ops);
}

/**
	@brief Look up a name in a directory
 */
inline static int inode_lookup(Inode* dir_inode, const pathcomp_t name, inode_t* id)
{
	return inode_fsys(dir_inode)->Lookup(dir_inode, name, id);
}

/**
	@brief Add a link to a file system element from a directory
 */
inline static int inode_link(Inode* dir_inode, const pathcomp_t name, inode_t inode)
{
	return inode_fsys(dir_inode)->Link(dir_inode, name, inode);
}


/**
	@brief Remove a link in a directory.
 */
inline static int inode_unlink(Inode* dir_inode, const pathcomp_t name)
{
	return inode_fsys(dir_inode)->Unlink(dir_inode, name);
}

/**
	@brief Flush the file system element to storage.
 */
inline static int inode_flush(Inode* inode)
{
	return inode_fsys(inode)->FlushInode(inode);
}


/**
	@brief Return the FSE type of an i-node
 */
Fse_type inode_type(Inode* inode);


/**
	@brief Return a pinned handle to the parent of dir

	This method knows how to cross into and out of submounts.
 */
Inode* dir_parent(Inode* dir);

/**
	@brief Check whether a directory contains a name.
  */
int dir_name_exists(Inode* dir, const pathcomp_t name);

/**
	@brief Do a directory lookup.

	This method knows how to cross into and out of submounts.
 */
Inode* dir_lookup(Inode* dir, const pathcomp_t name);


/**
	@brief Allocate a new i-node in a directory.

	
  */
Inode* dir_allocate(Inode* dir, const pathcomp_t name, Fse_type type);

/**
	@brief Follow a pathname 
  */
Inode* lookup_pathname(const char* pathname, const char** last);


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
