#include <assert.h>
#include <string.h>

#include "kernel_fs.h"
#include "kernel_proc.h"
#include "kernel_streams.h"
#include "kernel_sys.h"

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


/* Look into the inode_table for existing inode */
Inode* inode_if_pinned(Inode_ref ino_ref)
{
	hash_value hval = inode_table_hash(&ino_ref);
	rlnode* node = rdict_lookup(&inode_table, hval, &ino_ref, NULL, inode_table_equal);
	if(node) {
		/* Found it! */
		Inode* ret = node->inode;
		return ret;
	} else {
		return NULL;
	}
}


/*
	The root inode points to the base of the file system.
 */

Inode* pin_inode(Inode_ref ino_ref)
{
	Inode* inode;

	/* Look into the inode_table for existing inode */
	hash_value hval = inode_table_hash(&ino_ref);
	rlnode* node = rdict_lookup(&inode_table, hval, &ino_ref, NULL, inode_table_equal);
	if(node) {				/* Found it! */
		inode = node->inode;
		repin_inode(inode);
		return inode;
	}

	/* Get the mount and file system from the reference */
	Mount* mnt = ino_ref.mnt;
	FSystem* fsys = mnt->fsys;

	/* 
		Get a new Inode object and initialize it. This is currently done via malloc, but
		we should really replace this with a pool, for speed.
	 */
	inode = (Inode*) xmalloc(sizeof(Inode));
	rlnode_init(&inode->inotab_node, inode);

	/* Initialize reference */
	inode->ino_ref = ino_ref;

	/* Fetch data from file system. If there is an error, clean up and return NULL */
	if(fsys->PinInode(inode)==-1) {
		free(inode);
		return NULL;
	}
	
	/* Add to inode table */
	inode->pincount = 1;
	mount_incref(mnt);
	assert(inode_table_equal(&inode->inotab_node, &ino_ref));
	rdict_insert(&inode_table, &inode->inotab_node, hval);

	return inode;
}


void repin_inode(Inode* inode)
{
	inode->pincount ++;
}

int unpin_inode(Inode* inode)
{
	inode->pincount --;
	if(inode->pincount != 0) return 0;

	/* Nobody is pinning the inode, so we may release the handle */
	/* Remove it from the inode table */
	hash_value hval = inode_table_hash(&inode->ino_ref);
	rdict_remove(&inode_table, &inode->inotab_node, hval);

	/* Remove reference to mount */
	mount_decref(inode_mnt(inode));

	/* Evict it */
	int ret = inode_fsys(inode)->UnpinInode(inode);

	/* Delete it */
	free(inode);

	return ret;
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
		if(mount_table[i].fsys == NULL) {
			mount_table[i].refcount = 0;
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

	Directory operations

  =========================================================*/


Inode* dir_parent(Inode* dir)
{
	Mount* mnt = inode_mnt(dir);
	Inode_id par_id;

	if(inode_lookup(dir, "..", &par_id)==-1) {
		/* Oh dear, we are deleted!! */
		return NULL;
	}

	/* See if we are root */
	if(par_id == dir->ino_ref.id) {
		/* Yes, we are a root in our file system.
		  Take the parent of our mount point */
		if(mnt->mount_point == NULL) {
			/* Oh dear, we are THE root */
			repin_inode(dir);
			return dir;
		} 
		return dir_parent(mnt->mount_point);
	}

	return pin_inode((Inode_ref){mnt, par_id});
}


Inode* dir_lookup(Inode* dir, const pathcomp_t name)
{
	/* Check the easy cases */
	if(strcmp(name, ".")==0) {
		repin_inode(dir);
		return dir;
	}
	if(strcmp(name, "..")==0) 
		return dir_parent(dir);

	Inode_id id;

	/* Do a lookup */
	if(inode_lookup(dir, name, &id) == -1) return NULL;

	/* Pin the next dir */
	Inode* inode = pin_inode((Inode_ref){inode_mnt(dir), id});
	if(inode==NULL) return NULL;
	
	/* Check to see if it has a mounted directory on it */
	if(inode->mounted != NULL) {
		Mount* mnt = inode->mounted;
		unpin_inode(inode);
		inode = pin_inode((Inode_ref){mnt, mnt->root_dir});
		if(inode==NULL) return NULL;
	}

	return inode;
}


Inode* dir_allocate(Inode* dir, const pathcomp_t name, Fse_type type)
{
	Mount* mnt = inode_mnt(dir);
	FSystem* fsys = mnt->fsys;
	Inode_id new_id;

	if(inode_lookup(dir, name, &new_id)==0) return NULL;
	assert(sys_GetError()==ENOENT);

	new_id = fsys->AllocateNode(mnt, type, NO_DEVICE);
	if( inode_link(dir, name, new_id) == -1 ) {
		// This should not have happened...
		fsys->FreeNode(mnt, new_id);
		return NULL;
	}

	return pin_inode((Inode_ref){mnt, new_id});
}



Inode* lookup_path(struct parsed_path* pp, unsigned int tail)
{
	Inode* inode=NULL;

	/* Anchor the search */
	if(pp->relpath) {
		inode = CURPROC->cur_dir;  
	} else {
		inode = CURPROC->root_dir;
	}

	assert(inode != NULL);

	/* Start the search */
	repin_inode(inode);

	for(unsigned int i=0; i+tail < pp->depth; i++) {
		Inode* next = dir_lookup(inode, pp->component[i]);
		unpin_inode(inode);
		if(next==NULL) return NULL;
		inode = next;
	}

	return inode;
}


/*=========================================================


	VFS system calls


  =========================================================*/




Fid_t sys_Open(const char* pathname, int flags)
{
	Fid_t fid;
	FCB* fcb;

	/* Try to reserve ids */
	if(! FCB_reserve(1, &fid, &fcb)) {
		return NOFILE;
	}
	/* RESOURCE: Now we have reserved fid/fcb pair */

	/* Take the path */
	struct parsed_path pp;
	if(parse_path(&pp, pathname)==-1) {
	    FCB_unreserve(1, &fid, &fcb);
		set_errcode(ENAMETOOLONG);
		return NOFILE;
	}

	Inode* dir = lookup_path(&pp, 1);
	if(dir==NULL) {
        FCB_unreserve(1, &fid, &fcb);		
		return NOFILE;
	}

	/* RESOURCE: Now we have a pin on dir */
	
	/* Try looking up the file system entity */
	Inode* file;
	if(pp.depth == 0)
		file = dir;
	else 
		file = dir_lookup(dir, pp.component[pp.depth-1]);

	if(file == NULL) {
		/* If no entity was found, look at the creation flags */
		if(flags & OPEN_CREATE) {
			/* Try to create a file by this name */
			file = dir_allocate(dir, pp.component[pp.depth-1], FSE_FILE);
			if(file==NULL) {
				FCB_unreserve(1, &fid, &fcb);
				unpin_inode(dir);
				return NOFILE;
			}
		} else {
			/* Creation was not specified, so report error */
			FCB_unreserve(1, &fid, &fcb);
			unpin_inode(dir);
			set_errcode(ENOENT);
			return NOFILE;
		}
	} else {
		/* An entity was found but again look at the creation flags */
		if((flags & OPEN_CREATE) && (flags & OPEN_EXCL)) {
			FCB_unreserve(1, &fid, &fcb);
			unpin_inode(dir);
			unpin_inode(file);
			set_errcode(EEXIST);
			return NOFILE;			
		}
	}

	/* RELEASE: We no longer need dir */
	unpin_inode(dir);

	/* RESOURCE: We now have an entity inode (file) */
	int rc = inode_open(file, flags & 077, &fcb->streamobj, &fcb->streamfunc);

	/* RELEASE: We no longer need file */
	unpin_inode(file);

	if(rc) {
		/* Error in inode_open() */
		FCB_unreserve(1, &fid, &fcb);
		return NOFILE;					
	}

	/* Success! */
	return fid;
}


int sys_Stat(const char* pathname, struct Stat* statbuf)
{
	/* Parse the path */
	struct parsed_path pp;
	if(parse_path(&pp, pathname)==-1) {
		set_errcode(ENAMETOOLONG);
		return -1;
	}
	
	/* Look it up */
	Inode* inode = lookup_path(&pp, 0);
	if(inode==NULL) return -1;

	inode_fsys(inode)->Status(inode, statbuf, STAT_ALL);

	unpin_inode(inode);
	return 0;
}


/*=========================================================


	VFS initialization and finalization


  =========================================================*/


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

	/* Mount the rootfs as the root filesystem */
	FSystem* rootfs = get_fsys("rootfs");
	Mount* root_mnt = mount_acquire();
	rootfs->Mount(root_mnt, rootfs, NO_DEVICE, NULL, 0, NULL);
	assert(root_mnt != NULL);
}


void finalize_filesys()
{
	/* Unmount rootfs */
	Mount* root_mnt = mount_table;
	FSystem* fsys = root_mnt->fsys;
	CHECK(fsys->Unmount(root_mnt));

	assert(inode_table.size == 0);
	rdict_destroy(&inode_table);
}



