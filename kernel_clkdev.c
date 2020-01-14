
#include "kernel_dev.h"
#include "kernel_proc.h"
#include "bios.h"

/*
	Clock device
 */

struct {} clockdev;

int clock_read(void* dev, char* buffer, unsigned int size)
{
	if( size!=sizeof(TimerDuration) || /* check size */
		( ((uintptr_t)((void*)buffer)) % _Alignof(TimerDuration))
		) 
	{ set_errcode(EINVAL); return -1; }

	* ((TimerDuration*)buffer) = bios_clock();
	return size;
}

int clock_close(void* dev) { return 0; }

void* clock_open(devnum_t minor)
{
	return &clockdev;
}


void clock_init(void)
{
	device_publish("clock", DEV_CLOCK, 0);
}

static DCB clock_dcb = {
	.type = DEV_CLOCK,
	.devnum = 1,
	.Open = clock_open,
	.Init = clock_init,
	.dev_fops = {
		.Read = clock_read,
		.Release = clock_close
	}
};

REGISTER_DRIVER(clock_dcb)
																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																																									