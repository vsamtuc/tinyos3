
struct rootfs_mount;
typedef struct rootfs_mount* rootfs_mount_p;

#define MOUNT rootfs_mount_p


#include "kernel_fs.h"
#include "kernel_proc.h"


/*=========================================================

	Root file system
    -----------------

	This is a minimal file system that is useful as a base 
	for mount points. All contents of this filesystem are 
	lost at reboot.

  =========================================================*/


#define ROOTFS_BLKSIZE  (1<<12)
#define ROOTFS_MAX_BLOCKS (1<<8)
#define ROOTFS_MAX_FILE  (1<<20)


extern FSystem ROOT_FSYS;
extern file_ops ROOTFS_DIR;
extern file_ops ROOTFS_FILE;

/*
	Rootfs off-core inode base. These attributes
	are at the beginning of every inode.
*/
struct rootfs_mount
{
	uintptr_t avail_blocks;
	uintptr_t used_blocks;
	uintptr_t num_inodes;

	inode_t root_dir;
};


#define ROOTFS_INODE_BASE  \
struct { \
	Fse_type type;  		/* type */ \
	unsigned int lnkcount;  /* links to this */	\
} \

struct rootfs_base_inode 
{
	ROOTFS_INODE_BASE;	
};

static int rootfs_pin_inode(Inode* inode)
{
	/* The inode id is equal to the pointer of the inode */
	inode->fsinode = (void*) inode_id(inode);
	return 0;
}


/* Forward declaration */
static int rootfs_free_node(MOUNT mnt, inode_t id);


static int rootfs_unpin_inode(Inode* inode)
{
	/* We should free an unpinned inode with a lnkcount of 0 */
	struct rootfs_base_inode* fsinode = inode->fsinode;
	inode->fsinode = NULL;

	if(fsinode->lnkcount == 0)
		return rootfs_free_node(inode_mnt(inode)->fsmount, inode_id(inode));

	return 0;
}


static int rootfs_flush_inode(Inode* inode)
{
	/* Nothing to do here, always successful */
	return 0;
}




/*----------------------------

	Directory implementation

 ----------------------------*/



struct dentry_node
{
	/* The directory entry */
	pathcomp_t name;
	inode_t id;

	/* list and dict nodes */
	rlnode lnode, dnode;
};

struct rootfs_dir_inode
{
	ROOTFS_INODE_BASE;
	inode_t parent;		/* id of parent */
	pathcomp_t name;
	rlnode dentry_list;		/* entries */
	rdict dentry_dict;
};


void rootfs_status_dir(struct rootfs_dir_inode* inode, struct Stat* st, int which)
{
	if(which & STAT_SIZE)	st->st_size = inode->dentry_dict.size;
	if(which & STAT_BLKNO)	st->st_blocks = 0;
}


void rootfs_dir_name(struct rootfs_dir_inode* inode, pathcomp_t name)
{
	memcpy(name, inode->name, MAX_NAME_LENGTH+1);
}


static inode_t rootfs_allocate_dir(MOUNT mnt)
{
	struct rootfs_dir_inode* rinode = xmalloc(sizeof(struct rootfs_dir_inode));
	rinode->type = FSE_DIR;
	rinode->lnkcount = 1;
	rinode->parent = (inode_t) rinode;
	rinode->name[0] = 0;
	rlnode_init(&rinode->dentry_list, NULL);
	rdict_init(&rinode->dentry_dict, 64);
	return (inode_t) rinode;
}

static int rootfs_free_dir(struct rootfs_dir_inode* rinode)
{
	/* Note that the dir must be empty and unreferenced ! */
	assert(rinode->lnkcount == 0);
	assert(is_rlist_empty(&rinode->dentry_list));
	assert(rinode->dentry_dict.size == 0);

	rdict_destroy(&rinode->dentry_dict);
	free(rinode);
	return 0;
}


static int dentry_equal(rlnode* dnode, rlnode_key key) 
{
	struct dentry_node* dn = dnode->obj;
	return strcmp(dn->name, key.str)==0;
}


static int rootfs_lookup(Inode* this, const pathcomp_t name, inode_t* id)
{
	assert(id!=NULL);

	struct rootfs_dir_inode* fsinode = this->fsinode;
	if(fsinode->type != FSE_DIR) { set_errcode(ENOTDIR); return -1; }

	if(fsinode->lnkcount == 0) { set_errcode(ENOENT); return -1; }
	if(strcmp(name, ".")==0) { *id = inode_id(this); return 0; }
	if(strcmp(name, "..")==0) { *id = fsinode->parent; return 0; }

	/* Look up into the dict */
	rlnode* dnode = rdict_lookup(&fsinode->dentry_dict, hash_string(name), name, NULL, dentry_equal);
	if(dnode) { *id = ((struct dentry_node*) dnode->obj)->id; return 0; }
	else { set_errcode(ENOENT); return -1; }
}



static int rootfs_link(Inode* this, const pathcomp_t name, inode_t id)
{
	struct rootfs_dir_inode* fsinode = this->fsinode;
	if(fsinode->type != FSE_DIR) { set_errcode(ENOTDIR); return -1; }

	if(fsinode->lnkcount == 0) { set_errcode(ENOENT); return -1; }
	if(rootfs_lookup(this, name, &id) != -1) { set_errcode(EEXIST); return -1; }

	struct rootfs_base_inode* linked = (void*) id;

	if(linked->type == FSE_DIR) {
		/* 
			Linking a directory is special. Directories cannot have more than one parent.
			Furthermore, a directory always knows its (filesystem) parent.
		 */
		if(linked->lnkcount != 1) { set_errcode(EPERM); return -1; }

		struct rootfs_dir_inode* linked_dir = (void*) linked;

		/* Make us the parent, also increase our own link count */
		linked_dir->parent = inode_id(this);
		fsinode->lnkcount ++;

		/* Copy the name to the linked inode */
		strncpy(linked_dir->name, name, MAX_NAME_LENGTH+1);
	} 

	/* Add to the link count of the linked element */
	linked->lnkcount ++;

	/* Add the link to the directory */
	struct dentry_node* newdnode = xmalloc(sizeof(struct dentry_node));

	strcpy(newdnode->name, name);
	newdnode->id = id;

	rlnode_init(&newdnode->lnode, newdnode);
	rlist_push_back(&fsinode->dentry_list, &newdnode->lnode);

	rlnode_init(&newdnode->dnode, newdnode);
	rdict_insert(&fsinode->dentry_dict, &newdnode->dnode, hash_string(name));

	/* Return success */
	return 0;
}


static int rootfs_unlink(Inode* this, const pathcomp_t name)
{
	struct rootfs_dir_inode* fsinode = this->fsinode;
	if(fsinode->type != FSE_DIR) {
		set_errcode(ENOTDIR);
		return -1;
	}

	if(fsinode->lnkcount == 0) { set_errcode(ENOENT); return -1; }
	if(strcmp(name,".")==0 || strcmp(name,"..")==0) {
		set_errcode(EPERM);
		return -1;
	}

	/* Do a dict lookup. */
	hash_value name_hash = hash_string(name);
	rlnode* dnode = rdict_lookup(&fsinode->dentry_dict, 
			name_hash, name, NULL, dentry_equal);
	if(dnode == NULL) { set_errcode(ENOENT); return -1; }

	struct dentry_node* dentry = dnode->obj;
	struct rootfs_base_inode* einode = (void*) dentry->id;

	if(einode->type == FSE_DIR) {
		/* Unlinking a directory is special */
		struct rootfs_dir_inode* dinode = (void*) einode;

		/* Check that the directory is empty */
		if(dinode->dentry_dict.size > 0) {
			set_errcode(ENOTEMPTY);
			return -1;
		}

		/* Ok, we may decrease the link */
		assert(dinode->lnkcount == 2);
		assert(dinode->parent == inode_id(this));
		dinode->lnkcount = 0;
		dinode->parent = dentry->id;
		fsinode->lnkcount --;
	} else {
		einode->lnkcount --;
	}

	/* Remove the dentry */
	rdict_remove(&fsinode->dentry_dict, dnode, name_hash);
	rlist_remove(&dentry->lnode);
	free(dentry);

	/* 
		In case the unlinked element is to be deleted, we need to wait for
		it to be unpinned, else we should free it now.
	*/
	if(einode->lnkcount==0 && inode_if_pinned(inode_mnt(this), dentry->id)==NULL) {
		return rootfs_free_node(inode_mnt(this)->fsmount, dentry->id);
	}	

	return 0;
}


/*
	This stream object is used to provide the list of directory names
	to a process.
 */
struct rootfs_dir_stream
{
	char* buffer;	/* This buffer contains the full content of the stream */
	size_t buflen;	/* The length of buffer */
	intptr_t pos;	/* Current position in the buffer */
	Inode* inode;	/* The directory inode */
};


/*
	Return a stream from which the list of current directory member names can be read.
 */
static int rootfs_open_dir(Inode* inode, int flags, void** obj, file_ops** ops)
{
	struct rootfs_dir_inode* rinode = inode->fsinode;

	/* The flag must be exactly OPEN_RDONLY */
	if(flags != OPEN_RDONLY) { set_errcode(EISDIR); return -1; }

	/* Create the stream object */
	struct rootfs_dir_stream* s = xmalloc(sizeof(struct rootfs_dir_stream));
	s->buffer = NULL;
	s->buflen = 0;

	/* Build the stream contents */
	FILE* mfile = open_memstream(& s->buffer, & s->buflen);
	if(rinode->lnkcount!=0) {
		fprintf(mfile, "%02x.%c", 1, 0);   /* My self */
		fprintf(mfile, "%02x..%c", 2, 0);  /* My parent */
		/* Everything else */
		for(rlnode* dnode=rinode->dentry_list.next; dnode!=&rinode->dentry_list; dnode=dnode->next) {
			struct dentry_node* dn = dnode->obj;
			fprintf(mfile, "%02x%s%c", (unsigned int)strlen(dn->name), dn->name, 0);
		}
	}
	fclose(mfile);

	/* Finish the initialization */
	s->pos = 0;
	s->inode = inode;
	repin_inode(inode);

	/* Return the right values */
	*obj = s;
	*ops = &ROOTFS_DIR;

	return 0;
}

static int rootfs_read_dir(void* this, char *buf, unsigned int size)
{
	if(size==0) return 0;
	struct rootfs_dir_stream* s = this;
	if(s->pos >= s->buflen) return 0;

	size_t remaining_bytes = s->buflen - s->pos;
	size_t txbytes = (remaining_bytes < size) ? remaining_bytes : size ;

	memcpy(buf, s->buffer+s->pos, txbytes);
	s->pos += txbytes;

	return txbytes;
}

/* static int rootfs_write_dir(void* this, const char* buf, unsigned int size); */

static int rootfs_close_dir(void* this)
{
	struct rootfs_dir_stream* s = this;
	unpin_inode(s->inode);
	free(s->buffer);
	return 0;
}


static intptr_t rootfs_seek_dir(void* this, intptr_t offset, int whence)
{
	struct rootfs_dir_stream* s = this;
	
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


/*------------------------------

	File implementation

 -------------------------------*/


struct rootfs_file_inode
{
	ROOTFS_INODE_BASE;
	size_t size;
	size_t nblocks;
	/* Block list */
	void* blocks[ROOTFS_MAX_BLOCKS];
};

void rootfs_status_file(struct rootfs_file_inode* inode, struct Stat* st, int which)
{
	if(which & STAT_SIZE)	st->st_size = inode->size;
	if(which & STAT_BLKNO)	st->st_blocks = inode->nblocks;
}


static inode_t rootfs_allocate_file(MOUNT mnt)
{
	struct rootfs_file_inode* rinode = xmalloc(sizeof(struct rootfs_file_inode));
	rinode->type = FSE_FILE;
	rinode->lnkcount = 0;

	rinode->size = 0;
	rinode->nblocks = 0;
	for(int i=0; i<ROOTFS_MAX_BLOCKS; i++)
		rinode->blocks[i] = NULL;

	return (inode_t) rinode;
}

static int rootfs_free_file(struct rootfs_file_inode* rinode)
{
	/* Note that the file must be unreferenced ! */
	assert(rinode->lnkcount == 0);

	/* Free the blocks */
	for(int i=0; i<ROOTFS_MAX_BLOCKS; i++) {
		void* block = rinode->blocks[i];
		if(block) free(block);
	}

	/* Free the object */
	free(rinode);
	return 0;
}


static int rootfs_truncate_file(struct rootfs_file_inode* rinode, intptr_t size)
{
	if(size < 0) {  set_errcode(EINVAL); return -1; }
	if(size > ROOTFS_MAX_FILE) {  set_errcode(EFBIG); return -1; }

	/* Set the position */
	rinode->size = size;

	/* Delete any blocks at and after the current size */
	size_t fromblk = (size+ROOTFS_BLKSIZE-1)/ROOTFS_BLKSIZE;
	while(fromblk < ROOTFS_MAX_BLOCKS) {
		if(rinode->blocks[fromblk] != NULL) {
			free(rinode->blocks[fromblk]);
			rinode->blocks[fromblk] = NULL;
			rinode->nblocks--;
		}
		fromblk ++;
	}

	return 0;
}




/*
	File stream. It holds the flags, the current position and the inode.
 */
struct rootfs_file_stream
{
	int flags;		/* Flags determine allowed operations */
	intptr_t pos;	/* Position in the file */
	Inode* inode;	/* Inode is used to access the file */
};



static int rootfs_open_file(Inode* inode, int flags, void** obj, file_ops** ops)
{
	struct rootfs_file_stream* s = xmalloc(sizeof(struct rootfs_file_stream));
	s->flags = flags;
	s->pos = 0;
	s->inode = inode;
	repin_inode(inode);

	/* If truncation is involved, do it now */


	*obj = s;
	*ops = &ROOTFS_FILE;
	return 0;
}


static int rootfs_read_file(void* this, char *buf, unsigned int size)
{
	struct rootfs_file_stream* s = this;
	struct rootfs_file_inode* rinode = s->inode->fsinode;

	/* Compute the amount of bytes that will be transferred */
	if(s->pos >= rinode->size) return 0;
	intptr_t remaining_bytes = rinode->size-s->pos;
	size_t tbytes = (size < remaining_bytes)?size:remaining_bytes;

	/* Ok, now iterate accessing sequentially the blocks that need to be accessed to do this transfer */
	size_t rbytes = tbytes;
	while(rbytes>0) {
		/* The "current block" is the block that s->pos falls in. Compute the bytes to be transferred from
		   this block */
		size_t curbk = s->pos / ROOTFS_BLKSIZE;  /* current block */
		size_t curoff = s->pos % ROOTFS_BLKSIZE;  /* offset of pos in current block */
		size_t qbytes = ROOTFS_BLKSIZE - curoff;  /* bytes to the end of this block */

		size_t txbuf = (qbytes < rbytes) ? qbytes : rbytes; /* amount to transfer from this block */

		/* get the block, but note that it may not exist! */
		void* curblock = rinode->blocks[curbk];
		if(curblock == NULL) {
			/* In this case, we just zero the bytes in buf */
			memset(buf, 0, txbuf);
		} else {
			/* copy bytes */
			memcpy(buf, curblock+curoff, txbuf);
		}
		/* update variables */
		rbytes -= txbuf;
		buf += txbuf;
		s->pos += txbuf;
		/* repeat as necessary */
	}

	return tbytes;
}

static int rootfs_write_file(void* this, const char* buf, unsigned int size)
{
	struct rootfs_file_stream* s = this;
	struct rootfs_file_inode* rinode = s->inode->fsinode;

	/* Compute the amount of bytes that will be transferred */
	if(s->pos == ROOTFS_MAX_FILE) {
		set_errcode(EFBIG);
		return -1;
	}

	intptr_t remaining_bytes = ROOTFS_MAX_FILE-s->pos;
	size_t tbytes = (size < remaining_bytes)?size:remaining_bytes;

	/* Ok, now iterate accessing sequentially the blocks that need to be accessed to do this transfer */
	size_t rbytes = tbytes;
	while(rbytes>0) {
		/* The "current block" is the block that s->pos falls in. Compute the bytes to be transferred from
		   this block */
		size_t curbk = s->pos / ROOTFS_BLKSIZE;  /* current block */
		size_t curoff = s->pos % ROOTFS_BLKSIZE;  /* offset of pos in current block */
		size_t qbytes = ROOTFS_BLKSIZE - curoff;  /* bytes to the end of this block */

		size_t txbuf = (qbytes < rbytes) ? qbytes : rbytes; /* amount to transfer from to this block */

		/* get the block, but note that it may not exist! */
		void* curblock = rinode->blocks[curbk];

		if(curblock == NULL) {
			/* allocate a block */
			curblock = xmalloc(ROOTFS_BLKSIZE);
			rinode->blocks[curbk] = curblock;
			rinode->nblocks++;
		} 

		/* copy bytes */
		memcpy(curblock+curoff, buf, txbuf);

		/* update variables */
		rbytes -= txbuf;
		buf += txbuf;
		s->pos += txbuf;
		/* repeat as necessary */
	}

	/* adjust file size, since we may have extended it */
	if(rinode->size < s->pos)  rinode->size = s->pos;

	return tbytes;	
}



static int rootfs_close_file(void* this)
{
	struct rootfs_file_stream* s = this;
	unpin_inode(s->inode);
	free(s);
	return 0;
}

static int rootfs_truncate_file_stream(void* this, intptr_t size)
{
	struct rootfs_file_stream* s = this;
	struct rootfs_file_inode* inode = s->inode->fsinode;

	return rootfs_truncate_file(inode, size);
}


static intptr_t rootfs_seek_file(void* this, intptr_t offset, int whence)
{
	struct rootfs_file_stream* s = this;
	struct rootfs_file_inode* rinode = s->inode->fsinode;

	intptr_t newpos;
	switch(whence) {
		case SEEK_SET: newpos = 0; break;
		case SEEK_CUR: newpos = s->pos; break;
		case SEEK_END: newpos = rinode->size; break;
		default:
			set_errcode(EINVAL);
			return -1;
	}
	newpos += offset;
	if(newpos<0 || newpos>ROOTFS_MAX_FILE) {
		set_errcode(EINVAL);
		return -1;
	}

	s->pos = newpos;
	return newpos;
}






/*------------------------------

	Generic API

 -------------------------------*/


static void rootfs_status(Inode* inode, struct Stat* st, pathcomp_t name, int which)
{
	if(which & STAT_DEV) st->st_dev = inode_mnt(inode)->device;
	if(which & STAT_INO) st->st_ino = inode_id(inode);

	struct rootfs_base_inode* fsinode = inode->fsinode;
	if(which & STAT_TYPE) 	st->st_type = fsinode->type;
	if(which & STAT_NLINK) 	st->st_nlink = fsinode->lnkcount;

	/* Opt: the above are the generic attributes. Check if we need to
		proceed. */
	which &= ~(STAT_DEV|STAT_INO|STAT_TYPE|STAT_NLINK);
	if(which==0) return;

	if(which & STAT_RDEV) st->st_rdev = NO_DEVICE;
	if(which & STAT_BLKSZ) st->st_blksize = ROOTFS_BLKSIZE;

	switch(fsinode->type) {
		case FSE_FILE:
			rootfs_status_file((struct rootfs_file_inode*)fsinode, st, which);
			if(which & STAT_NAME) {	name[0]=0; }
			break;
		case FSE_DIR:
			rootfs_status_dir((struct rootfs_dir_inode*)fsinode, st, which);
			if(which & STAT_NAME) {	rootfs_dir_name((struct rootfs_dir_inode*)fsinode, name); }
			break;
		default:
			assert(0);
	}

}


static inode_t rootfs_allocate_node(MOUNT this, Fse_type type, Dev_t dev)
{
	switch(type) {
		case FSE_FILE:
		return rootfs_allocate_file(this);
		case FSE_DIR:
		return rootfs_allocate_dir(this);
		default:
		return 0;
	};
}

static int rootfs_free_node(MOUNT mnt, inode_t id)
{
	struct rootfs_base_inode* ino = (void*) id;
	switch(ino->type) {
		case FSE_FILE:
			return rootfs_free_file((struct rootfs_file_inode*)ino);
		case FSE_DIR:
			return rootfs_free_dir((struct rootfs_dir_inode*)ino);
		default:
			/* What is this? This is probably due to memory corruption,
			   therefore it counts as an I/O error ! */
			set_errcode(EIO);
			return -1;
	};
}


static int rootfs_open(Inode* inode, int flags, void** obj, file_ops** ops)
{
	struct rootfs_base_inode* fsinode = inode->fsinode;
	switch(fsinode->type) {
		case FSE_FILE:
			return rootfs_open_file(inode, flags, obj, ops);
		case FSE_DIR:
			return rootfs_open_dir(inode, flags, obj, ops);
		default:
			return -1;
	};
}



static int rootfs_mount(MOUNT* mntp, FSystem* this, Dev_t dev, unsigned int pc, mount_param* pv)
{
	/* The only purpose of this file system is to create the mount point and inode
	   for the system root. Therefore, there is no mountpoint to speak of, nor is
	   there an associated device */

	if(dev!=NO_DEVICE) {
		set_errcode(ENXIO);
		return -1;
	}

	/* Allocate */
	MOUNT mnt = xmalloc(sizeof(struct rootfs_mount));

	mnt->avail_blocks = 0;
	mnt->used_blocks = 0;
	mnt->num_inodes = 1;

	/* Make the root node */
	mnt->root_dir = rootfs_allocate_dir(mnt);

	/* Set the return value */
	*mntp = mnt;

	/* Success */
	return 0;
}


static void rootfs_statfs(MOUNT mnt, struct StatFs* sfs)
{
	sfs->fs_dev = NO_DEVICE;
	sfs->fs_root = mnt->root_dir;
	sfs->fs_fsys = ROOT_FSYS.name;
	sfs->fs_blocks = mnt->avail_blocks;
	sfs->fs_bused = mnt->used_blocks;
	sfs->fs_files = mnt->num_inodes;
	sfs->fs_fused = mnt->num_inodes;
}

static void rootfs_purge(MOUNT mnt, inode_t id)
{
	struct rootfs_base_inode* inode = (void*)id;
	if(inode->type == FSE_DIR) {
		/* Empty all */
		struct rootfs_dir_inode* dir = (void*)inode;

		for(rlnode* n=dir->dentry_list.next; n !=&dir->dentry_list; n = n->next) {
			struct dentry_node* dentry = n->obj;
			rootfs_purge(mnt, dentry->id);
		}

		/* Remove all direntries from the dict */
		rdict_destroy(& dir->dentry_dict);

		/* Iterate over the list and free them */
		while(! is_rlist_empty(&dir->dentry_list)) {
			rlnode* n = rlist_pop_front(& dir->dentry_list);
			struct dentry_node* d = n->obj;
			free(d);
		}
		dir->lnkcount = 0;
	}
	inode->lnkcount = 0;
	rootfs_free_node(mnt, id);
}

static int rootfs_unmount(MOUNT mnt)
{
	/* Recursively delete data */
	rootfs_purge(mnt, mnt->root_dir);
	free(mnt);
	return 0;
}


file_ops ROOTFS_FILE = {
	.Read = rootfs_read_file,
	.Write = rootfs_write_file,
	.Release = rootfs_close_file,
	.Truncate = rootfs_truncate_file_stream,
	.Seek = rootfs_seek_file
};

file_ops ROOTFS_DIR = {
	.Read = rootfs_read_dir,
	.Release = rootfs_close_dir,
	.Seek = rootfs_seek_dir
};

FSystem ROOT_FSYS = {
	.name = "rootfs",
	.Mount = rootfs_mount,
	.Unmount = rootfs_unmount,
	.AllocateNode = rootfs_allocate_node,
	.FreeNode = rootfs_free_node,
	.PinInode = rootfs_pin_inode,
	.UnpinInode = rootfs_unpin_inode,
	.FlushInode = rootfs_flush_inode,
	.OpenInode = rootfs_open,
	.Lookup = rootfs_lookup,
	.Link = rootfs_link,
	.Unlink = rootfs_unlink,
	.Status = rootfs_status,
	.StatFs = rootfs_statfs
};


REGISTER_FSYS(ROOT_FSYS)

