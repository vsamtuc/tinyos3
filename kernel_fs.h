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

/** @brief Maximum length of a directory entry name */
#define MAX_NAME_LENGTH 31

/** @brief Maximum number of components in a path */
#define MAX_PATH_DEPTH   12

/** @brief Maximum length of a pathname. */
#define MAX_PATHNAME 512

typedef char pathcomp_t[MAX_NAME_LENGTH+1];

struct parsed_path
{
	int relpath;
	int depth;
	pathcomp_t component[MAX_PATH_DEPTH];
};


/**
	@brief Construct a path string out of a parsed path.

	@return string length of dest
  */
int parsed_path_str(struct parsed_path* pp, char* dest);


/** 
	@brief Parse a pathname into a struct.

	Returns 0 on success, -1 on error
  */
int parse_path(struct parsed_path* path, const char* pathname);


/*----------------------------------
 *
 * Virtual File system declarations
 *
 *----------------------------------*/


struct dir_entry;
struct Inode;
struct FSys_type;
struct Mount;

typedef uintptr_t Inode_id;
typedef struct Inode Inode;
typedef struct Mount Mount;
typedef struct FSystem_type FSystem;
typedef struct dir_entry dir_entry;


typedef struct Inode_ref
{
	struct Mount* mnt;
	Inode_id id;
} Inode_ref;


/* The type of FSE (File System Entity) Determines the API */
typedef enum {
	FSE_FS,
	FSE_DIR,
	FSE_FILE,
	FSE_DEV
} Fse_type;


typedef struct fs_param 
{
	const char* param_name;
	rlnode_key param_value;
} fs_param;


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

	/* Create and delete inodes (except for directories) */
	Inode_id (*AllocateNode)(Mount* mnt, Fse_type type, Dev_t dev);
	int (*ReleaseNode)(Mount* mnt, Inode_id id);

	/* Load and save inodes */
	void (*FetchInode)(Inode* inode);
	void (*FlushInode)(Inode* inode, int keep);

	/* Return a stream object for this inode */
	void* (*OpenInode)(Inode* inode, int flags);

	/* Directory ops */
	Inode* (*Lookup)(Inode* this, pathcomp_t name);
	int (*Link)(Inode* this, pathcomp_t name, Inode* inode);
	int (*Unlink)(Inode* this, pathcomp_t name);

	/* Mount a file system of this type. This is a file system method */
	struct Mount* (*Mount)(FSystem* this, Dev_t dev, Inode* mpoint, 
		unsigned int param_no, fs_param* param_vec);

	/* Unmount a particular mount */
	int (*Unmount)(Mount* mnt);
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

struct Mount
{
	/* Counts users of this mount */
	unsigned int refcount;

	/* Inode on top of which we mounted. This is NULL for the root mount only. */
	Inode* mount_point;			

	/* Inode of our root directory */
	Inode_id root_dir;

	/* The file system */
	FSystem* fsys;

	/* List of all submounts */
	rlnode submount_list;
	rlnode submount_node;  /* Intrusive node */

	void* fsdata;	/* This is understood by tbe file system */
};


extern Mount mount_table[MOUNT_MAX];
Mount* mount_acquire();
void mount_incref(Mount* mnt);
void mount_decref(Mount* mnt);



/*----------------------------------
 *
 * Directories
 *
 *----------------------------------*/


typedef struct dir_entry
{
	pathcomp_t name;
	Inode_id id;
} dir_entry;


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
	of (Mount*, Inode_id). The call @ref get_inode() makes the inode "in-core", 
	so that it can be read and modified.
	
	For consistency, it is important that there be **at most one Inode handle** 
	for every inode. To ensure that there is a unique Inode object for each 
	\c Inode_ref, we use a hash table.

*/
typedef struct Inode
{
	unsigned int refcount;  /**< @brief Counts the pointers to this inode */

	Fse_type type;			/**< @brief  Type of inode (determines the API) */
	Inode_ref ino_ref;		/**< @brief  Inode reference */

	unsigned int lnkcount;  /**< @brief  Hard links to this inode */
	int dirty;			 	/**< @brief  True if this inode is dirty */


	/* API-specific data */
	union {
		struct {
			struct Mount* mount;/**< @brief Points to a mount on this directory, or NULL */
			Inode_id parent;	/**< @brief This is our parent in our own filesystem. */
		} dir;

		struct {} file;
		
		struct {
			Dev_t rdev;			/**< @brief The device represented by this device node */
		} dev;
	};

	rlnode inotab_node;   	/**< @brief Used to add this to the inode table */
} Inode;



/* This is the root directory of the tinyos session */
extern Inode* root_inode;

Inode* get_inode(Inode_ref inoref);

void inode_incref(Inode* inode);
void inode_decref(Inode* inode);

void inode_flush(Inode* inode);


/*----------------------------------
 *
 * Initialization and finalization
 * These are called from kernel_init.c
 *
 *----------------------------------*/

void initialize_filesys();
void finalize_filesys();


#endif
