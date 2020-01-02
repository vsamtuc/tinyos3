#include <assert.h>
#include "kernel_cc.h"
#include "kernel_dev.h"
#include "kernel_sched.h"
#include "kernel_streams.h"
#include "kernel_proc.h"



/*============================================

  The serial device driver

 ============================================*/


/* forward */
void serial_rx_handler();
void serial_tx_handler();

/* Two wait channels */
static wait_channel wchan_serial_read = { SCHED_IO, "serial_read" };
static wait_channel wchan_serial_write = { SCHED_POLL, "serial_write" };

/*
	A serial device stream object type. There is one object for each serial device.
 */
typedef struct serial_device_control_block 
{
	uint devno;
	Mutex spinlock;
	wait_queue rx_ready;
	wait_queue tx_ready;
} serial_dcb_t;

/* The serial devices */
serial_dcb_t serial_dev[MAX_TERMINALS];


/*
  Interrupt-driven driver for serial-device reads.
 */

void serial_rx_handler()
{
	int pre = preempt_off;

	/* 
		We do not know which terminal is
		ready, so we must signal them all !
	*/
	for(int i=0;i<bios_serial_ports();i++) {
		serial_dcb_t* dev = &serial_dev[i];
		wqueue_broadcast(&dev->rx_ready);
	}
	if(pre) preempt_on;
}

/*
  Read from the device, sleeping if needed.
 */
int serial_read(void* dev, char *buf, unsigned int size)
{
	serial_dcb_t* dcb = (serial_dcb_t*)dev;

	preempt_off;            /* Stop preemption */

	uint count =  0;

	while(count<size) {
		int valid = bios_read_serial(dcb->devno, &buf[count]);

		if (valid) {
			count++;
		}
		else if(count==0) {
			kernel_wait(&dcb->rx_ready);
		}
		else
			break;
	}

	preempt_on;	/* Restart preemption */

	return count;
}


/*
  A polling driver for serial writes
  */

/* Interrupt driver */
void serial_tx_handler()
{
	int pre = preempt_off;

	/* 
		We do not know which terminal is
		ready, so we must signal them all !
	*/
	for(int i=0;i<bios_serial_ports();i++) {
		serial_dcb_t* dcb = &serial_dev[i];
		wqueue_broadcast(&dcb->tx_ready);
	}
	if(pre) preempt_on;
}

/* 
  Write to a serial device
*/
int serial_write(void* dev, const char* buf, unsigned int size)
{
	serial_dcb_t* dcb = (serial_dcb_t*)dev;

	unsigned int count = 0;
	while(count < size) {
		int success = bios_write_serial(dcb->devno, buf[count] );

		if(success) {
			count++;
		} 
		else if(count==0)
		{
			kernel_wait(& dcb->tx_ready);
		}
		else
			break;
	}

	return count;  
}


int serial_close(void* dev) 
{
	return 0;
}


void* serial_open(uint term)
{
	assert(term<bios_serial_ports());
	return & serial_dev[term];
}

static void serial_devinit();

static DCB serial_dcb = {
	.type = DEV_SERIAL,
	.Init = serial_devinit,
	.Open = serial_open,
	.dev_fops = {
		.Read = serial_read,
		.Write = serial_write,
		.Release = serial_close
	}
};


static void serial_devinit()
{
	device_table[DEV_SERIAL] = &serial_dcb;
	serial_dcb.devnum = bios_serial_ports();

	/* Initialize the serial devices */
	for(int i=0; i<bios_serial_ports(); i++) {
		char buffer[20];
		snprintf(buffer, 20, "serial%d", i+1);
		device_publish(buffer, DEV_SERIAL, i);

		serial_dev[i].devno = i;
		wqueue_init(& serial_dev[i].rx_ready, &wchan_serial_read);
		wqueue_init(& serial_dev[i].tx_ready, &wchan_serial_write);
		serial_dev[i].spinlock = MUTEX_INIT;
	}

	cpu_interrupt_handler(SERIAL_RX_READY, serial_rx_handler);
	cpu_interrupt_handler(SERIAL_TX_READY, serial_tx_handler);	
}


REGISTER_DRIVER(serial_dcb)

