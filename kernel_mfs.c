#include "kernel_fs.h"
#include "kernel_proc.h"


/* =========================================================

	Memory-based file system
    -----------------

	This is a simple file system that uses memory to store
	its data. All contents of this filesystem are lost at 
	reboot.

  =========================================================*/


#define MEMFS_BLKSIZE  (1<<12)
#define MEMFS_MAX_BLOCKS (1<<8)
#define MEMFS_MAX_FILE  (1<<20)


extern FSystem MEM_FSYS;
extern file_ops MEMFS_DIR;
extern file_ops MEMFS_FILE;


/* Some casts for convenience */

#define DEF_ARGS(M,I) \
struct memfs_mount* M __attribute__((unused)) = _##M .ptr; \
memfs_inode* I  __attribute__((unused)) = (memfs_inode*) _##I ;\


/*
	The control block for memfs mounts

	Note: busy_count is the total number of pinned i-nodes
	and stream handles, active at any point. When this is
	0, the file system can be unmounted.
 */
typedef struct memfs_mount
{
	uintptr_t busy_count;    /* Busy count */

	uintptr_t avail_blocks;  /* This is meaningless currently */
	uintptr_t used_blocks;   /* total number of used blocks */
	uintptr_t num_inodes;	 /* Total i-nodes */

	inode_t root_dir;		/* The root dir of the file system */
} memfs_mnt;



/*
	The control block of an i-node 
 */
typedef struct memfs_inode 
{
	int pinned;  			/* Pinned flag */

	Fse_type type;  		/* type */ 
	unsigned int lnkcount;  /* links to this, includes dir entries and stream handles */

	union {
		/* directory-related */
		struct { 
			pathcomp_t name;	/* own name */
			rdict dentry_dict;	/* dict of entries */
		};

		/* file-related */
		struct {
			size_t size;
			size_t nblocks;
			/* Block list: (void*)[MEMFS_MAX_BLOCKS] */
			void** blocks;
		};
	};
} memfs_inode;



/* Forward declaration */
static int memfs_free_node(memfs_mnt* mnt, memfs_inode* ino);


/* Incr/dect link counts. When lnkcount becomes 0, the i-node is deleted */

static inline void memfs_inclink(memfs_mnt* mnt, memfs_inode* ino)
{
	ino->lnkcount ++;
}

static inline int memfs_declink(memfs_mnt* mnt, memfs_inode* ino)
{
	ino->lnkcount --;
	if(ino->lnkcount == 0)
		return memfs_free_node(mnt, ino);
	return 0;
}

/*------------------------------------------------------
	Directory implementation

	A directory is a dictionary of dentry_node objects.
 -------------------------------------------------------*/


/*
	The following restriction is due to the way a directory stream encodes names.
 */
_Static_assert(MAX_NAME_LENGTH <= 255, "The memfs filesystem currently requires names less than 256 bytes.");


struct dentry_node
{
	/* The directory entry */
	pathcomp_t name;
	memfs_inode* ino;

	/* dict node */
	rlnode dnode;
};

static int __dentry_equal(rlnode* dnode, rlnode_key key) 
{
	struct dentry_node* dn = dnode->obj;
	return strcmp(dn->name, key.str)==0;
}
static inline struct dentry_node* dentry_lookup(memfs_inode* dir, const pathcomp_t name)
{
	assert(dir->type == FSE_DIR);
	rlnode* node = rdict_lookup(& dir->dentry_dict, hash_string(name), name, __dentry_equal);
	return node ? node->obj : NULL;
}
static inline void dentry_add(memfs_mnt* mnt, memfs_inode* dir, const pathcomp_t name, memfs_inode* ino)
{
	assert(dentry_lookup(dir,name) == NULL);
	struct dentry_node* newdnode = xmalloc(sizeof(struct dentry_node));

	strcpy(newdnode->name, name);
	newdnode->ino = ino;
	memfs_inclink(mnt, ino);

	rdict_node_init(&newdnode->dnode, newdnode, hash_string(name));
	rdict_insert(&dir->dentry_dict, &newdnode->dnode);	
}
static inline int dentry_remove(memfs_mnt* mnt, memfs_inode* dir, struct dentry_node* dentry)
{
	memfs_inode* inode = dentry->ino;
	rdict_remove(& dir->dentry_dict, & dentry->dnode);
	free(dentry);
	return memfs_declink(mnt, inode);
}



static memfs_inode* memfs_create_dir(memfs_mnt* mnt, memfs_inode* dir, const pathcomp_t name)
{
	memfs_inode* rinode = xmalloc(sizeof(memfs_inode));

	rinode->pinned = 0;
	rinode->type = FSE_DIR;
	rinode->lnkcount = 0;

	rdict_init(&rinode->dentry_dict, 8);

	/* Initialize depending on whether we are the root */
	if(dir == NULL) {
		dir = rinode;
		rinode->name[0] = '\0';
	} else {
		strncpy(rinode->name, name, MAX_NAME_LENGTH+1);
	}

	dentry_add(mnt, rinode, ".", rinode);
	dentry_add(mnt, rinode, "..", dir);

	return rinode;
}

static int memfs_free_dir(memfs_mnt* mnt, memfs_inode* rinode)
{
	/* Note that the dir must be empty and unreferenced ! */
	assert(rinode->lnkcount == 0 && rinode->pinned == 0);
	assert(rinode->dentry_dict.size == 0);

	rdict_destroy(&rinode->dentry_dict);
	free(rinode);
	return 0;
}


/*
	This stream object is used to provide the list of directory names
	to a process.
 */
struct memfs_dir_stream
{
	dir_list dlist;
	memfs_mnt* mnt;
	memfs_inode* inode;	/* The directory inode */
};


/*
	Return a stream from which the list of current directory member names can be read.
 */
static int memfs_open_dir(memfs_mnt* mnt, memfs_inode* inode, int flags, void** obj, file_ops** ops)
{
	/* The flag must be exactly OPEN_RDONLY */
	if(flags != OPEN_RDONLY) return EISDIR;

	/* Create the stream object */
	struct memfs_dir_stream* s = xmalloc(sizeof(struct memfs_dir_stream));

	/* Build the stream contents */
	dir_list_create(& s->dlist);

	void add_dentry_to_dlist(rlnode* p) {
		struct dentry_node* dn = p->obj;
		dir_list_add(& s->dlist, dn->name);
	}

	rdict_apply(& inode->dentry_dict, add_dentry_to_dlist);
	dir_list_open(& s->dlist);

	/* Initialize the rest */
	s->mnt = mnt;
	s->inode = inode;

	/* Take a handle on the directory */
	memfs_inclink(mnt, inode);
	mnt->busy_count ++;

	/* Return the right values */
	*obj = s;
	*ops = &MEMFS_DIR;

	return 0;
}

int memfs_read_dir(void* this, char* buf, unsigned int size)
{
	struct memfs_dir_stream* s = this;
	return dir_list_read(& s->dlist, buf, size);
}

intptr_t memfs_seek_dir(void* this, intptr_t offset, int which)
{
	struct memfs_dir_stream* s = this;
	return dir_list_seek(& s->dlist, offset, which);
}

static int memfs_close_dir(void* this)
{
	struct memfs_dir_stream* s = this;
	dir_list_close(& s->dlist);

	/* Release handle on directory */
	memfs_declink(s->mnt, s->inode);
	s->mnt->busy_count --;

	free(s);
	return 0;
}


file_ops MEMFS_DIR = {
	.Read = memfs_read_dir,
	.Seek = memfs_seek_dir,
	.Release = memfs_close_dir
};


/*------------------------------

	Regular File implementation

 -------------------------------*/

static memfs_inode* memfs_create_file()
{
	memfs_inode* rinode = xmalloc(sizeof(memfs_inode));
	rinode->pinned = 0;
	rinode->type = FSE_FILE;
	rinode->lnkcount = 0;

	rinode->size = 0;
	rinode->nblocks = 0;
	rinode->blocks = calloc(MEMFS_MAX_BLOCKS, sizeof(void*));

	for(int i=0; i<MEMFS_MAX_BLOCKS; i++)
		rinode->blocks[i] = NULL;

	return rinode;
}

static int memfs_free_file(memfs_mnt* mnt, memfs_inode* inode)
{
	/* Note that the file must be unreferenced ! */
	assert(inode->lnkcount == 0);

	/* Free the blocks */
	for(int i=0; i<MEMFS_MAX_BLOCKS; i++) {
		void* block = inode->blocks[i];
		if(block) free(block);
	}
	mnt->used_blocks -= inode->nblocks;

	/* Free the block array */
	free(inode->blocks);

	/* Free the object */
	free(inode);
	return 0;
}


static int memfs_truncate_file(memfs_mnt* mnt, memfs_inode* rinode, intptr_t size)
{
	if(size < 0) return EINVAL;
	if(size > MEMFS_MAX_FILE) return EINVAL;

	/* Set the size */
	rinode->size = size;

	/* Delete any blocks at and after the current size */
	size_t fromblk = (size+MEMFS_BLKSIZE-1)/MEMFS_BLKSIZE;
	while(fromblk < MEMFS_MAX_BLOCKS) {
		if(rinode->blocks[fromblk] != NULL) {
			free(rinode->blocks[fromblk]);
			rinode->blocks[fromblk] = NULL;
			rinode->nblocks--;
			mnt->used_blocks--;
		}
		fromblk ++;
	}

	return 0;
}


/*
	File stream. It holds the flags, the current position and the inode.
 */
struct memfs_file_stream
{
	int flags;			/* Flags determine allowed operations */
	intptr_t pos;		/* Position in the file */
	memfs_mnt* mnt;		/* The mount */
	memfs_inode* inode;	/* Inode is used to access the file */
};



static int memfs_open_file(memfs_mnt* mnt, memfs_inode* inode, int flags, void** obj, file_ops** ops)
{
	struct memfs_file_stream* s = xmalloc(sizeof(struct memfs_file_stream));
	s->flags = flags;
	s->pos = 0;
	s->mnt = mnt;
	s->inode = inode;
	memfs_inclink(mnt, inode);
	mnt->busy_count ++;

	*obj = s;
	*ops = &MEMFS_FILE;
	return 0;
}


static int memfs_read_file(void* this, char *buf, unsigned int size)
{
	struct memfs_file_stream* s = this;
	struct memfs_inode* rinode = s->inode;

	/* Check that the stream is readable */
	if( (s->flags & OPEN_RDONLY) == 0 ) {
		set_errcode(EINVAL); return -1;
	}

	/* Compute the amount of bytes that will be transferred */
	if(s->pos >= rinode->size) return 0;
	intptr_t remaining_bytes = rinode->size-s->pos;
	size_t tbytes = (size < remaining_bytes)?size:remaining_bytes;

	/* Ok, now iterate accessing sequentially the blocks that need to be accessed to do this transfer */
	size_t rbytes = tbytes;
	while(rbytes>0) {
		/* The "current block" is the block that s->pos falls in. Compute the bytes to be transferred from
		   this block */
		size_t curbk = s->pos / MEMFS_BLKSIZE;  /* current block */
		size_t curoff = s->pos % MEMFS_BLKSIZE;  /* offset of pos in current block */
		size_t qbytes = MEMFS_BLKSIZE - curoff;  /* bytes to the end of this block */

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


static intptr_t memfs_seek_file(void* this, intptr_t offset, int whence)
{
	struct memfs_file_stream* s = this;
	struct memfs_inode* rinode = s->inode;

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
	if(newpos<0 || newpos>MEMFS_MAX_FILE) {
		set_errcode(EINVAL);
		return -1;
	}

	s->pos = newpos;
	return newpos;
}

static int memfs_write_file(void* this, const char* buf, unsigned int size)
{
	struct memfs_file_stream* s = this;
	struct memfs_inode* rinode = s->inode;

	/* Check that the stream is writable */
	if( (s->flags & OPEN_WRONLY) == 0 ) {
		set_errcode(EINVAL); return -1;
	}

	/* If in append mode, seek to the end of the stream */
	if( (s->flags & OPEN_APPEND)!=0 ) {
		memfs_seek_file(this, 0, 2);
	}

	/* Check the case where size==0 */
	if(size==0) return 0;

	/* Check that the write does not exceed max file size */
	if(s->pos + size > MEMFS_MAX_FILE) {
		set_errcode(EFBIG);
		return -1;
	}

	/* Compute the amount of bytes that will be transferred */
	intptr_t remaining_bytes = MEMFS_MAX_FILE-s->pos;
	size_t tbytes = (size < remaining_bytes)?size:remaining_bytes;

	/* Ok, now iterate accessing sequentially the blocks that need to be accessed to do this transfer */
	size_t rbytes = tbytes;
	while(rbytes>0) {
		/* The "current block" is the block that s->pos falls in. Compute the bytes to be transferred from
		   this block */
		size_t curbk = s->pos / MEMFS_BLKSIZE;  /* current block */
		size_t curoff = s->pos % MEMFS_BLKSIZE;  /* offset of pos in current block */
		size_t qbytes = MEMFS_BLKSIZE - curoff;  /* bytes to the end of this block */

		size_t txbuf = (qbytes < rbytes) ? qbytes : rbytes; /* amount to transfer from to this block */

		/* get the block, but note that it may not exist! */
		void* curblock = rinode->blocks[curbk];

		if(curblock == NULL) {
			/* allocate a block */
			curblock = xmalloc(MEMFS_BLKSIZE);
			rinode->blocks[curbk] = curblock;
			rinode->nblocks++;
			s->mnt->used_blocks ++;
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

static int memfs_close_file(void* this)
{
	struct memfs_file_stream* s = this;
	memfs_declink(s->mnt, s->inode);
	s->mnt->busy_count --;

	free(s);
	return 0;
}

static int memfs_truncate_file_stream(void* this, intptr_t size)
{
	struct memfs_file_stream* s = this;

	int rc = memfs_truncate_file(s->mnt, s->inode, size);
	if(rc) { set_errcode(rc); return -1; }
	return 0;
}


file_ops MEMFS_FILE = {
	.Read = memfs_read_file,
	.Write = memfs_write_file,
	.Release = memfs_close_file,
	.Truncate = memfs_truncate_file_stream,
	.Seek = memfs_seek_file
};



/*------------------------------

	FSystem API

 -------------------------------*/

static inline int dir_is_unlinked(memfs_inode* dir) 
{ return dir->type==FSE_DIR && dir->dentry_dict.size == 0; }

static int memfs_create(MOUNT _mnt, inode_t _dir, const pathcomp_t name, Fse_type type, inode_t* newino, void* data)
{
	DEF_ARGS(mnt,dir);
	assert(newino!=NULL);

	if(dir->type != FSE_DIR) return ENOTDIR;
	if(dir_is_unlinked(dir)) return ENOENT;

	struct dentry_node* dnode = dentry_lookup(dir, name);
	if(dnode != NULL) return EEXIST;
	if(strlen(name)==0) return EINVAL;
 
 	memfs_inode* rinode;
	switch(type) {
		case FSE_FILE:
		rinode = memfs_create_file(mnt); break;
		case FSE_DIR:
		rinode = memfs_create_dir(mnt, dir, name); break;
		default:
		return EPERM;
	};
	dentry_add(mnt, dir, name, rinode);
	*newino = (inode_t) rinode;
	mnt->num_inodes ++;

	return 0;
}


static int memfs_fetch(MOUNT _mnt, inode_t _dir, const pathcomp_t name, inode_t* id, int creat)
{
	DEF_ARGS(mnt,dir);
	assert(id!=NULL);

	if(dir->type != FSE_DIR) return ENOTDIR;
	if(dir_is_unlinked(dir)) return ENOENT;

	/* Look up into the dict */
	struct dentry_node* dnode = dentry_lookup(dir, name);
	if(dnode) { 
		*id = (inode_t) dnode->ino;
		return 0; 
	} else { 
		/* Not found, see if we should create it */
		if(! creat) return ENOENT;

		/* Try to create it */
		return memfs_create(_mnt, _dir, name, FSE_FILE, id, NULL);
	}
}



static int memfs_link(MOUNT _mnt, inode_t _dir, const pathcomp_t name, inode_t id)
{
	DEF_ARGS(mnt,dir);

	if(dir->type != FSE_DIR) return ENOTDIR;
	if(dir_is_unlinked(dir)) return ENOENT;

	memfs_inode* linked = (memfs_inode*) id;

	if(linked->type == FSE_DIR) return EPERM;

	if( dentry_lookup(dir, name) != NULL) return EEXIST;
	if(strlen(name)==0) return EINVAL;

	/* All good, add the link */
	dentry_add(mnt, dir, name, linked);

	/* Return success */
	return 0;
}


static int memfs_unlink(MOUNT _mnt, inode_t _dir, const pathcomp_t name)
{
	DEF_ARGS(mnt,dir);

	if(dir->type != FSE_DIR) return ENOTDIR;
	if(dir_is_unlinked(dir)) return ENOENT;
	if(strcmp(name,".")==0 || strcmp(name,"..")==0) return EPERM;

	/* Do a dict lookup. */
	struct dentry_node* dentry = dentry_lookup(dir, name);
	if(dentry == NULL) return ENOENT;

	memfs_inode* inode = dentry->ino;

	/* 
		Unlinking a directory is special. Check that the directory is "empty", i.e.,
		it only contains "." and ".."
	*/
	if(inode->type == FSE_DIR) {
		if(inode->dentry_dict.size > 2) return ENOTEMPTY;
		assert(inode->dentry_dict.size == 2);

		/* Remove the two special names */
		dentry_remove(mnt, inode, dentry_lookup(inode,".."));
		dentry_remove(mnt, inode, dentry_lookup(inode,"."));
	}

	/* Remove the dentry */
	return dentry_remove(mnt, dir, dentry);

}


static int memfs_truncate(MOUNT _mnt, inode_t _inode, intptr_t length)
{
	DEF_ARGS(mnt,inode);

	if(inode->type != FSE_FILE) return EISDIR;
	return memfs_truncate_file(mnt, inode, length);
}

static int memfs_status(MOUNT _mnt, inode_t _inode, struct Stat* st, pathcomp_t name)
{
	DEF_ARGS(mnt,inode);

	if(name!=NULL) {
		if(inode->type == FSE_DIR)
			strncpy(name, inode->name, MAX_NAME_LENGTH+1);
		else
			return ENOTDIR;
	}

	if(st != NULL) {
		st->st_dev = 0;
		st->st_ino = _inode;
		st->st_type = inode->type;
		st->st_nlink = inode->lnkcount - inode->pinned;

		st->st_rdev = NO_DEVICE;
		st->st_blksize = MEMFS_BLKSIZE;

		switch(inode->type) {
		case FSE_FILE:
			st->st_size = inode->size;
			st->st_blocks = inode->nblocks;
			break;
		case FSE_DIR:
			st->st_size = inode->dentry_dict.size;
			st->st_blocks = 0;
			break;
		default:
			assert(0);
		}
	}

	return 0;
}



static int memfs_free_node(memfs_mnt* mnt, memfs_inode* inode)
{
	mnt->num_inodes --;
	switch(inode->type) {
		case FSE_FILE:
			return memfs_free_file(mnt, inode);
		case FSE_DIR:
			return memfs_free_dir(mnt, inode);
		default:
			/* What is this? This is probably due to memory corruption,
			   therefore it counts as an I/O error ! */
			return EIO;
	};
}


static int memfs_open(MOUNT _mnt, inode_t _inode, int flags, void** obj, file_ops** ops)
{
	DEF_ARGS(mnt,inode);

	switch(inode->type) {
		case FSE_FILE:
			return memfs_open_file(mnt, inode, flags, obj, ops);

		case FSE_DIR:
			return memfs_open_dir(mnt, inode, flags, obj, ops);

		default:
			assert(0);
			return EIO;
	};
}



static int memfs_mount(MOUNT* mntp, Dev_t dev, unsigned int pc, mount_param* pv)
{
	/* The only purpose of this file system is to create the mount point and inode
	   for the system root. Therefore, there is no mountpoint to speak of, nor is
	   there an associated device */

	if(dev!=NO_DEVICE && dev!=0) return ENXIO;

	/* Allocate */
	memfs_mnt* mnt = xmalloc(sizeof(struct memfs_mount));

	mnt->busy_count = 0;
	mnt->avail_blocks = 0;
	mnt->used_blocks = 0;
	mnt->num_inodes = 0;

	/* Make the root node. Also, manually increment num_inodes, since the function does not */

	mnt->root_dir = (inode_t) memfs_create_dir(mnt, NULL, "");
	mnt->num_inodes ++;

	/* Set the return value */
	mntp->ptr = mnt;

	/* Success */
	return 0;
}


static int memfs_statfs(MOUNT _mnt, struct StatFs* sfs)
{
	memfs_mnt* mnt = _mnt.ptr;
	sfs->fs_dev = 0;
	sfs->fs_root = mnt->root_dir;
	sfs->fs_fsys = MEM_FSYS.name;
	sfs->fs_blocks = mnt->avail_blocks;
	sfs->fs_bused = mnt->used_blocks;
	sfs->fs_files = mnt->num_inodes;
	sfs->fs_fused = mnt->num_inodes;

	return 0;
}


/* Recursively delete all data in a non-busy file system */
static void memfs_purge(memfs_mnt* mnt, memfs_inode* inode)
{
	void purge_dentry(rlnode* p) {
		struct dentry_node* dentry = p->obj;
		dentry->ino->lnkcount --;
		if(strcmp(dentry->name, ".")!=0 && strcmp(dentry->name, "..")!=0)
			memfs_purge(mnt, dentry->ino);
		free(dentry);
	}

	if(inode->type == FSE_DIR) {
		/* Purge directory contents */
		rdict_apply_removed(&inode->dentry_dict, purge_dentry);
	}
	if(inode->lnkcount == 0)
		memfs_free_node(mnt, inode);
}


static int memfs_unmount(MOUNT _mnt)
{
	memfs_mnt* mnt = _mnt.ptr;

	if(mnt->busy_count) return EBUSY;

	memfs_purge(mnt, (memfs_inode*) mnt->root_dir);
	free(mnt);
	return 0;
}



static int memfs_pin(MOUNT _mnt, inode_t _ino)
{
	DEF_ARGS(mnt,ino)

	if(ino->pinned == 0) {
		mnt->busy_count ++;
		ino->pinned = 1;
		memfs_inclink(mnt, ino);
	}

	return 0;
}



static int memfs_unpin(MOUNT _mnt, inode_t _ino)
{
	DEF_ARGS(mnt,ino)

	if(ino->pinned) {
		assert(mnt->busy_count > 0);
		mnt->busy_count --;
		ino->pinned = 0;
		return memfs_declink(mnt,ino);
	}
	return 0;
}


static int memfs_flush(MOUNT _mnt, inode_t _ino)
{
	/* Nothing to do here, always successful */
	return 0;
}




FSystem MEM_FSYS = {
	.name = "memfs",

	.Mount = memfs_mount,
	.Unmount = memfs_unmount,

	.Pin = memfs_pin,
	.Unpin = memfs_unpin,
	.Flush = memfs_flush,
	.Create = memfs_create,
	.Fetch = memfs_fetch,
	.Open = memfs_open,
	.Truncate = memfs_truncate,
	.Link = memfs_link,
	.Unlink = memfs_unlink,
	.Status = memfs_status,
	.StatFs = memfs_statfs
};


REGISTER_FSYS(MEM_FSYS)

