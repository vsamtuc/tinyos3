
#include <assert.h>
#include "kernel_dev.h"
#include "kernel_proc.h"
#include "kernel_fs.h"

/*******************************************

  Device table and device management routines

 ********************************************/


/***********************************

  The device table

***********************************/

DCB* device_table[DEV_MAX];


typedef struct device_advert
{
	char* name;
	uint major, minor;
	rlnode dnode;
} dev_advert;

static int dev_advert_eq(rlnode* node, rlnode_key key)
{
	dev_advert* adv = node->obj;
	return strcmp(adv->name, key.str)==0;
}

static rdict dev_directory;


void device_publish(const char* devname, uint major, uint minor)
{
	dev_advert* adv = xmalloc(sizeof(dev_advert));
	adv->name = strdup(devname);
	adv->major = major;
	adv->minor = minor;
	rdict_node_init(& adv->dnode, adv, hash_string(devname));

	rdict_insert(& dev_directory, & adv->dnode);
}

void device_retract(const char* devname)
{
	rlnode* n = rdict_lookup(&dev_directory, hash_string(devname), devname, dev_advert_eq);
	if(n==NULL) return;
	rdict_remove(&dev_directory, n);
	dev_advert* adv = n->obj;
	free(adv->name);
	free(adv);
}


void initialize_devices()
{
	rdict_init(&dev_directory, 0);

	/*
		Iterate through the device table and initialize each
		device type.
	 */	
	for(int d=0; d<DEV_MAX; d++) {
		if(device_table[d]) {
			void (*devinit)() = device_table[d]->Init;
			if(devinit) devinit();
		}
		else fprintf(stderr, "Device type %d is not initialized\n",d);
	}
}


void finalize_devices()
{

	for(int d=0; d<DEV_MAX; d++) {
		if(device_table[d]) {
			void (*devfini)() = device_table[d]->Fini;
			if(devfini) devfini();
		}
		else fprintf(stderr, "Device type %d is not finalized\n",d);
	}


	rdict_destroy(&dev_directory);
}


void register_device(DCB* dcb)
{
	device_table[dcb->type] = dcb;
}

int device_open(uint major, uint minor, void** obj, file_ops** ops)
{
	assert(major < DEV_MAX);  
	if(minor >= device_table[major]->devnum) {
		set_errcode(ENXIO);				
		return -1;
	}
	*obj = device_table[major]->Open(minor);
	*ops = &device_table[major]->dev_fops;
	return 0;
}

uint device_no(uint major)
{
  return device_table[major]->devnum;
}


/***********************************

  The device file system

***********************************/

extern FSystem DEVFS;

/* Mount a file system of this type. */
int devfs_mount(MOUNT* mnt, Dev_t dev, unsigned int param_no, mount_param* param_vec)
{
    if(dev!=NO_DEVICE) return ENXIO;
    mnt->ptr = NULL;
    return 0;
}

/* Unmount a particular mount */
int devfs_umount(MOUNT mnt) { return 0; }


int devfs_statfs(MOUNT mnt, struct StatFs* statfs)
{ 
	statfs->fs_dev = NO_DEVICE;
	statfs->fs_root = (inode_t) NO_DEVICE;

	statfs->fs_fsys = DEVFS.name;
	statfs->fs_blocks = 0;
	statfs->fs_bused = 0;
	statfs->fs_files = 1;
	statfs->fs_fused = 1;

	return 0;
}


/* This is a read-only file system */
int devfs_create(MOUNT _mnt, inode_t _dir, const pathcomp_t name, Fse_type type, inode_t* newino, void* data)
{ return EROFS; }
int devfs_link(MOUNT _mnt, inode_t _dir, const pathcomp_t name, inode_t id)
{  return EROFS; }
int devfs_unlink(MOUNT _mnt, inode_t _dir, const pathcomp_t name)
{ return EROFS; }


/* These always succeed */
int devfs_pin(MOUNT _mnt, inode_t _ino) { return 0; }
int devfs_unpin(MOUNT _mnt, inode_t _ino) { return 0; }
int devfs_flush(MOUNT _mnt, inode_t _ino) { return 0; }


static int __release_devfs_dir_list(void* this)
{
	dir_list* dlist = this;
	int ret = dir_list_close(dlist);
	free(this);
	return ret;
}	


static file_ops devfs_dir_list_ops = {
	.Read = (void*) dir_list_read,
	.Seek = (void*) dir_list_seek,
	.Release = __release_devfs_dir_list
};

int devfs_open(MOUNT _mnt, inode_t _ino, int flags, void** obj, file_ops** ops)
{
	if(_ino == (inode_t) NO_DEVICE) {
		/* Return a stream listing the contents of dev_directory */
		if(flags != OPEN_RDONLY) return EISDIR;

		dir_list* dlist = xmalloc(sizeof(dir_list));
		dir_list_create(dlist);

		void dir_list_add_dev(rlnode* n) {
			dev_advert* adv = n->obj;
			dir_list_add(dlist, adv->name);
		}

		rdict_apply(& dev_directory, dir_list_add_dev);

		dir_list_open(dlist);
		*obj = dlist;
		*ops = & devfs_dir_list_ops;

		return 0;
	}

	/* The case of a Dev_t */
	Dev_t dev = (Dev_t) _ino;
	if(device_open(DEV_MAJOR(dev), DEV_MINOR(dev), obj, ops)==0)
		return 0;
	else
		return ENODEV;
}


int devfs_fetch(MOUNT _mnt, inode_t _dir, const pathcomp_t name, inode_t* id, int creat)
{
	if(_dir != (inode_t) NO_DEVICE) return ENOTDIR;

	/* First treat the standard directory entries */
	if(strcmp(name,".")==0 || strcmp(name,"..")==0) { *id = _dir; return 0; }

	/* Look up into dev_directory */
	rlnode* n = rdict_lookup(& dev_directory, hash_string(name), name, dev_advert_eq);
	if(n==NULL) return ENOENT;

	/* Found device */
	dev_advert* adv = n->obj;
	*id = (inode_t) device_id(adv->major, adv->minor);

	return 0;
}


int devfs_status(MOUNT _mnt, inode_t _ino, struct Stat* st, pathcomp_t name, int which)
{
	st->st_dev = NO_DEVICE;
	st->st_ino = _ino;

	if(_ino == (inode_t) NO_DEVICE) {
		st->st_type = FSE_DIR;
		st->st_nlink = 2;
		st->st_rdev = NO_DEVICE;
		st->st_size = dev_directory.size;
		st->st_blksize = 1024;
		st->st_blocks = 0;
		if(name != NULL) name[0]='\0';
		return 0;
	}

	st->st_type = FSE_DEV;
	st->st_nlink = 1;
	st->st_rdev = (Dev_t) _ino;
	st->st_size = 0;
	st->st_blksize = 1024;
	st->st_blocks = 0;
	
	return 0;
}


FSystem DEVFS = {
	.name = "devfs",
	.Mount = devfs_mount,
	.Unmount = devfs_umount,
	.Create = devfs_create,
	.Pin = devfs_pin,
	.Unpin = devfs_unpin,
	.Flush = devfs_flush,
	.Open = devfs_open,
	.Fetch = devfs_fetch,
	.Link = devfs_link,
	.Unlink = devfs_unlink,
	.Status = devfs_status,
	.StatFs = devfs_statfs
};

REGISTER_FSYS(DEVFS);

