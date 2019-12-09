#include <assert.h>
#include <string.h>

#include "kernel_fs.h"
#include "kernel_proc.h"
#include "kernel_streams.h"
#include "kernel_sys.h"

/*=========================================================

	Inode manipulation

  =========================================================*/


/* 
	The inode table is used to map Inode_ref(mnt, id)-> Inode*.
	Using this table, we are sure that there is at most one Inode* object
	for each Inode_ref. 
 */
static rdict inode_table;

/* This type is used as the key of the inode_table */
struct Inode_ref
{
	struct FsMount* mnt;
	Inode_id id;
};


/* Equality function for inode_table. The key is assumed to be a pointer to Inode_ref. */
static int inode_table_equal(rlnode* node, rlnode_key key)
{
	/* Get the ino_ref pointer from the key */
	struct Inode_ref* keyiref = key.obj;
	return inode_mnt(node->inode)==keyiref->mnt && inode_id(node->inode) == keyiref->id;
}


/* Look into the inode_table for existing inode */
Inode* inode_if_pinned(FsMount* mnt, Inode_id id)
{
	hash_value hval = hash_combine((hash_value)mnt, id);
	struct Inode_ref ino_ref = {mnt, id};
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

Inode* pin_inode(FsMount* mnt, Inode_id id)
{
	Inode* inode;

	/* Look into the inode_table for existing inode */
	hash_value hval = hash_combine((hash_value)mnt, id);
	struct Inode_ref ino_ref = {mnt, id};
	rlnode* node = rdict_lookup(&inode_table, hval, &ino_ref, NULL, inode_table_equal);
	if(node) {				/* Found it! */
		inode = node->inode;
		repin_inode(inode);
		return inode;
	}

	/* Get the file system */
	FSystem* fsys = mnt->fsys;

	/* 
		Get a new Inode object and initialize it. This is currently done via malloc, but
		we should really replace this with a pool, for speed.
	 */
	inode = (Inode*) xmalloc(sizeof(Inode));
	rlnode_init(&inode->inotab_node, inode);

	/* Initialize reference */
	inode->ino_mnt = mnt;
	inode->ino_id = id;
	inode->mounted = NULL;

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
	hash_value hval = hash_combine((hash_value) inode_mnt(inode), inode_id(inode));
	rdict_remove(&inode_table, &inode->inotab_node, hval);

	/* Remove reference to mount */
	mount_decref(inode_mnt(inode));

	/* Evict it */
	int ret = inode_fsys(inode)->UnpinInode(inode);

	/* Delete it */
	free(inode);

	return ret;
}



Fse_type inode_type(Inode* inode)
{
	struct Stat s;
	inode_fsys(inode)->Status(inode, &s, NULL, STAT_TYPE);
	return s.st_type;
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

	FsMount calls

  =========================================================*/


/* Initialize the root directory */
FsMount mount_table[MOUNT_MAX];

FsMount* mount_acquire()
{
	for(unsigned int i=0; i<MOUNT_MAX; i++) {
		if(mount_table[i].fsys == NULL) {
			mount_table[i].refcount = 0;
			return & mount_table[i];
		}
	}
	return NULL;
}

void mount_incref(FsMount* mnt)
{
	mnt->refcount ++;
}

void mount_decref(FsMount* mnt)
{
	mnt->refcount --;
}



/*=========================================================

	Directory operations

  =========================================================*/


Inode* dir_parent(Inode* dir)
{
	FsMount* mnt = inode_mnt(dir);
	Inode_id par_id;

	if(inode_lookup(dir, "..", &par_id)==-1) {
		/* Oh dear, we are deleted!! */
		return NULL;
	}

	/* See if we are root */
	if(par_id == inode_id(dir)) {
		/* Yes, we are a root in our file system.
		  Take the parent of our mount point */
		if(mnt->mount_point == NULL) {
			/* Oh dear, we are THE root */
			repin_inode(dir);
			return dir;
		} 
		return dir_parent(mnt->mount_point);
	}

	return pin_inode(mnt, par_id);
}


int dir_name_exists(Inode* dir, const pathcomp_t name)
{
	if(strcmp(name, ".")==0 || strcmp(name, "..")==0) return 1;
	Inode_id id;
	return inode_lookup(dir, name, &id) == 0;
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
	Inode* inode = pin_inode(inode_mnt(dir), id);
	if(inode==NULL) return NULL;
	
	/* Check to see if it has a mounted directory on it */
	if(inode->mounted != NULL) {
		FsMount* mnt = inode->mounted;
		unpin_inode(inode);
		inode = pin_inode(mnt, mnt->root_dir);
		if(inode==NULL) return NULL;
	}

	return inode;
}


Inode* dir_allocate(Inode* dir, const pathcomp_t name, Fse_type type)
{
	FsMount* mnt = inode_mnt(dir);
	FSystem* fsys = mnt->fsys;
	Inode_id new_id;

	if(dir_name_exists(dir, name)) return NULL;

	new_id = fsys->AllocateNode(mnt, type, NO_DEVICE);
	if( inode_link(dir, name, new_id) == -1 ) {
		// This should not have happened...
		fsys->FreeNode(mnt, new_id);
		return NULL;
	}

	return pin_inode(mnt, new_id);
}



/*=========================================================

	Path manipulation

  =========================================================*/


#define PATHSEP '/'


Inode* lookup_pathname(const char* pathname, const char** last)
{
	int pathlen = strlen(pathname);
	if(pathlen > MAX_PATHNAME) { set_errcode(ENAMETOOLONG); return NULL; }
	if(pathlen==0) { set_errcode(ENOENT); return NULL; }

	const char* base =  pathname;
	const char* cur;
	Inode* inode = NULL;

	/* Two local helpers */
	int advance() {
		pathcomp_t comp;
		memcpy(comp, base, (cur-base));
		comp[cur-base] = 0;
		Inode* next = dir_lookup(inode, comp);
		unpin_inode(inode);
		inode = next;		
		return next!=NULL; 
	}

	int length_is_ok() {
		if( (cur-base)>MAX_NAME_LENGTH ) {
			set_errcode(ENAMETOOLONG);
			unpin_inode(inode);
			return 0;
		}
		return 1;
	}

	/* Start with the first character */
	if(*base == '/')  { inode = CURPROC->root_dir; base++; }
	else { inode= CURPROC->cur_dir; }
	repin_inode(inode);

	/* Iterate over all but the last component */
	for(cur = base; *base != '\0'; base=++cur) 
	{
		assert(cur==base);

		/* Get the next component */
		while(*cur != '\0' && *cur != '/') cur++;
		if(cur==base) continue;

		/* cur is at the end, break out to treat last component specially */
		if(*cur=='\0') break;

		/* We have a segment, check it */
		if(! length_is_ok()) return NULL;

		/* ok good size, look it up */
		if(! advance()) return NULL;
	}

	/* (*base) is either 0 or some char */
	assert(*base!='/');  

	/* if last component is empty, pathname ended in '/' */
	assert( (*base != '\0') || (*(base-1)=='/')  );

	/* One last check */
	if(! length_is_ok()) return NULL;

	/* So, at the end either we have a final segment, 
		or *base == '\0' */
	if(last==NULL) {
		if(*base!='\0') {
			/* one last hop */
			if(! advance()) return NULL;
		}	
	} else {
		if(*base!='\0') 
			*last = base;
		else
			*last = NULL;
	}

	return inode;
 }



/*=========================================================


	VFS system calls


  =========================================================*/


/* The cleanup attribute greatly simplifies the implementation of system calls */

void unpin_cleanup(Inode** inoptr) { if(*inoptr) unpin_inode(*inoptr); }
#define AUTO_UNPIN  __attribute__((cleanup(unpin_cleanup)))


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
	const char* last;
	Inode* dir AUTO_UNPIN = lookup_pathname(pathname, &last);
	if(dir==NULL) {
        FCB_unreserve(1, &fid, &fcb);		
		return NOFILE;
	}

	/* Try looking up the file system entity */
	Inode* file AUTO_UNPIN = dir_lookup(dir, last);

	if(file == NULL) {
		/* If no entity was found, look at the creation flags */
		if(flags & OPEN_CREAT) {
			/* Try to create a file by this name */
			file = dir_allocate(dir, last, FSE_FILE);
			if(file==NULL) {
				FCB_unreserve(1, &fid, &fcb);
				return NOFILE;
			}
		} else {
			/* Creation was not specified, so report error */
			FCB_unreserve(1, &fid, &fcb);
			set_errcode(ENOENT);
			return NOFILE;
		}
	} else {
		/* An entity was found but again look at the creation flags */
		if((flags & OPEN_CREAT) && (flags & OPEN_EXCL)) {
			FCB_unreserve(1, &fid, &fcb);
			set_errcode(EEXIST);
			return NOFILE;			
		}
	}

	int rc = inode_open(file, flags & 077, &fcb->streamobj, &fcb->streamfunc);

	if(rc==-1) {
		/* Error in inode_open() */
		FCB_unreserve(1, &fid, &fcb);
		return NOFILE;					
	}

	/* Success! */
	return fid;
}



int sys_Stat(const char* pathname, struct Stat* statbuf)
{	
	/* Look it up */
	Inode* inode AUTO_UNPIN = lookup_pathname(pathname, NULL);
	if(inode==NULL) return -1;

	inode_fsys(inode)->Status(inode, statbuf, NULL, STAT_ALL);
	return 0;
}




int sys_Link(const char* pathname, const char* newpath)
{
	/* Check new path */
	const char* last;
	Inode* newdir AUTO_UNPIN = lookup_pathname(newpath, &last);

	if(newdir == NULL) return -1;
	if(last==NULL || dir_name_exists(newdir, last)) { set_errcode(EEXIST); return -1; }

	Inode* old AUTO_UNPIN = lookup_pathname(pathname, NULL);
	if(old==NULL) return -1;

	/* They must be in the same FS */
	if(inode_mnt(old) != inode_mnt(newdir)) {
		set_errcode(EXDEV);
		return -1;
	}

	return inode_link(newdir, last, inode_id(old));
}



int sys_Unlink(const char* pathname)
{
	const char* last;
	Inode* dir AUTO_UNPIN = lookup_pathname(pathname, &last);

	if(dir==NULL) return -1;
	if(last==NULL) { set_errcode(EISDIR); return -1; }

	Inode* inode AUTO_UNPIN = dir_lookup(dir, last);

	if(inode_type(inode)==FSE_DIR) {
		set_errcode(EISDIR);
		return -1;
	}
	
	return inode_unlink(dir, last);
}


int sys_MkDir(const char* pathname)
{
	const char* last;
	Inode* dir AUTO_UNPIN = lookup_pathname(pathname, &last);

	if(dir==NULL) return -1;
	if(last==NULL || dir_name_exists(dir, last)) { set_errcode(EEXIST); return -1; }
	if(inode_type(dir)!=FSE_DIR) { set_errcode(ENOTDIR); return -1; }

	Inode* newdir AUTO_UNPIN = dir_allocate(dir, last, FSE_DIR);
	return (newdir == NULL)?-1:0;
}


int sys_RmDir(const char* pathname)
{
	const char* last;
	Inode* dir AUTO_UNPIN = lookup_pathname(pathname, &last);
	if(dir==NULL) { return -1; }
	if(last==NULL) { set_errcode(ENOENT); return -1; }
	if(strcmp(last,".")==0) { set_errcode(EINVAL); return -1; }	
	if(! dir_name_exists(dir,last)) { set_errcode(ENOENT); return -1; }

	return inode_unlink(dir, last);
}

int sys_GetCwd(char* buffer, unsigned int size)
{
	Inode* curdir = CURPROC->cur_dir;
	
	char* buf = buffer;
	unsigned int sz = size;

	int bprintf(const char* str) {
		unsigned int len = strlen(str);
		if(len>=sz) {
			set_errcode(ERANGE);
			return -1;
		} else {
			strcpy(buf, str);
			buf+=len;
			sz -= len;
			return 0;
		}
	}

	int print_path_rec(Inode* dir, int level) {
		/* Get your parent */
		Inode* parent AUTO_UNPIN = dir_parent(dir);
		if(parent==NULL) return -1;
		if(parent == dir) {
			/* We are the root */
			return  (level == 0) ? bprintf("/") : 0;
		} else {
			/* We are just a normal dir */
			int rc = print_path_rec(parent, level+1);
			if(rc!=0) return rc;

			pathcomp_t comp;
			inode_fsys(dir)->Status(dir, NULL, comp, STAT_NAME);
			if(rc!=0) return -1;

			if(bprintf("/")!=0) return-1;
			return bprintf(comp);
		}
	}

	return print_path_rec(curdir, 0);
}

int sys_ChDir(const char* pathname)
{
	Inode* dir AUTO_UNPIN = lookup_pathname(pathname, NULL);
	if(dir==NULL)  return -1;
	if(inode_type(dir)!=FSE_DIR) {
		set_errcode(ENOTDIR);
		return -1;
	}

	if(CURPROC->cur_dir != dir) {
		Inode* prevcd = CURPROC->cur_dir;
		CURPROC->cur_dir = dir;
		dir = prevcd;
	}
	return 0;
}

int sys_Mount(Dev_t device, const char* mount_point, const char* fstype, unsigned int paramc, mount_param* paramv)
{
	/* Find the file system */
	FSystem* fsys = get_fsys(fstype);
	if(fsys==NULL) { set_errcode(ENODEV); return -1; }

	/* Get the mount record */
	FsMount* mnt = mount_acquire();
	if(mnt==NULL) return -1;

	/* Resolve mount point */
	Inode* mpoint AUTO_UNPIN = NULL;
	if(mount_point != NULL) {
		mpoint = lookup_pathname(mount_point, NULL);
		if(mpoint==NULL) return -1;
		if(mpoint->pincount != 1) { set_errcode(EBUSY); return -1; }
	} 
	else 
	{
		/* If mpoint is NULL, we must be the root mount */
		if(mnt != mount_table) {
			set_errcode(EBUSY);
			return -1;
		}
	}

	/* TODO: check that the device is not busy */

	int rc = fsys->Mount(mnt, fsys, device, mpoint, paramc, paramv);
	if(rc==-1) return -1;

	return 0;
}

int sys_Umount(const char* mount_point)
{
	FsMount* mnt;

	/* Special case: this must be the root mount */
	if(mount_point==NULL) {
		mnt = mount_table;
	} else {
		/* Look up the mount point */
		Inode* mpoint AUTO_UNPIN = lookup_pathname(mount_point, NULL);
		if(mpoint==NULL) return -1;

		if(inode_mnt(mpoint)->root_dir != inode_id(mpoint)) {
			set_errcode(EINVAL);    /* This is not a mount point */
			return -1;
		}

		mnt = inode_mnt(mpoint);
	}

    /* See if we are busy */
    if(mnt->refcount != 0) {
        set_errcode(EBUSY);
        return -1;
    }

    /* Ok, let's unmount */

    /* Detach from the filesystem */
    if(mnt->mount_point!=NULL) {
        mnt->mount_point->mounted = NULL;
        rlist_remove(& mnt->submount_node);
    } 

    int rc = mnt->fsys->Unmount(mnt);

    mnt->fsys = NULL;
    return rc;
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

	/* FsMount the rootfs as the root filesystem */
	int rc = sys_Mount(NO_DEVICE, NULL, "rootfs", 0, NULL);
	assert(rc==0);
	if(rc!=0) abort();
}


void finalize_filesys()
{
	/* Unmount rootfs */
	int rc = sys_Umount(NULL);
	assert(rc==0);
	if(rc!=0) abort();

	assert(inode_table.size == 0);
	rdict_destroy(&inode_table);
}



