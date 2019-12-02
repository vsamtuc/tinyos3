#include <assert.h>
#include <string.h>

#include "kernel_fs.h"
#include "kernel_proc.h"


/*=========================================================

	Path manipulation

  =========================================================*/


#define PATHSEP '/'

/* print a parsed path to dest */
int parsed_path_str(struct parsed_path* path, char* dest)
{
	char* init_dest = dest;
	if(!path->relpath) { *dest=PATHSEP;  dest++; }

	assert(path->depth>0 || (path->depth==0 && !path->relpath ));
	assert(path->depth <= MAX_PATH_DEPTH);

	for(int i=0; i<path->depth; i++) {
		if(i>0) { *dest=PATHSEP;  dest++; }
		char* d = memccpy(dest, path->component[i], 0, MAX_NAME_LENGTH+1);
		assert(d && d>dest+1);
		dest = d-1;
	}

	*dest = '\0';
	return dest-init_dest;
}


int parse_path(struct parsed_path* path, const char* pathname)
{
	assert(pathname);
	assert(path);

	/* First determine the length. If 0 or greater than MAX_PATHNAME, return -1 */
	unsigned int pnlen = 0;
	for(pnlen=0; pnlen<=MAX_PATHNAME; pnlen++) 
		if(pathname[pnlen]=='\0') break;

	if(pnlen==0 || pnlen>MAX_PATHNAME)
		return -1;

	const char* fromp = pathname;
	const char* endp = pathname+pnlen;
	path->depth = 0;

	/* Check if this is a root */
	if(*fromp == PATHSEP) {
		path->relpath = 0;
		fromp++;
	} else {
		path->relpath = 1;
	}

	/* parse components, but mind the length of each */
	int cclen = 0;
	while(fromp != endp) {
		/* If we find a '/' ... */
		if(*fromp == PATHSEP) {
			/* Terminate current component, if not empty. This
			   allows for consequtive '/' in a pathname to be ignored */
			if(cclen>0) {
				assert(path->depth < MAX_PATH_DEPTH);
				assert(cclen <= MAX_NAME_LENGTH);
				path->component[path->depth][cclen] = '\0';
				path->depth ++;
				cclen = 0;
			}
		} else {
			/* We have an extra char in the current component */
			if(cclen == MAX_NAME_LENGTH || path->depth>= MAX_PATH_DEPTH)
				return -1;
			path->component[path->depth][cclen] = *fromp;
			cclen ++;
		}

		// take next char
		fromp++;
	}

	/* Close the last name in the appropriate manner */
	if(cclen>0) {
		assert(path->depth < MAX_PATH_DEPTH);
		assert(cclen <= MAX_NAME_LENGTH);
		path->component[path->depth][cclen] = '\0';
		path->depth ++;
	} else if(path->depth > 0) {
		/* last character was a '/', so add another component as '.' */
		if(path->depth >= MAX_PATH_DEPTH)
			return -1;
		path->component[path->depth][0] = '.';
		path->component[path->depth][1] = '\0';
		path->depth ++;
	}

	return 0;
}



/*=========================================================

	Inode manipulation

  =========================================================*/


/* 
	The inode table is used to map Inode_ref(mnt, id)-> Inode*.
	Using this table, we are sure that there is at most one Inode* object
	for each Inode_ref. 
 */
static rdict inode_table;

/* Hash function for inode_table */
static hash_value inode_table_hash(Inode_ref* iref)
{
	uintptr_t lhs = (uintptr_t) iref->mnt;
	uintptr_t rhs = iref->id;
	return hash_combine(lhs, rhs);
}

/* Equality function for inode_table. The key is assumed to be a pointer to Inode_ref. */
static int inode_table_equal(rlnode* node, rlnode_key key)
{
	/* Get the ino_ref pointer from the key */
	Inode_ref* keyiref = (Inode_ref*) key.obj;

	Inode_ref* ino_ref = & node->inode->ino_ref;
	return ino_ref->mnt == keyiref->mnt &&  ino_ref->id == keyiref->id;
}


/*
	The root inode points to the base of the file system.
 */


static void release_inode(Inode* inode)
{
	Mount* mnt = inode->ino_ref.mnt;
	FSystem* fsys = mnt->fsys;

	/* If the inode is dirty, flush it */
	if(inode->dirty)
		fsys->FlushInode(inode, 0);
	assert(! inode->dirty);

	/* Remove it from the inode table */
	hash_value hval = inode_table_hash(&inode->ino_ref);
	rdict_remove(&inode_table, &inode->inotab_node, hval);

	/* Remove reference to mount */
	mount_decref(mnt);

	/* Delete it */
	free(inode);
}


Inode* get_inode(Inode_ref ino_ref)
{
	/* Look into the inode_table for existing inode */
	hash_value hval = inode_table_hash(&ino_ref);
	rlnode* node = rdict_lookup(&inode_table, hval, &ino_ref, NULL, inode_table_equal);
	if(node) /* Found it! */
		return node->inode;

	/* Get the mount and file system from the reference */
	Mount* mnt = ino_ref.mnt;
	FSystem* fsys = mnt->fsys;

	/* Get a new inode and initialize it */
	Inode* inode = (Inode*) xmalloc(sizeof(Inode));
	rlnode_init(&inode->inotab_node, inode);

	/* Initialize reference */
	inode->ino_ref = ino_ref;
	mount_incref(mnt);

	/* Add to inode table */
	inode->refcount = 0;
	rdict_insert(&inode_table, &inode->inotab_node, hval);

	/* Fetch data from file system */
	fsys->FetchInode(inode);
	assert(inode_table_equal(&inode->inotab_node, &ino_ref));

	return inode;
}


void inode_incref(Inode* inode)
{
	inode->refcount ++;
}


void inode_decref(Inode* inode)
{
	inode->refcount --;
	if(inode->refcount == 0) 
		release_inode(inode);
}


void inode_flush(Inode* inode)
{
	if(! inode->dirty) return;
	FSystem* fsys = inode->ino_ref.mnt->fsys;
	fsys->FlushInode(inode, 1);
}



/*=========================================================

	FSys calls

  =========================================================*/



/* File system table */
void register_file_system(FSystem* fsys)
{
	for(unsigned i=0;i<FSYS_MAX; i++) {
		if(file_system_table[i]==NULL) {
			file_system_table[i] = fsys;
			return;
		}
	}
	/* If we reached here, we are in trouble! There are too many file systems. */
	assert(0);
	abort();
}


const char* const * get_filesystems()
{
	static const char* fsnames[FSYS_MAX+1];
	int p=0;

	for(unsigned i=0; i<FSYS_MAX; i++)
		if(file_system_table[i] != NULL)
			fsnames[p++] = file_system_table[i]->name;
	return fsnames;
}


FSystem* get_fsys(const char* fsname)
{
	for(unsigned i=0; i<FSYS_MAX; i++)
		if(file_system_table[i] != NULL 
			&& strcmp(file_system_table[i]->name, fsname)==0)
		{
			return file_system_table[i];
		}
	return NULL;
}

FSystem* file_system_table[FSYS_MAX];

/*=========================================================

	Mount calls

  =========================================================*/


/* Initialize the root directory */
Mount mount_table[MOUNT_MAX];

Mount* mount_acquire()
{
	for(unsigned int i=0; i<MOUNT_MAX; i++) {
		if(mount_table[i].refcount == 0) {
			mount_table[i].refcount = 1;
			return & mount_table[i];
		}
	}
	return NULL;
}

void mount_incref(Mount* mnt)
{
	mnt->refcount ++;
}


void mount_decref(Mount* mnt)
{
	mnt->refcount --;
}



/*=========================================================


	VFS system calls


  =========================================================*/


Inode* lookup_dirname(struct parsed_path* pp)
{
	Inode* prev=NULL;
	Inode* last=NULL;
	int ncomp = 0;

	


}




Fid_t sys_Open(const char* pathname, int flags)
{
	/* Take the path */
	struct parsed_path pp;
	if(parse_path(&pp, pathname)==-1) {
		set_errcode(ENAMETOOLONG);
		return -1;
	}

	Inode* dir = lookup_dirname(&pp);


	return NOFILE;
}


int sys_Stat(const char* pathname, struct Stat* statbuf)
{
	return -1;
}


/*=========================================================


	VFS initialization and finalization


  =========================================================*/

/* The root of the file system */
Inode* root_inode;

/* Initialization of the file system module */
void initialize_filesys()
{
	/* Init mounts */
	for(unsigned i=0; i<MOUNT_MAX; i++) {
		mount_table[i].refcount = 0;
		rlnode_init(&mount_table[i].submount_node, &mount_table[i]);
		rlnode_init(&mount_table[i].submount_list, NULL);
	}

	/* Init inode_table and root_node */
	rdict_init(&inode_table, MAX_PROC);
	root_inode = NULL;

	/* Mount the rootfs as the root filesystem */
	FSystem* rootfs = get_fsys("rootfs");
	Mount* root_mnt = rootfs->Mount(rootfs, NO_DEVICE, root_inode, 0, NULL);
	assert(root_mnt != NULL);
}


void finalize_filesys()
{
	/* Unmount rootfs */
	Mount* root_mnt = root_inode->ino_ref.mnt;
	FSystem* fsys = root_mnt->fsys;	
	CHECK(fsys->Unmount(root_mnt));
}



