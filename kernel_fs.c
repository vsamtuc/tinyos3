#include <assert.h>
#include <string.h>

#include "kernel_fs.h"
#include "kernel_proc.h"
#include "kernel_streams.h"
#include "kernel_sys.h"

/*=========================================================

	Inode manipulation

  =========================================================*/


/* --------------------------------------------
	Some convenience methods on Inode objects.
	These methods essentially wrap the FSystem API
	using Inode handles
 ---------------------------------------------- */

/* Return the mount of this inode handle */
inline static FsMount* i_mnt(Inode* inode) { return inode->ino_mnt; }

/* Return MOUNT for inode */
inline static MOUNT i_fsmount(Inode* inode) { return i_mnt(inode)->fsmount; }

/*  Return the mount of this inode handle */
inline static inode_t i_id(Inode* inode) { return inode->ino_id; }

/* Return the file system driver of this inode handle */
inline static FSystem* i_fsys(Inode* inode) {  return i_mnt(inode)->fsys; }


/* Wraps Open */
inline static int i_open(Inode* inode, int flags, void** obj, file_ops** ops)
{
	return i_fsys(inode)->Open(i_fsmount(inode), i_id(inode), flags, obj, ops);
}

/* Wraps Fetch */
inline static int i_fetch(Inode* dir, const pathcomp_t name, inode_t* id, int creat)
{
	return i_fsys(dir)->Fetch(i_fsmount(dir), i_id(dir), name, id, creat);
}


/* Wrap link */
inline static int i_link(Inode* dir, const pathcomp_t name, inode_t inode)
{
	return i_fsys(dir)->Link(i_fsmount(dir), i_id(dir), name, inode);
}


/* Wrap unlink */
inline static int i_unlink(Inode* dir, const pathcomp_t name)
{
	return i_fsys(dir)->Unlink(i_fsmount(dir), i_id(dir), name);
}

/* Wrap Flush */
inline static int i_flush(Inode* inode)
{
	return i_fsys(inode)->Flush(i_fsmount(inode), i_id(inode));
}

/* Wrap Truncate */
inline static int i_truncate(Inode* inode, intptr_t length)
{
	return i_fsys(inode)->Truncate(i_fsmount(inode), i_id(inode), length);
}

inline static int i_status(Inode* inode, struct Stat* st, pathcomp_t name)
{
	return i_fsys(inode)->Status(i_fsmount(inode), i_id(inode), st, name);
}

/* Return the FSE type of an i-node */
inline static Fse_type i_type(Inode* inode)
{
	struct Stat s;
	int rc = i_status(inode, &s, NULL);
	assert(rc==0);
	return s.st_type;
}


/* --------------------------------------------

  Handle management

 ---------------------------------------------- */


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
	inode_t id;
};


/* Equality function for inode_table. The key is assumed to be a pointer to Inode_ref. */
static int inode_table_equal(rlnode* node, rlnode_key key)
{
	/* Get the ino_ref pointer from the key */
	struct Inode_ref* keyiref = key.obj;
	return i_mnt(node->inode)==keyiref->mnt && i_id(node->inode) == keyiref->id;
}


/* Look into the inode_table for existing inode */
Inode* inode_if_pinned(FsMount* mnt, inode_t id)
{
	hash_value hval = hash_combine((hash_value)mnt, id);
	struct Inode_ref ino_ref = {mnt, id};
	rlnode* node = rdict_lookup(&inode_table, hval, &ino_ref, inode_table_equal);
	if(node) {
		/* Found it! */
		Inode* ret = node->inode;
		return ret;
	} else {
		return NULL;
	}
}


/*
	Creating a Inode handle.
 */

Inode* pin_inode(FsMount* mnt, inode_t id)
{
	Inode* inode;

	/* Look into the inode_table for existing inode */
	hash_value hval = hash_combine((hash_value)mnt, id);
	struct Inode_ref ino_ref = {mnt, id};
	rlnode* node = rdict_lookup(&inode_table, hval, &ino_ref, inode_table_equal);
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
	rdict_node_init(&inode->inotab_node, inode, hval);

	/* Initialize reference */
	inode->ino_mnt = mnt;
	inode->ino_id = id;
	inode->mounted = NULL;

	/* Pin inode in the file system. If there is an error, clean up and return NULL */
	int rc = fsys->Pin(mnt->fsmount, id);
	if(rc!=0) {
		free(inode);
		set_errcode(rc);
		return NULL;
	}
	
	/* Add to inode table */
	inode->pincount = 1;
	mnt->use_count++;
	assert(inode_table_equal(&inode->inotab_node, &ino_ref));
	rdict_insert(&inode_table, &inode->inotab_node);

	return inode;
}


Inode* repin_inode(Inode* inode)
{
	inode->pincount ++;
	return inode;
}

int unpin_inode(Inode* inode)
{
	inode->pincount --;
	if(inode->pincount != 0) return 0;

	/* Nobody is pinning the inode, so we release the handle */

	/* Remove it from the inode table */
	rdict_remove(&inode_table, &inode->inotab_node);

	/* Remove reference to mount */
	i_mnt(inode)->use_count--;

	/* Unpin the i-node */
	int rc = i_fsys(inode)->Unpin(i_fsmount(inode), i_id(inode));

	/* Delete the handle and return */
	free(inode);

	if(rc) {
		set_errcode(rc);
		return -1;
	} else 
		return 0;
}

/* 
	The 'cleanup' attribute greatly simplifies the manipulation of pins.
*/

void unpin_cleanup(Inode** inoptr) { if(*inoptr) unpin_inode(*inoptr); }
#define AUTO_UNPIN  __attribute__((cleanup(unpin_cleanup)))


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
			/* Found an entry, initialize its use count */
			mount_table[i].use_count = 0;
			return & mount_table[i];
		}
	}
	return NULL;
}




/*=========================================================

	Directory operations

	For mount roots some operations are translated. Suppose
	that R is the root i-node of a file system mounted at 
	path /p mount point whose i-node is M. Then, both i-nodes
	R and M are needed to apply operations on /p.

	Parent:   /p/.. is the parent of M
	Dir Name: is the name of M
	Fetch in /p: must be applied to R
	stat /p: must be applied to R.

	etc ...

	To support this, we say that M is the "back" and R is the
	"front" i-node of /p. 

	For paths that are not mount points, front and back return
	the same concept. 
  =========================================================*/

static inline int replace_inode(Inode** ilval, Inode* newval) {
	Inode* oldval = *ilval;
	*ilval = newval;
	return oldval ? unpin_inode(oldval) : 0;
}

/* Return 1 if dir corresponds to a mount point, either front or back */
int dir_is_mount_point(Inode* dir) 
{
	if(dir->mounted) return 1;
	FsMount* mnt = i_mnt(dir);
	if(mnt->mount_point && mnt->root_dir == i_id(dir)) return 1;
	return 0;
}

/* Return a pin to dir's front, consuming dir */
Inode* dir_front(Inode* dir)
{
	if(dir->mounted)
		return pin_inode(dir->mounted, dir->mounted->root_dir);
	else 
		return repin_inode(dir);
}


/* Return new pin to dir's back  */
Inode* dir_back(Inode* dir) 
{
	FsMount* mnt = i_mnt(dir);
	if(mnt->mount_point && mnt->root_dir == i_id(dir)) 
		return repin_inode(mnt->mount_point);
	else
		return repin_inode(dir);
}


/*
	Get a new pin on the parent of dir.
 */
Inode* dir_parent(Inode* dir)
{
	Inode* bdir AUTO_UNPIN = dir_back(dir);
	inode_t par_id;

	if(i_fetch(bdir, "..", &par_id, 0)!=0) {
		/* Oh dear, we are deleted!! */
		return NULL;
	}

	Inode* parent AUTO_UNPIN = pin_inode(i_mnt(bdir), par_id);
	return dir_front(parent);
}

/* 
	Return the name of a directory, mount aware.
	If D is the root of a mounted file system,
	the name of D is the name of its mount point.
*/
int dir_name(Inode* dir, pathcomp_t name)
{
	Inode* bdir AUTO_UNPIN = dir_back(dir);
	return i_status(bdir, NULL, name);
}


/* Return true if the name exists in the directory */
int dir_name_exists(Inode* _dir, const pathcomp_t name)
{
	assert(i_type(_dir)==FSE_DIR);
	Inode* dir AUTO_UNPIN = dir_front(_dir);
	if(strcmp(name, ".")==0 || strcmp(name, "..")==0) return 1;
	inode_t id;
	return i_fetch(dir, name, &id, 0) == 0;
}




/* 
	Resolve a member of this directory. A new pin is acquired on it.
	With the creat flag, potentially create a regular file.

	This call knows to traverse mount points. 
	Assume  /mnt/foo is a mount point for mount M and X is the root of M,
	and let Y = dir_lookup(X, "..");
	Then,
	(a) Y corresponds to /mnt (on mount M') and
	(b) dir_lookup(Y, "foo") returns X on mount M 
	  (instead of whatever was /mnt/foo on M')

	A particular idiom is the following:
	// Inode* dir = ...  defined previously

	dir = dir_fetch(dir, ".", 0);
	unpin_inode(dir);

	This ensures that if dir pointed to a mount point, it now points
	to the mounted filesystem.
 */
Inode* dir_fetch(Inode* dir, const pathcomp_t name, int creat)
{
	/* Check the easy cases */
	if(strcmp(name, "..")==0) 
		return dir_parent(dir);

	if(strcmp(name, ".")==0) 
		return dir_front(dir);

	/* Take the front */
	Inode* fdir AUTO_UNPIN = dir_front(dir);

	/* Do the lookup */
	inode_t id;
	int rc = i_fetch(fdir, name, &id, creat);
	if(rc != 0) { set_errcode(rc); return NULL; }

	/* Pin the fetched i-node */
	Inode* inode AUTO_UNPIN = pin_inode(i_mnt(dir), id);
	if(inode==NULL) return NULL;
	
	return dir_front(inode);
}


/*
	Create a new file system element in a directory. 

	This call can be used to create new members in a directory.
 */
Inode* dir_create(Inode* dir, const pathcomp_t name, Fse_type type, void* data)
{
	Inode* fdir AUTO_UNPIN = dir_front(dir);
	FsMount* mnt = i_mnt(fdir);
	FSystem* fsys = mnt->fsys;
	inode_t new_id;

	int rc = fsys->Create(mnt->fsmount, i_id(fdir), name, type, &new_id, data);
	if(rc != 0) { set_errcode(rc); return NULL; }
	return pin_inode(mnt, new_id);
}


/*
	Mount a file system on mpoint. Return 0 on success or an error code.
 */
int dir_mount(Inode* mpt, Dev_t device, FSystem* fsys, unsigned int pmc, mount_param* pmv)
{
	assert(mpt);
	assert(fsys);
	/* TODO: check that the device is not busy */

	/* TODO 2: Check that the device and fstype agree */

	/* Take the mpoint back (if not null) */
	Inode* mpoint AUTO_UNPIN = dir_back(mpt);
	if(mpoint->mounted) return EBUSY;

	/* Get the mount record */
	FsMount* mnt = mount_acquire();
	if(mnt==NULL) return ENOMEM;

	int rc = fsys->Mount(& mnt->fsmount, device, pmc, pmv);
	if(rc==0) {
		struct StatFs sfs;
		fsys->StatFs(mnt->fsmount, &sfs); /* This does not fail */
		assert(strcmp(sfs.fs_fsys, fsys->name)==0);

		/* Init fields of mnt */
		mnt->device = device;
		mnt->fsys = fsys;
		mnt->root_dir = sfs.fs_root;

		/* Link mount point and mount object */
		mnt->mount_point = repin_inode(mpoint);
		mpoint->mounted = mnt;

		/* Add new mount to submounts of mount_point's mount */
		FsMount* pmnt = i_mnt(mpoint);
		rlist_push_back(& pmnt->submount_list, & mnt->submount_node);
	}
	return rc;
}


/*
	Unmounts the mount at 'mpoint', if not busy, returns 0 on success,
	or an error code on failure.
 */
int dir_umount(Inode* mpoint)
{
	assert(mpoint!=NULL && mpoint->mounted != NULL);

	FsMount* mnt = mpoint->mounted;
	if(mnt->use_count) return EBUSY;

	assert(is_rlist_empty(& mnt->submount_list));
	assert(mnt->mount_point == mpoint);

	int rc = mnt->fsys->Unmount(mnt->fsmount);
	if(rc==0) {
		/* Detach from the rest of the filesystem */
		if(mnt->mount_point!=NULL) {
			mnt->mount_point->mounted = NULL;
			unpin_inode(mnt->mount_point);
		} 

		rlist_remove(& mnt->submount_node);

		/* Make mount object unreserved */
		mnt->fsys = NULL;
	}
	return rc;
}




/*=========================================================

	Path manipulation

  =========================================================*/


#define PATHSEP '/'

/* 
	Resolve a pathname.
	
	If last is provided (!= NULL), this call splits the
	pathname into two, as in dirname/last. It returns 
	an Inode to dirname and makes last point to the last
	component of pathname. 
	Note: if the pathname ended in '/' (i.e., "/foo/bar/")
	then *last will be set to NULL.

	If last is NULL, this call returns the Inode corresponding
	to the full pathname.

	In both cases, if there is an error the returned value is
	NULL and set_errcode() has been called with the correct 
	error code.
*/
Inode* resolve_pathname(const char* pathname, const char** last)
{
	int pathlen = strlen(pathname);
	if(pathlen > MAX_PATHNAME) { set_errcode(ENAMETOOLONG); return NULL; }
	if(pathlen==0) { set_errcode(ENOENT); return NULL; }

	const char* base =  pathname;
	const char* cur;
	Inode* inode = NULL;

	/* Local helper function to advance to the next path component */
	int advance() {
		pathcomp_t comp;
		memcpy(comp, base, (cur-base));
		comp[cur-base] = 0;
		Inode* next = dir_fetch(inode, comp, 0);
		unpin_inode(inode);
		inode = next;		
		return next!=NULL; 
	}

	/* Local helper function to check the length of the current path component */
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

	/* Obtain a new pin, making sure we are on a top-level inode */
	inode = dir_fetch(inode, ".", 0);

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
	} else 
		*last = base;

	return inode;
 }



int get_pathname(Inode* dir, char* buffer, unsigned int size)
{	
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
			rc = dir_name(dir, comp);
			if(rc!=0) return -1;

			if(bprintf("/")!=0) return -1;
			return bprintf(comp);
		}
	}

	return print_path_rec(dir, 0);
}






/*=========================================================


	VFS system calls


  =========================================================*/




Fid_t sys_Open(const char* pathname, int flags)
{
	Fid_t fid;
	FCB* fcb;

	/* Check the flags */
	if( ((flags&OPEN_APPEND) || (flags&OPEN_TRUNC)) && !(flags&OPEN_WRONLY) ) {
		set_errcode(EINVAL);
		return -1;
	}

	/* Try to reserve ids */
	if(! FCB_reserve(1, &fid, &fcb)) {
		return NOFILE;
	}
	/* RESOURCE: Now we have reserved fid/fcb pair */

	/* Take the path */
	const char* last;
	Inode* dir AUTO_UNPIN = resolve_pathname(pathname, &last);
	if(dir==NULL) {
        FCB_unreserve(1, &fid, &fcb);		
		return NOFILE;
	}
	if(*last == '\0') 
		/* The pathname ended in '/', fix it! */
		last = ".";

	/* Try to fetch or create the file */
	Inode* file AUTO_UNPIN = NULL;

	if(flags & OPEN_EXCL) 
		file = dir_create(dir, last, FSE_FILE, NULL);
	else
		file = dir_fetch(dir, last, (flags & OPEN_CREAT) ? 1 : 0);

	/* Have we succeeded? */
	if(file==NULL) {
		FCB_unreserve(1, &fid, &fcb);
		return NOFILE;
	}

	/* Possibly truncate the file */
	if(flags & OPEN_TRUNC) {
		int rc = i_truncate(file, 0);
		if(rc) {
			set_errcode(rc);
			FCB_unreserve(1, &fid, &fcb);
			return NOFILE;		
		}
	}

	/* Perform the opening, pass status flags to open */
	int rc = i_open(file, flags & 0xff, &fcb->streamobj, &fcb->streamfunc);
	if(rc!=0) {
		/* Error in open() */
		set_errcode(rc);
		FCB_unreserve(1, &fid, &fcb);
		return NOFILE;
	}

	/* Success! */
	return fid;
}



int sys_Stat(const char* pathname, struct Stat* statbuf)
{	
	/* Look it up */
	Inode* inode AUTO_UNPIN = resolve_pathname(pathname, NULL);
	if(inode==NULL) return -1;

	int rc = i_status(inode, statbuf, NULL);
	if(rc) { set_errcode(rc); return -1; }
	return 0;
}




int sys_Link(const char* pathname, const char* newpath)
{
	/* Check new path */
	const char* last;
	Inode* newdir AUTO_UNPIN = resolve_pathname(newpath, &last);

	if(newdir == NULL) return -1;
	if(*last=='\0' || dir_name_exists(newdir, last)) { set_errcode(EEXIST); return -1; }

	Inode* old AUTO_UNPIN = resolve_pathname(pathname, NULL);
	if(old==NULL) return -1;

	/* They must be in the same FS */
	if(i_mnt(old) != i_mnt(newdir)) { set_errcode(EXDEV); return -1; }

	int rc = i_link(newdir, last, i_id(old));
	if(rc) { set_errcode(rc); return -1; }
	return 0;
}



int sys_Unlink(const char* pathname)
{
	const char* last;
	Inode* dir AUTO_UNPIN = resolve_pathname(pathname, &last);

	if(dir==NULL) return -1;
	if(*last=='\0') { set_errcode(EISDIR); return -1; }

	Inode* inode AUTO_UNPIN = dir_fetch(dir, last, 0);

	/* The i-node unlink can treat directories, but we must not let it ... */
	if(i_type(inode)==FSE_DIR) {
		set_errcode(EISDIR);
		return -1;
	}
	
	int rc = i_unlink(dir, last);
	if(rc) { set_errcode(rc); return -1; }
	return 0;
}


int sys_MkDir(const char* pathname)
{
	const char* last;
	Inode* dir AUTO_UNPIN = resolve_pathname(pathname, &last);

	if(dir==NULL) return -1;
	if(*last=='\0' || dir_name_exists(dir, last)) { set_errcode(EEXIST); return -1; }
	if(i_type(dir)!=FSE_DIR) { set_errcode(ENOTDIR); return -1; }

	Inode* newdir AUTO_UNPIN = dir_create(dir, last, FSE_DIR, NULL);
	return (newdir == NULL)?-1:0;
}


int sys_RmDir(const char* pathname)
{
	Inode* dir AUTO_UNPIN = resolve_pathname(pathname, NULL);
	if(dir==NULL) { return -1; }
	if(i_type(dir)!=FSE_DIR) { set_errcode(ENOTDIR); return -1; }

	if(dir_is_mount_point(dir)) { set_errcode(EBUSY); return -1; }

	Inode* parent AUTO_UNPIN = dir_parent(dir);
	if(parent==NULL) return 0; /* dir is already deleted! */

	/* Parent is a mount point! dir is on the back! */
	if(i_mnt(dir) != i_mnt(parent)) {
		assert(dir_is_mount_point(parent) && parent->mounted == NULL);
		replace_inode(&parent, dir_back(parent));
	}
	assert(i_mnt(dir) == i_mnt(parent));

	pathcomp_t name;
	int rc = dir_name(dir, name);
	assert(rc==0);

	rc = i_unlink(parent, name);
	if(rc) { set_errcode(rc); return -1; }
	return 0;
}


int sys_GetCwd(char* buffer, unsigned int size)
{
	Inode* curdir = CURPROC->cur_dir;
	return get_pathname(curdir, buffer, size);
}


int sys_ChDir(const char* pathname)
{
	Inode* dir AUTO_UNPIN = resolve_pathname(pathname, NULL);
	if(dir==NULL)  return -1;
	if(i_type(dir)!=FSE_DIR) {
		set_errcode(ENOTDIR);
		return -1;
	}

	/* Swap CURPROC->cur_dir  with dir */
	Inode* prevcd = CURPROC->cur_dir;
	CURPROC->cur_dir = dir;
	dir = prevcd;

	return 0;
}


int sys_Mount(Dev_t device, const char* mount_point, const char* fstype, unsigned int paramc, mount_param* paramv)
{
	/* Find the file system */
	FSystem* fsys = get_fsys(fstype);
	if(fsys==NULL) { set_errcode(ENODEV); return -1; }

	Inode* mpoint AUTO_UNPIN = resolve_pathname(mount_point, NULL);
	if(mpoint==NULL) return -1;
	if(i_type(mpoint) != FSE_DIR) { set_errcode(ENOTDIR); return -1; }

	int rc = dir_mount(mpoint, device, fsys, paramc, paramv);
	if(rc) { set_errcode(rc); return -1; }
	return 0;
}



int sys_Umount(const char* mount_point)
{
	assert(mount_point != NULL);

	/* Look up the mount point */
	Inode* mpoint AUTO_UNPIN = resolve_pathname(mount_point, NULL);
	if(mpoint==NULL) return -1;

	Inode* bmpoint AUTO_UNPIN = dir_back(mpoint);

	if(mpoint == bmpoint) {
		set_errcode(EINVAL);    /* This is not a mount point */
		return -1;
	}

	/* We need to unpin mpoint, or else the mount is used! */
	unpin_inode(mpoint);  mpoint=NULL;
	return dir_umount(bmpoint);
}


int sys_StatFs(const char* pathname, struct StatFs* statfs)
{
	if(statfs == NULL) return set_errcode(EFAULT), -1;
	Inode* inode AUTO_UNPIN = resolve_pathname(pathname, NULL);
	if(inode==NULL) return -1;

	FsMount* mnt = i_mnt(inode);
	FSystem* fsys = mnt->fsys;

	fsys->StatFs(mnt->fsmount, statfs);
	return 0;
}



/*---------------------------------------------
 *
 * Directory listing
 *
 * A dir_list is an object that can be used to
 * make the contents of a directory available
 * to the kernel in a standard format.
 *
 *-------------------------------------------*/

void dir_list_create(dir_list* dlist)
{
	dlist->buffer = NULL;
	dlist->buflen = 0;
	dlist->builder = open_memstream(& dlist->buffer, & dlist->buflen);
}

void dir_list_add(dir_list* dlist, const char* name)
{
	FILE* mfile = dlist->builder;
	unsigned int len = strlen(name);
	assert(len < 256);
    fprintf(mfile, "%02x%s%c", len, name, 0);                  
}

void dir_list_open(dir_list* dlist)
{
	FILE* mfile = dlist->builder;
	fclose(mfile);
	dlist->pos = 0;	
}

int dir_list_read(dir_list* s, char* buf, unsigned int size)
{
	if(size==0) return 0;
	if(s->pos >= s->buflen) return 0;

	size_t remaining_bytes = s->buflen - s->pos;
	size_t txbytes = (remaining_bytes < size) ? remaining_bytes : size ;

	memcpy(buf, s->buffer+s->pos, txbytes);
	s->pos += txbytes;

	return txbytes;
}

intptr_t dir_list_seek(dir_list* s, intptr_t offset, int whence)
{
	intptr_t newpos;
	switch(whence) {
	        case SEEK_SET:
	                newpos = 0; break;
	        case SEEK_CUR: 
	                newpos = s->pos; break;
	        case SEEK_END:
	                newpos = s->buflen; break;
	        default:
	                set_errcode(EINVAL);
	                return -1;
	}

	newpos += offset;
	if(newpos <0 || newpos>= s->buflen) {
	                set_errcode(EINVAL);
	                return -1;              
	}
	s->pos = newpos;
	return newpos;
}

int dir_list_close(dir_list* dlist)
{
	free(dlist->buffer);
	return 0;
}



/*========================================================
  Rootfs is a minimal read-only file system, whose only
  element is the root node. 

  Used as the first system, on which mounting can be done.

  Rootfs exposes /dev which is also stateless, but without
  actually mounting it! It just redirects calls to it.
 ---------------------------------------------------------*/

/* 
	These calls are trivial 
*/
static int root_Mount(MOUNT* mnt, Dev_t dev, 
			unsigned int param_no, mount_param* param_vec)
{ return 0; }

static int root_Unmount(MOUNT mnt) { return 0; }

static int root_Pin(MOUNT mnt, inode_t ino) { return 0; }
static int root_Unpin(MOUNT mnt, inode_t ino) { return 0; }
static int root_Flush(MOUNT mnt, inode_t ino) { return 0; }

static int root_Create(MOUNT mnt, inode_t dir, const pathcomp_t name, Fse_type type, inode_t* newino, void* data)
{ return EROFS; }
static int root_Link(MOUNT mnt, inode_t dir, const pathcomp_t name, inode_t ino) { return EROFS; }
static int root_Unlink(MOUNT mnt, inode_t dir, const pathcomp_t name) { return EROFS; }

/*
	Devfs uses all legal Dev_t values as well as NO_DEVICE for inodes.
	The root of rootfs can then be (MAX_DEV)<<16|0, which is not a
	legal i-node for devfs!
 */

#define ROOT ((inode_t)((MAX_DEV)<<16))

/* Reference to DEVFS driver */
extern FSystem DEVFS;

static int root_Truncate(MOUNT mnt, inode_t fino, intptr_t length) { 
	return (fino==ROOT)? EISDIR : DEVFS.Truncate(mnt, fino, length);
}

static int root_StatFs(MOUNT mnt, struct StatFs* statfs) {
	*statfs = (struct StatFs){ 0, ROOT, "rootfs", 0, 0, 0, 0 };
	return 0;
}


static int root_Fetch(MOUNT mnt, inode_t dir, const pathcomp_t name, inode_t* ino, int createflag)
{ 
	if(dir==ROOT) {
		if((strcmp(name,".")==0 || strcmp(name,"..")==0) ) { *ino=ROOT; return 0; } 
		if(strcmp(name,"dev")==0) { *ino = NO_DEVICE; return 0; }
		return (createflag) ? EROFS : ENOENT;
	}
	if(dir==NO_DEVICE) {
		if(strcmp(name,"..")==0) { *ino=ROOT; return 0; }
	}
	return DEVFS.Fetch(mnt, dir, name, ino, createflag);
}

static dir_list root_dlist = { .buffer = "01.\0" "02..\0" "03dev\0" , .buflen=15, .pos = 0 };
static int root_dlist_close(void* obj) { free(obj); return 0; }
static file_ops root_dir_ops = {
	.Read = (void*)dir_list_read,
	.Seek = (void*)dir_list_seek,
	.Release = root_dlist_close
};

static int root_Open(MOUNT mnt, inode_t ino, int flags, void** obj, file_ops** ops)
{ 
	if(ino!=ROOT) return DEVFS.Open(mnt, ino, flags, obj, ops);
	if(flags!=OPEN_RDONLY) return EISDIR;
	dir_list* dlist = xmalloc(sizeof(dir_list));
	*dlist = root_dlist;
	*obj = dlist;
	*ops = &root_dir_ops;
	return 0;
}

static struct Stat root_stat = { 0, ROOT, FSE_DIR, 2, 0, 2, 0, 0 };

static int root_Status(MOUNT mnt, inode_t ino, struct Stat* status, pathcomp_t name) {
	if(ino!=ROOT && ino!=NO_DEVICE) return DEVFS.Status(mnt, ino, status, name);
	if(ino==NO_DEVICE) {
		int rc = DEVFS.Status(mnt, ino, status, NULL);
		if(name) strncpy(name, "dev", MAX_NAME_LENGTH);
		return rc;
	}
	if(status) { *status = root_stat; }
	if(name) { strncpy(name, "", MAX_NAME_LENGTH+1); }
	return 0;
}

static FSystem root_fsys = {
	.name = "rootfs",
	.Mount = root_Mount,
	.Unmount = root_Unmount,

	.Pin = root_Pin,
	.Unpin = root_Unpin,
	.Flush = root_Flush,
	.Create = root_Create,
	.Fetch = root_Fetch,
	.Open = root_Open,
	.Truncate = root_Truncate,
	.Link = root_Link,
	.Unlink = root_Unlink,
	.Status = root_Status,
	.StatFs = root_StatFs
};


/*=========================================================


	VFS initialization and finalization


  =========================================================*/


/* Initialization of the file system module */
void initialize_filesys()
{
	/* Init mounts */
	for(unsigned i=0; i<MOUNT_MAX; i++) {
		mount_table[i].use_count = 0;
		mount_table[i].fsys = NULL;
		rlnode_init(&mount_table[i].submount_node, &mount_table[i]);
		rlnode_init(&mount_table[i].submount_list, NULL);
	}

	/* Init inode_table and root_node */
	rdict_init(&inode_table, MAX_PROC);

	/* Init mount_table[0] with the root filesystem */
	mount_table[0].use_count = 0;
	mount_table[0].fsys = &root_fsys;
	mount_table[0].device = 0;
	mount_table[0].mount_point = NULL;
	mount_table[0].root_dir = ROOT;
	mount_table[0].fsmount = (MOUNT){ NULL};

}



int umount_all(FsMount* mnt)
{
	while(! is_rlist_empty(& mnt->submount_list)) {
		FsMount* submnt = mnt->submount_list.next->obj;
		int rc = umount_all(submnt);
		if(rc!=0) fprintf(stderr, "Unmounting failed, error = %s\n", strerror(rc));
		if(rc) return rc;
	}
	Inode* mpoint = mnt->mount_point;
	return mpoint ? dir_umount(mpoint) : 0;
}


void finalize_filesys()
{
	/* Unmount root fs */
	int rc = umount_all(mount_table);
	assert(rc==0);

	assert(inode_table.size == 0);
	rdict_destroy(&inode_table);
}



