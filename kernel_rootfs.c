
#include "kernel_fs.h"
#include "kernel_proc.h"


/*=========================================================

	Root file system
    -----------------

	This is a minimal file system that
	is only useful as a base for mount points. All contents
	of this filesystem are lost at reboot.

  =========================================================*/


extern FSystem ROOT_FSYS;
extern file_ops ROOT_DIR;
extern file_ops ROOT_FILE;

/*
	Rootfs off-core inode base. These attributes
	are at the beginning of every inode.
*/

#define ROOTFS_INODE_BASE  \
struct { \
	Fse_type type;  		/* type */ \
	unsigned int lnkcount;  /* links to this */	\
} \

struct rootfs_base_inode 
{
	ROOTFS_INODE_BASE;	
};

static void rootfs_fetch_base(Inode* inode)
{
	struct rootfs_base_inode* rinode = (struct rootfs_base_inode*) inode->ino_ref.id;
	inode->type = rinode->type;
	inode->lnkcount = rinode->lnkcount;
	inode->dirty = 0;
}

static void rootfs_flush_base(Inode* inode)
{
	struct rootfs_base_inode* rinode = (struct rootfs_base_inode*) inode->ino_ref.id;
	assert(inode->type == rinode->type);
	rinode->lnkcount = inode->lnkcount;
}


/*----------------------------

	Directory implementation

 ----------------------------*/

struct dentry_node
{
	/* The direntry */
	dir_entry dentry;

	/* list and dict nodes */
	rlnode lnode, dnode;
};

struct rootfs_dir_inode
{
	ROOTFS_INODE_BASE;
	Inode_id parent;		/* id of parent */
	rlnode dentry_list;		/* entries */
	rdict dentry_dict;
};

static Inode_id rootfs_allocate_dir(Mount* mnt)
{
	struct rootfs_dir_inode* rinode = (struct rootfs_dir_inode*)xmalloc(sizeof(struct rootfs_dir_inode));
	rinode->type = FSE_DIR;
	rinode->lnkcount = 0;
	rinode->parent = 0;
	rlnode_init(&rinode->dentry_list, NULL);
	rdict_init(&rinode->dentry_dict, 64);
	return (Inode_id) rinode;
}

static int rootfs_release_dir(Mount* mnt, void* rbinode)
{
	struct rootfs_dir_inode* rinode = (struct rootfs_dir_inode*) rbinode;
	/* Note that the dir must be empty and unreferenced ! */
	assert(rinode->lnkcount == 0);
	assert(rinode->dentry_dict.size == 0);
	rdict_destroy(&rinode->dentry_dict);
	free(rinode);
	return 0;
}

static void rootfs_fetch_dir(Inode* inode)
{
	struct rootfs_dir_inode* rinode = (struct rootfs_dir_inode*) inode->ino_ref.id;
	inode->dir.parent = rinode->parent;
}

static void rootfs_flush_dir(Inode* inode)
{
	struct rootfs_dir_inode* rinode = (struct rootfs_dir_inode*) inode->ino_ref.id;
	rinode->parent = inode->dir.parent;	
}


struct rootfs_dir_stream
{
	char* buffer;
	size_t buflen;
	intptr_t pos;

	Inode* inode;	/* For Stat */
};

static void* rootfs_open_dir(Inode* inode, int flags)
{
	struct rootfs_dir_stream* s = (struct rootfs_dir_stream*)xmalloc(sizeof(struct rootfs_dir_stream));
	s->buffer = NULL;
	s->buflen = 0;

	FILE* mfile = open_memstream(& s->buffer, & s->buflen);
	struct rootfs_dir_inode* rinode = (struct rootfs_dir_inode*) inode->ino_ref.id;
	for(rlnode* dnode=rinode->dentry_list.next; dnode!=&rinode->dentry_list; dnode=dnode->next) {
		struct dentry_node* dn = dnode->obj;
		fprintf(mfile, "%s%c", dn->dentry.name,0);
	}
	fclose(mfile);

	s->pos = 0;
	s->inode = inode;
	inode_incref(inode);

	return s;
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
	inode_decref(s->inode);
	free(s->buffer);
	return 0;
}

/* static int rootfs_truncate_dir(void* this, intptr_t size); */

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


#define ROOTFS_BLKSIZE  (1<<12)
#define ROOTFS_MAX_BLOCKS (1<<8)
#define ROOTFS_MAX_FILE  (1<<20)

struct rootfs_file_inode
{
	ROOTFS_INODE_BASE;
	size_t size;
	/* Block list */
	void* blocks[ROOTFS_MAX_BLOCKS];
};


struct rootfs_file_stream
{
	intptr_t pos;
	Inode* inode;
};


static void* rootfs_open_file(Inode* inode, int flags)
{
	struct rootfs_file_stream* s = (struct rootfs_file_stream*) xmalloc(sizeof(struct rootfs_file_stream));
	s->pos = 0;
	s->inode = inode;
	inode_incref(inode);
	return s;
}


static int rootfs_read_file(void* this, char *buf, unsigned int size)
{
	struct rootfs_file_stream* s = this;
	struct rootfs_file_inode* rinode = (struct rootfs_file_inode*) s->inode->ino_ref.id;

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
	struct rootfs_file_inode* rinode = (struct rootfs_file_inode*) s->inode->ino_ref.id;

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
	inode_decref(s->inode);
	free(s);
	return 0;
}

static int rootfs_truncate_file(void* this, intptr_t size)
{
	struct rootfs_file_stream* s = this;
	struct rootfs_file_inode* rinode = (struct rootfs_file_inode*) s->inode->ino_ref.id;

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
		}
		fromblk ++;
	}

	return 0;
}


static intptr_t rootfs_seek_file(void* this, intptr_t offset, int whence)
{
	struct rootfs_file_stream* s = this;
	struct rootfs_file_inode* rinode = (struct rootfs_file_inode*) s->inode->ino_ref.id;

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



static Inode_id rootfs_allocate_file(Mount* mnt)
{
	struct rootfs_file_inode* rinode = (void*)xmalloc(sizeof(struct rootfs_file_inode));
	rinode->type = FSE_FILE;
	rinode->lnkcount = 0;

	rinode->size = 0;
	for(int i=0; i<ROOTFS_MAX_BLOCKS; i++)
		rinode->blocks[i] = NULL;

	return (Inode_id) rinode;
}

static int rootfs_release_file(Mount* mnt, void* rbinode)
{
	struct rootfs_file_inode* rinode = (struct rootfs_file_inode*) rbinode;

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

static void rootfs_fetch_file(Inode* inode)
{
#if 0  /* Nada to do! */	
	struct rootfs_file_inode* rinode = (struct rootfs_file_inode*) inode->ino_ref.id;
#endif
}

static void rootfs_flush_file(Inode* inode)
{
#if 0  /* Nada to do! */	
	struct rootfs_file_inode* rinode = (struct rootfs_file_inode*) inode->ino_ref.id;
#endif
}




/*------------------------------

	Generic API

 -------------------------------*/

static Inode_id rootfs_allocate_node(Mount* this, Fse_type type, Dev_t dev)
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


static int rootfs_release_node(Mount* this, Inode_id id)
{
	struct rootfs_base_inode* rbinode = 	(struct rootfs_base_inode*) id;
	switch(rbinode->type) {
		case FSE_FILE:
		return rootfs_release_file(this, rbinode);
		case FSE_DIR:
		return rootfs_release_dir(this, rbinode);
		default:
		return -1;
	};
}

static void* rootfs_open(Inode* inode, int flags)
{
	rootfs_fetch_base(inode);
	switch(inode->type) {
		case FSE_FILE:
			return rootfs_open_file(inode, flags);
		case FSE_DIR:
			return rootfs_open_dir(inode, flags);
		default:
			return NULL;
	};			
}

static void rootfs_fetch_inode(Inode* inode)
{
	rootfs_fetch_base(inode);
	switch(inode->type) {
		case FSE_FILE:
			rootfs_fetch_file(inode); break;
		case FSE_DIR:
			rootfs_fetch_dir(inode); break;
		default:
		return ;
	};		
}

static void rootfs_flush_inode(Inode* inode, int keep)
{
	/* Keep is ignored, we do not need to do anything to shed the fsdata */
	rootfs_flush_base(inode);
	switch(inode->type) {
		case FSE_FILE:
			rootfs_flush_file(inode); break;
		case FSE_DIR:
			rootfs_flush_dir(inode); break;
		default:
		return ;
	};		
}


static Mount* rootfs_mount(FSystem* this, Dev_t dev, Inode* mpoint, unsigned int pc, fs_param* pv)
{
	/* The only purpose of this file system is to create the mount point and inode
	   for the system root. Therefore, there is no mountpoint to speak of, nor is
	   there an associated device */

	if(mpoint==NULL && root_inode!=NULL) {
		set_errcode(ENOENT);
		return NULL;
	}
	if(dev!=NO_DEVICE) {
		set_errcode(ENXIO);
		return NULL;
	}

	Mount* mnt = mount_acquire();

	/* Init the fsys */
	mnt->fsys = this;

	/* Make the root node */
	mnt->root_dir = rootfs_allocate_dir(mnt);

	/* Set the fsdata field on the mount */
	mnt->fsdata = NULL;

	/* Get mount root */
	Inode* mount_root = get_inode( (Inode_ref){mnt, mnt->root_dir });
	inode_incref(mount_root);

	/* Take care of the mountpoint */
	mnt->mount_point = mpoint;
	if(mpoint != NULL) {

		/* Hold it for the lifetime of this mount */
		inode_incref(mpoint);

		/* Update its `mount` field, so that lookups are redirected */
		mpoint->dir.mount = mnt;

	} else {

		/* We are the root file system ! */
		root_inode = mount_root;

	}

	return mnt;
}

static int rootfs_unmount(Mount* mnt)
{
	/* Check if we have submounts */
	assert(is_rlist_empty(& mnt->submount_list));

	/* Detach from the filesystem */
	if(mnt->mount_point!=NULL) {
		mnt->mount_point->dir.mount = NULL;

	} else {
		/* We are the root */
		inode_decref(root_inode);
		root_inode = NULL;
	}

	/* See if we are busy */
	assert(mnt->refcount == 1);
	mount_decref(mnt);

	/* TODO: Recursively delete objects */
	return 0;
}


file_ops ROOT_FILE = {
	.Read = rootfs_read_file,
	.Write = rootfs_write_file,
	.Release = rootfs_close_file,
	.Truncate = rootfs_truncate_file,
	.Seek = rootfs_seek_file
};

file_ops ROOT_DIR = {
	.Read = rootfs_read_dir,
	.Release = rootfs_close_dir,
	.Seek = rootfs_seek_dir
};

FSystem ROOT_FSYS = {
	.name = "rootfs",
	.AllocateNode = rootfs_allocate_node,
	.ReleaseNode = rootfs_release_node,
	.FetchInode = rootfs_fetch_inode,
	.FlushInode = rootfs_flush_inode,
	.OpenInode = rootfs_open,
	.Mount = rootfs_mount,
	.Unmount = rootfs_unmount
};



REGISTER_FSYS(ROOT_FSYS)

