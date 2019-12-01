
#include <assert.h>
#include "kernel_dev.h"
#include "kernel_proc.h"

/*******************************************

  Device table and device management routines

 ********************************************/


/***********************************

  The device table

***********************************/

DCB* device_table[DEV_MAX];


void initialize_devices()
{
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


