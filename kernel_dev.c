
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


static rdict dev_directory;


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



int devfs_open(MOUNT _mnt, inode_t _ino, int flags, void** obj, file_ops** ops)
{
	set_errcode(ENOSYS); return -1;
}


int devfs_fetch(MOUNT _mnt, inode_t _dir, const pathcomp_t name, inode_t* id, int creat)
{
	return 0;
}


int devfs_status(MOUNT _mnt, inode_t _ino, struct Stat* status, pathcomp_t name, int which)
{

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

