#include "kernel_fs.h"
#include "kernel_proc.h"


/* =========================================================

	Memory-based file system
    -----------------

	This is a simple file system that uses memory to store
	its data. All contents of this filesystem are lost at 
	reboot.

  =========================================================*/


#define TMPFS_BLKSIZE  (1<<12)
#define TMPFS_MAX_BLOCKS (1<<8)
#define TMPFS_MAX_FILE  (1<<20)


extern FSystem TMPFS_FSYS;
extern file_ops TMPFS_FILE;


/* Some casts for convenience */

#define DEF_ARGS(M,I) \
struct tmpfs_mount* M __attribute__((unused)) = _##M .ptr; \
tmpfs_inode* I  __attribute__((unused)) = (tmpfs_inode*) _##I ;\


/*
	The control block for tmpfs mounts

	Note: busy_count is the total number of pinned i-nodes
	and stream handles, active at any point. When this is
	0, the file system can be unmounted.
 */
typedef struct tmpfs_mount
{
	uintptr_t busy_count;    /* Busy count */

	uintptr_t avail_blocks;  /* This is meaningless currently */
	uintptr_t used_blocks;   /* total number of used blocks */
	uintptr_t num_inodes;	 /* Total i-nodes */

	inode_t root_dir;		/* The root dir of the file system */
} tmpfs_mnt;



/*
	The control block of an i-node 
 */
typedef struct tmpfs_inode 
{
	int pinned;  			/* Pinned flag */

	Fse_type type;  		/* type */ 
	unsigned int lnkcount;  /* links to this, includes dir entries and stream handles */

	timestamp_t acc, chg, mod; /* time stamps */

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
			/* Block list: (void*)[TMPFS_MAX_BLOCKS] */
			void** blocks;
		};
	};
} tmpfs_inode;



/* Forward declaration */
static tmpfs_inode* tmpfs_alloc_node(tmpfs_mnt* mnt, Fse_type type);
static int tmpfs_free_node(tmpfs_mnt* mnt, tmpfs_inode* ino);

/* Incr/dect link counts. When lnkcount becomes 0, the i-node is deleted */

static inline void tmpfs_inclink(tmpfs_mnt* mnt, tmpfs_inode* ino)
{
	ino->lnkcount ++;
}

static inline int tmpfs_declink(tmpfs_mnt* mnt, tmpfs_inode* ino)
{
	ino->lnkcount --;
	if(ino->lnkcount == 0)
		return tmpfs_free_node(mnt, ino);
	return 0;
}


#define ACC 1
#define MOD 2
#define CHG 4

static void mark_tstamp(tmpfs_inode* ino, int flags)
{
	timestamp_t ts = bios_clock();
	if(flags & ACC) ino->acc = ts;
	if(flags & MOD) ino->mod = ts;
	if(flags & CHG) ino->chg = ts;
}


/*------------------------------------------------------
	Directory implementation

	A directory is a dictionary of dentry_node objects.
 -------------------------------------------------------*/


/*
	The following restriction is due to the way a directory stream encodes names.
 */
_Static_assert(MAX_NAME_LENGTH <= 255, "The tmpfs filesystem currently requires names less than 256 bytes.");


struct dentry_node
{
	/* The directory entry */
	pathcomp_t name;
	tmpfs_inode* ino;

	/* dict node */
	rlnode dnode;
};

static int __dentry_equal(rlnode* dnode, rlnode_key key) 
{
	struct dentry_node* dn = dnode->obj;
	return strncmp(dn->name, key.str, MAX_NAME_LENGTH)==0;
}
static inline struct dentry_node* dentry_lookup(tmpfs_inode* dir, const pathcomp_t name)
{
	assert(dir->type == FSE_DIR);
	rlnode* node = rdict_lookup(& dir->dentry_dict, hash_nstring(name, MAX_NAME_LENGTH), name, __dentry_equal);
	return node ? node->obj : NULL;
}
static inline void dentry_add(tmpfs_mnt* mnt, tmpfs_inode* dir, const pathcomp_t name, tmpfs_inode* ino)
{
	assert(dentry_lookup(dir,name) == NULL);
	struct dentry_node* newdnode = xmalloc(sizeof(struct dentry_node));

	strncpy(newdnode->name, name, MAX_NAME_LENGTH);
	newdnode->ino = ino;
	tmpfs_inclink(mnt, ino);

	rdict_node_init(&newdnode->dnode, newdnode, hash_nstring(name, MAX_NAME_LENGTH));
	rdict_insert(&dir->dentry_dict, &newdnode->dnode);	
}
static inline int dentry_remove(tmpfs_mnt* mnt, tmpfs_inode* dir, struct dentry_node* dentry)
{
	tmpfs_inode* inode = dentry->ino;
	rdict_remove(& dir->dentry_dict, & dentry->dnode);
	free(dentry);
	return tmpfs_declink(mnt, inode);
}



static tmpfs_inode* tmpfs_create_dir(tmpfs_mnt* mnt, tmpfs_inode* dir, const pathcomp_t name)
{
	tmpfs_inode* rinode = tmpfs_alloc_node(mnt, FSE_DIR);

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

static int tmpfs_free_dir(tmpfs_mnt* mnt, tmpfs_inode* rinode)
{
	/* Note that the dir must be empty and unreferenced ! */
	assert(rinode->lnkcount == 0 && rinode->pinned == 0);
	assert(rinode->dentry_dict.size == 0);

	rdict_destroy(&rinode->dentry_dict);
	free(rinode);
	return 0;
}



/*------------------------------

	Regular File implementation

 -------------------------------*/

static tmpfs_inode* tmpfs_create_file(tmpfs_mnt* mnt)
{
	tmpfs_inode* rinode = tmpfs_alloc_node(mnt, FSE_FILE);

	rinode->size = 0;
	rinode->nblocks = 0;
	rinode->blocks = calloc(TMPFS_MAX_BLOCKS, sizeof(void*));

	for(int i=0; i<TMPFS_MAX_BLOCKS; i++)
		rinode->blocks[i] = NULL;

	return rinode;
}

static int tmpfs_free_file(tmpfs_mnt* mnt, tmpfs_inode* inode)
{
	/* Note that the file must be unreferenced ! */
	assert(inode->lnkcount == 0);

	/* Free the blocks */
	for(int i=0; i<TMPFS_MAX_BLOCKS; i++) {
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


static int tmpfs_truncate_file(tmpfs_mnt* mnt, tmpfs_inode* rinode, intptr_t size)
{
	if(size < 0) return EINVAL;
	if(size > TMPFS_MAX_FILE) return EINVAL;

	/* Set the size */
	rinode->size = size;

	/* Delete any blocks at and after the current size */
	size_t fromblk = (size+TMPFS_BLKSIZE-1)/TMPFS_BLKSIZE;
	while(fromblk < TMPFS_MAX_BLOCKS) {
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
struct tmpfs_file_stream
{
	int flags;			/* Flags determine allowed operations */
	intptr_t pos;		/* Position in the file */
	tmpfs_mnt* mnt;		/* The mount */
	tmpfs_inode* inode;	/* Inode is used to access the file */
};



static int tmpfs_open_file(tmpfs_mnt* mnt, tmpfs_inode* inode, int flags, void** obj, file_ops** ops)
{
	struct tmpfs_file_stream* s = xmalloc(sizeof(struct tmpfs_file_stream));
	s->flags = flags;
	s->pos = 0;
	s->mnt = mnt;
	s->inode = inode;
	tmpfs_inclink(mnt, inode);
	mnt->busy_count ++;

	*obj = s;
	*ops = &TMPFS_FILE;
	return 0;
}


static int tmpfs_read_file(void* this, char *buf, unsigned int size)
{
	struct tmpfs_file_stream* s = this;
	struct tmpfs_inode* rinode = s->inode;

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
		size_t curbk = s->pos / TMPFS_BLKSIZE;  /* current block */
		size_t curoff = s->pos % TMPFS_BLKSIZE;  /* offset of pos in current block */
		size_t qbytes = TMPFS_BLKSIZE - curoff;  /* bytes to the end of this block */

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


static intptr_t tmpfs_seek_file(void* this, intptr_t offset, int whence)
{
	struct tmpfs_file_stream* s = this;
	struct tmpfs_inode* rinode = s->inode;

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
	if(newpos<0 || newpos>TMPFS_MAX_FILE) {
		set_errcode(EINVAL);
		return -1;
	}

	s->pos = newpos;
	return newpos;
}

static int tmpfs_write_file(void* this, const char* buf, unsigned int size)
{
	struct tmpfs_file_stream* s = this;
	struct tmpfs_inode* rinode = s->inode;

	/* Check that the stream is writable */
	if( (s->flags & OPEN_WRONLY) == 0 ) {
		set_errcode(EINVAL); return -1;
	}

	/* If in append mode, seek to the end of the stream */
	if( (s->flags & OPEN_APPEND)!=0 ) {
		tmpfs_seek_file(this, 0, 2);
	}

	/* Check the case where size==0 */
	if(size==0) return 0;

	/* Check that the write does not exceed max file size */
	if(s->pos + size > TMPFS_MAX_FILE) {
		set_errcode(EFBIG);
		return -1;
	}

	/* Compute the amount of bytes that will be transferred */
	intptr_t remaining_bytes = TMPFS_MAX_FILE-s->pos;
	size_t tbytes = (size < remaining_bytes)?size:remaining_bytes;

	/* Ok, now iterate accessing sequentially the blocks that need to be accessed to do this transfer */
	size_t rbytes = tbytes;
	while(rbytes>0) {
		/* The "current block" is the block that s->pos falls in. Compute the bytes to be transferred from
		   this block */
		size_t curbk = s->pos / TMPFS_BLKSIZE;  /* current block */
		size_t curoff = s->pos % TMPFS_BLKSIZE;  /* offset of pos in current block */
		size_t qbytes = TMPFS_BLKSIZE - curoff;  /* bytes to the end of this block */

		size_t txbuf = (qbytes < rbytes) ? qbytes : rbytes; /* amount to transfer from to this block */

		/* get the block, but note that it may not exist! */
		void* curblock = rinode->blocks[curbk];

		if(curblock == NULL) {
			/* allocate a block */
			curblock = xmalloc(TMPFS_BLKSIZE);
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

static int tmpfs_close_file(void* this)
{
	struct tmpfs_file_stream* s = this;
	tmpfs_declink(s->mnt, s->inode);
	s->mnt->busy_count --;

	free(s);
	return 0;
}

static int tmpfs_truncate_file_stream(void* this, intptr_t size)
{
	struct tmpfs_file_stream* s = this;

	int rc = tmpfs_truncate_file(s->mnt, s->inode, size);
	if(rc) { set_errcode(rc); return -1; }
	return 0;
}


file_ops TMPFS_FILE = {
	.Read = tmpfs_read_file,
	.Write = tmpfs_write_file,
	.Release = tmpfs_close_file,
	.Truncate = tmpfs_truncate_file_stream,
	.Seek = tmpfs_seek_file
};



/*------------------------------

	FSystem API

 -------------------------------*/

static inline int dir_is_unlinked(tmpfs_inode* dir) 
{ return dir->type==FSE_DIR && dir->dentry_dict.size == 0; }

static int tmpfs_create(MOUNT _mnt, inode_t _dir, const pathcomp_t name, Fse_type type, inode_t* newino, void* data)
{
	DEF_ARGS(mnt,dir);
	assert(newino!=NULL);

	if(dir->type != FSE_DIR) return ENOTDIR;
	if(dir_is_unlinked(dir)) return ENOENT;

	struct dentry_node* dnode = dentry_lookup(dir, name);
	if(dnode != NULL) return EEXIST;
	if(strnlen(name, MAX_NAME_LENGTH)==0) return EINVAL;
 
 	tmpfs_inode* rinode;
	switch(type) {
		case FSE_FILE:
		rinode = tmpfs_create_file(mnt); break;
		case FSE_DIR:
		rinode = tmpfs_create_dir(mnt, dir, name); break;
		default:
		return EPERM;
	};
	dentry_add(mnt, dir, name, rinode);
	*newino = (inode_t) rinode;

	return 0;
}


static int tmpfs_fetch(MOUNT _mnt, inode_t _dir, const pathcomp_t name, inode_t* id, int creat)
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
		return tmpfs_create(_mnt, _dir, name, FSE_FILE, id, NULL);
	}
}



static int tmpfs_link(MOUNT _mnt, inode_t _dir, const pathcomp_t name, inode_t id)
{
	DEF_ARGS(mnt,dir);

	if(dir->type != FSE_DIR) return ENOTDIR;
	if(dir_is_unlinked(dir)) return ENOENT;

	tmpfs_inode* linked = (tmpfs_inode*) id;

	if(linked->type == FSE_DIR) return EPERM;

	if( dentry_lookup(dir, name) != NULL) return EEXIST;
	if(strnlen(name, MAX_NAME_LENGTH)==0) return EINVAL;

	/* All good, add the link */
	dentry_add(mnt, dir, name, linked);

	/* Return success */
	return 0;
}


static int tmpfs_unlink(MOUNT _mnt, inode_t _dir, const pathcomp_t name)
{
	DEF_ARGS(mnt,dir);

	if(dir->type != FSE_DIR) return ENOTDIR;
	if(dir_is_unlinked(dir)) return ENOENT;
	if(strcmp(name,".")==0 || strcmp(name,"..")==0) return EPERM;

	/* Do a dict lookup. */
	struct dentry_node* dentry = dentry_lookup(dir, name);
	if(dentry == NULL) return ENOENT;

	tmpfs_inode* inode = dentry->ino;

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


static int tmpfs_truncate(MOUNT _mnt, inode_t _inode, intptr_t length)
{
	DEF_ARGS(mnt,inode);

	if(inode->type != FSE_FILE) return EISDIR;
	return tmpfs_truncate_file(mnt, inode, length);
}

static int tmpfs_status(MOUNT _mnt, inode_t _inode, struct Stat* st, pathcomp_t name)
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
		st->st_blksize = TMPFS_BLKSIZE;
		st->st_access = inode->acc;
		st->st_modify = inode->mod;
		st->st_change = inode->chg;

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


static tmpfs_inode* tmpfs_alloc_node(tmpfs_mnt* mnt, Fse_type type)
{
	tmpfs_inode* inode = xmalloc(sizeof(tmpfs_inode));
	mnt->num_inodes ++;

	inode->pinned = 0;
	inode->type = type;
	inode->lnkcount = 0;
	mark_tstamp(inode, ACC|MOD|CHG);

	return inode;
}


static int tmpfs_free_node(tmpfs_mnt* mnt, tmpfs_inode* inode)
{
	mnt->num_inodes --;
	switch(inode->type) {
		case FSE_FILE:
			return tmpfs_free_file(mnt, inode);
		case FSE_DIR:
			return tmpfs_free_dir(mnt, inode);
		default:
			/* What is this? This is probably due to memory corruption,
			   therefore it counts as an I/O error ! */
			return EIO;
	};
}


static int tmpfs_open(MOUNT _mnt, inode_t _inode, int flags, void** obj, file_ops** ops)
{
	DEF_ARGS(mnt,inode);

	switch(inode->type) {
		case FSE_FILE:
			return tmpfs_open_file(mnt, inode, flags, obj, ops);

		case FSE_DIR:
			return EISDIR;

		default:  /* Not yet implemented */
			assert(0);
			return EIO;
	};
}


/*
	Return a stream from which the list of current directory member names can be read.
 */
static int tmpfs_list_dir(MOUNT _mnt, inode_t _inode, struct dir_list* dlist)
{
	DEF_ARGS(mnt,inode);

	/* Check */
	if(inode->type != FSE_DIR) return ENOTDIR;
	if(dir_is_unlinked(inode)) return ENOENT;

	/* Build the stream contents */
	void add_dentry_to_dlist(rlnode* p) {
		struct dentry_node* dn = p->obj;
		dir_list_add(dlist, dn->name);
	}

	rdict_apply(& inode->dentry_dict, add_dentry_to_dlist);

	return 0;
}




static int tmpfs_mount(MOUNT* mntp, Dev_t dev, unsigned int pc, mount_param* pv)
{
	/* The only purpose of this file system is to create the mount point and inode
	   for the system root. Therefore, there is no mountpoint to speak of, nor is
	   there an associated device */

	if(dev!=NO_DEVICE && dev!=0) return ENXIO;

	/* Allocate */
	tmpfs_mnt* mnt = xmalloc(sizeof(struct tmpfs_mount));

	mnt->busy_count = 0;
	mnt->avail_blocks = 0;
	mnt->used_blocks = 0;
	mnt->num_inodes = 0;

	/* Make the root node. Also, manually increment num_inodes, since the function does not */

	mnt->root_dir = (inode_t) tmpfs_create_dir(mnt, NULL, "");
	mnt->num_inodes ++;

	/* Set the return value */
	mntp->ptr = mnt;

	/* Success */
	return 0;
}


static int tmpfs_statfs(MOUNT _mnt, struct StatFs* sfs)
{
	tmpfs_mnt* mnt = _mnt.ptr;
	sfs->fs_dev = 0;
	sfs->fs_root = mnt->root_dir;
	sfs->fs_fsys = TMPFS_FSYS.name;
	sfs->fs_blocks = mnt->avail_blocks;
	sfs->fs_bused = mnt->used_blocks;
	sfs->fs_files = mnt->num_inodes;
	sfs->fs_fused = mnt->num_inodes;

	return 0;
}


/* Recursively delete all data in a non-busy file system */
static void tmpfs_purge(tmpfs_mnt* mnt, tmpfs_inode* inode)
{
	void purge_dentry(rlnode* p) {
		struct dentry_node* dentry = p->obj;
		dentry->ino->lnkcount --;
		if(strcmp(dentry->name, ".")!=0 && strcmp(dentry->name, "..")!=0)
			tmpfs_purge(mnt, dentry->ino);
		free(dentry);
	}

	if(inode->type == FSE_DIR) {
		/* Purge directory contents */
		rdict_apply_removed(&inode->dentry_dict, purge_dentry);
	}
	if(inode->lnkcount == 0)
		tmpfs_free_node(mnt, inode);
}


static int tmpfs_unmount(MOUNT _mnt)
{
	tmpfs_mnt* mnt = _mnt.ptr;

	if(mnt->busy_count) return EBUSY;

	tmpfs_purge(mnt, (tmpfs_inode*) mnt->root_dir);
	free(mnt);
	return 0;
}



static int tmpfs_pin(MOUNT _mnt, inode_t _ino)
{
	DEF_ARGS(mnt,ino)

	if(ino->pinned == 0) {
		mnt->busy_count ++;
		ino->pinned = 1;
		tmpfs_inclink(mnt, ino);
	}

	return 0;
}



static int tmpfs_unpin(MOUNT _mnt, inode_t _ino)
{
	DEF_ARGS(mnt,ino)

	if(ino->pinned) {
		assert(mnt->busy_count > 0);
		mnt->busy_count --;
		ino->pinned = 0;
		return tmpfs_declink(mnt,ino);
	}
	return 0;
}


static int tmpfs_flush(MOUNT _mnt, inode_t _ino)
{
	/* Nothing to do here, always successful */
	return 0;
}




FSystem TMPFS_FSYS = {
	.name = "tmpfs",

	.Mount = tmpfs_mount,
	.Unmount = tmpfs_unmount,

	.Pin = tmpfs_pin,
	.Unpin = tmpfs_unpin,
	.Flush = tmpfs_flush,
	.Create = tmpfs_create,
	.Fetch = tmpfs_fetch,
	.Open = tmpfs_open,
	.ListDir = tmpfs_list_dir,
	.Truncate = tmpfs_truncate,
	.Link = tmpfs_link,
	.Unlink = tmpfs_unlink,
	.Status = tmpfs_status,
	.StatFs = tmpfs_statfs
};


REGISTER_FSYS(TMPFS_FSYS)

