
#include "kernel_dev.h"


/* ===================================

  The null device driver

  ====================================*/


/* There is just one null device stream object! */
static struct {} nulldev;

/*
	The null device always returns zeros
 */
int nulldev_read(void* dev, char *buf, unsigned int size)
{
	/*
		The null device always returns zeros
	 */
	memset(buf, 0, size);
	return size;
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
void* nulldev_open(uint minor)
{
	return &nulldev;
}

void nulldev_init()
{
	device_publish("null", DEV_NULL, 0);
}

/* The DCB for the null device */
static DCB nulldev_dcb = {
	.type = DEV_NULL,
	.devnum = 1,
	.Init = nulldev_init,
	.Open = nulldev_open,
	.dev_fops = {
		.Read = nulldev_read,
		.Write = nulldev_write,
		.Release = nulldev_close
	}
};


REGISTER_DRIVER(nulldev_dcb)

