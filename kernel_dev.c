
#include <assert.h>
#include "kernel_dev.h"
#include "kernel_proc.h"
#include "kernel_fs.h"

/*******************************************

  Device table and device management routines

 ********************************************/


/*********************************************

  The device directory.

  This is a dictionary mapping string names to
  devices. This is the main way in which 
  user code interacts with devices, and is much
  more user-friendly than device numbers.

  The contents of the device directory are
  accessed through the 'devfs' file system.

 *******************************************/

/* A node in the device directory */
typedef struct device_advert
{
	char name[MAX_NAME_LENGTH];
	uint major, minor;
	rlnode dnode;
} dev_advert;

static int dev_advert_eq(rlnode* node, rlnode_key key)
{
	dev_advert* adv = node->obj;
	return strncmp(adv->name, key.str, MAX_NAME_LENGTH)==0;
}

static rlnode* dev_adv_alloc(const char* devname, uint major, uint minor)
{
	dev_advert* adv = xmalloc(sizeof(dev_advert));
	strncpy(adv->name, devname, MAX_NAME_LENGTH);
	adv->major = major;
	adv->minor = minor;
	rdict_node_init(& adv->dnode, adv, hash_nstring(adv->name, MAX_NAME_LENGTH));
	return &adv->dnode;	
}

static void dev_adv_free(rlnode* n)
{
	dev_advert* adv = n->obj;
	free(adv);
}

/* The directory rdict */
static rdict dev_directory;


void device_publish(const char* devname, uint major, uint minor)
{
	rdict_insert(& dev_directory, dev_adv_alloc(devname, major, minor));
}


void device_retract(const char* devname)
{
	rlnode* n = rdict_lookup(&dev_directory, hash_nstring(devname, MAX_NAME_LENGTH), devname, dev_advert_eq);
	if(n==NULL) return;
	rdict_remove(&dev_directory, n);
	dev_adv_free(n);
}


/***********************************

  The device table

***********************************/

DCB* device_table[MAX_DEV];

/* This is initialized at the time devices are */
TimerDuration boot_timestamp;

void initialize_devices()
{
	/* Init device directory */
	rdict_init(&dev_directory, 0);

	/* Take down the time of boot */
	boot_timestamp = bios_clock();

	/*
		Iterate through the device table and initialize each
		device type.
	 */	
	for(int d=0; d<MAX_DEV; d++) {
		if(device_table[d]) {
			void (*devinit)() = device_table[d]->Init;
			if(devinit) devinit();
		}
	}
}


void finalize_devices()
{
	/* Run finalizers */
	for(int d=0; d<MAX_DEV; d++) {
		if(device_table[d]) {
			void (*devfini)() = device_table[d]->Fini;
			if(devfini) devfini();
		}
	}

	/* Destroy the device directory */
	rdict_apply_removed(& dev_directory, dev_adv_free);
	rdict_destroy(&dev_directory);
}


void register_device(DCB* dcb)
{
	device_table[dcb->type] = dcb;
}

int device_open(uint major, uint minor, void** obj, file_ops** ops)
{
	/* Check limits */
	if(major >= MAX_DEV || device_table[major]==NULL) { set_errcode(ENXIO); return -1; }
	if(minor >= device_table[major]->devnum) { set_errcode(ENXIO); return -1; }

	/* Check that open succeeds */
	void* devstream = device_table[major]->Open(minor);
	if(devstream == NULL) { set_errcode(ENXIO); return -1; }

	/* Return stream */
	*obj = devstream;
	*ops = &device_table[major]->dev_fops;
	return 0;
}

uint device_no(uint major)
{
  return device_table[major]->devnum;
}


/****************************************************

  The device file system. This is a really minimal
  file system that depends on the device directory
  facility to show all devices as special nodes in
  a directory.

  This is typically mounted under /dev.

 ****************************************************/

extern FSystem DEVFS;

/* Mount a file system of this type. */
static int devfs_mount(MOUNT* mnt, Dev_t dev, unsigned int param_no, mount_param* param_vec)
{
    if(dev!=NO_DEVICE && dev!=0) return ENXIO;
    mnt->ptr = NULL;
    return 0;
}

/* Unmount a particular mount always succeeds */
static int devfs_umount(MOUNT mnt) { return 0; }


static int devfs_statfs(MOUNT mnt, struct StatFs* statfs)
{ 
	statfs->fs_dev = 0;
	statfs->fs_root = (inode_t) NO_DEVICE;

	statfs->fs_fsys = DEVFS.name;
	statfs->fs_blocks = 0;
	statfs->fs_bused = 0;
	statfs->fs_files = dev_directory.size +1;
	statfs->fs_fused = statfs->fs_files;

	return 0;
}


/* This is a read-only file system */
static int devfs_create(MOUNT _mnt, inode_t _dir, const pathcomp_t name, Fse_type type, inode_t* newino, void* data)
{ return EROFS; }
static int devfs_link(MOUNT _mnt, inode_t _dir, const pathcomp_t name, inode_t id)
{  return EROFS; }
static int devfs_unlink(MOUNT _mnt, inode_t _dir, const pathcomp_t name)
{ return EROFS; }


/* These always succeed */
static int devfs_pin(MOUNT _mnt, inode_t _ino) { return 0; }
static int devfs_unpin(MOUNT _mnt, inode_t _ino) { return 0; }
static int devfs_flush(MOUNT _mnt, inode_t _ino) { return 0; }

static int devfs_listdir(MOUNT _mnt, inode_t _ino, struct dir_list* dlist)
{
	if(_ino != (inode_t) NO_DEVICE) return ENOTDIR;

	void dir_list_add_dev(rlnode* n) {
		dev_advert* adv = n->obj;
		dir_list_add(dlist, adv->name);
	}

	rdict_apply(& dev_directory, dir_list_add_dev);
	return 0;
}


static int devfs_open(MOUNT _mnt, inode_t _ino, int flags, void** obj, file_ops** ops)
{
	if(_ino == (inode_t) NO_DEVICE) return EISDIR;
	Dev_t dev = (Dev_t) _ino;
	if(device_open(DEV_MAJOR(dev), DEV_MINOR(dev), obj, ops)==0)
		return 0;
	else
		return ENODEV;
}

static int devfs_truncate(MOUNT _mnt, inode_t _ino, intptr_t length)
{
	if(_ino != (inode_t) NO_DEVICE) return 0;
	else return EISDIR;
}

static int devfs_fetch(MOUNT _mnt, inode_t _dir, const pathcomp_t name, inode_t* id, int creat)
{
	if(_dir != (inode_t) NO_DEVICE) return ENOTDIR;

	/* First treat the standard directory entries */
	if(strcmp(name,".")==0 || strcmp(name,"..")==0) { *id = _dir; return 0; }

	/* Look up into dev_directory */
	rlnode* n = rdict_lookup(& dev_directory, hash_nstring(name, MAX_NAME_LENGTH), name, dev_advert_eq);
	if(n==NULL) return creat ? EROFS : ENOENT;

	/* Found device */
	dev_advert* adv = n->obj;
	*id = (inode_t) device_id(adv->major, adv->minor);

	return 0;
}


static int devfs_status(MOUNT _mnt, inode_t _ino, struct Stat* st, pathcomp_t name)
{
	if(_ino == (inode_t) NO_DEVICE) {
		if(name != NULL) name[0]='\0';
		if(st == NULL) return 0;

		st->st_type = FSE_DIR;
		st->st_nlink = 2;
		st->st_rdev = NO_DEVICE;
		st->st_size = dev_directory.size;
	} else {
		if(name != NULL) return ENOTDIR;
		if(st == NULL) return 0;

		st->st_type = FSE_DEV;
		st->st_nlink = 1;
		st->st_rdev = (Dev_t) _ino;
		st->st_size = 0;
	}

	st->st_dev = 0;
	st->st_ino = _ino;
	st->st_blksize = 1024;
	st->st_blocks = 0;
	st->st_change = st->st_modify = st->st_access = boot_timestamp;
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
	.ListDir = devfs_listdir,
	.Truncate = devfs_truncate,
	.Fetch = devfs_fetch,
	.Link = devfs_link,
	.Unlink = devfs_unlink,
	.Status = devfs_status,
	.StatFs = devfs_statfs
};

REGISTER_FSYS(DEVFS);

