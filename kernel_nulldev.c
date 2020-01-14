
#include "kernel_dev.h"


/* ===================================

  The null device driver

  ====================================*/


/* There are just 2 device stream object, /dev/null and /dev/zero */
static struct nulldev_cb { uint minor; } nulldev[2] = {{0}, {1}};

/*
	The null device always returns zeros
 */
int nulldev_read(void* dev, char *buf, unsigned int size)
{
	uint minor = *(uint*)dev;
	/*
		The null device always returns zeros
	 */
	if(minor == 1) {
		memset(buf, 0, size);  /* /dev/zero returns zeros */
		return size;
	} 
	/* /dev/null is always empty */
	return 0;
}


/*
The null device always returns zeros
 */
int nulldev_write(void* dev, const char* buf, unsigned int size)
{
	/* 
		Here, we do not copy anything, therefore simply return
		a value equal to the argument.
	 */
	return size;
}

/*
	Releasing always succeeds
 */
int nulldev_close(void* dev) 
{
	return 0;
}

/*
	Open just returns `nulldev`
 */
void* nulldev_open(devnum_t minor)
{
	return &nulldev[minor];
}

void nulldev_init()
{
	device_publish("null", DEV_NULL, 0);
	device_publish("zero", DEV_NULL, 1);
}

/* The DCB for the null device */
static DCB nulldev_dcb = {
	.type = DEV_NULL,
	.devnum = 2,
	.Init = nulldev_init,
	.Open = nulldev_open,
	.dev_fops = {
		.Read = nulldev_read,
		.Write = nulldev_write,
		.Release = nulldev_close
	}
};


REGISTER_DRIVER(nulldev_dcb)

