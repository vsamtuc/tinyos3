
#include <assert.h>
#include <stdlib.h>

#include "kernel_cc.h"
#include "kernel_dev.h"
#include "kernel_sched.h"
#include "kernel_streams.h"
#include "kernel_proc.h"

/*************************************

  Devices and device drivers

 *************************************/

DCB DT[MAX_TERMINALS];


/* ===================================

  The null device driver

  ====================================*/


int nulldev_read(void* dev, char *buf, unsigned int size)
{
  memset(buf, 0, size);
  return size;
}

int nulldev_write(void* dev, const char* buf, unsigned int size)
{
    /* Here, we do not copy anything, therefore simply return
       a value equal to the argument.
     */
    return size;
}


int nulldev_close(void* dev) 
{
  return 0;
}

void* nulldev_open(uint minor)
{
  return NULL;
}

static file_ops nulldev_fops = {
  .Open = nulldev_open,
  .Read = nulldev_read,
  .Write = nulldev_write,
  .Close = nulldev_close
};


/*============================================

  The serial device driver

 ============================================*/


/* forward */
void serial_rx_handler();
void serial_tx_handler();

typedef struct serial_device_control_block {
  uint devno;
  Mutex spinlock;
  CondVar rx_ready;
} serial_dcb_t;

serial_dcb_t serial_dcb[MAX_TERMINALS];



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
    serial_dcb_t* dcb = &serial_dcb[i];
    Cond_Broadcast(&dcb->rx_ready);
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
      kernel_wait(&dcb->rx_ready, SCHED_IO);
    }
    else
      break;
  }

  preempt_on;           /* Restart preemption */

  return count;
}


/*
  A polling driver for serial writes
  */

/* Interrupt driver */
void serial_tx_handler()
{
  /* There is nothing to do */
}

/* 
  Write call 
  This is currently a polling driver.
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
      yield(SCHED_IO);
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
  return & serial_dcb[term];  
}



file_ops serial_fops = {
  .Open = serial_open,
  .Read = serial_read,
  .Write = serial_write,
  .Close = serial_close
};


/*============================================

  The terminal device driver

 ============================================*/

typedef struct terminal_device_stream {
  void* serial_stream;
  file_ops* serial_ops;
  int pending_eof;
} terminal_stream_t;

static void* terminal_open(uint term)
{
  terminal_stream_t* stream = xmalloc(sizeof(*stream));
  stream->serial_stream = serial_open(term);
  stream->serial_ops = &serial_fops;
  stream->pending_eof = 0;
  return stream;
}

static int terminal_read(void* dev, char *buf, unsigned int size)
{
  terminal_stream_t* stream = (terminal_stream_t*)dev;

  if(stream->pending_eof) {
    stream->pending_eof = 0;
    return 0;
  }

  unsigned int count = 0;
  while(count < size) {
    char ch;
    int rc = stream->serial_ops->Read(stream->serial_stream, &ch, 1);

    if(rc < 0)
      return rc;

    if(rc == 0) {
      if(count == 0)
        return 0;
      break;
    }

    if(ch == 4) {
      if(count == 0) {
        return 0;
      }
      stream->pending_eof = 1;
      break;
    }

    buf[count++] = ch;
  }

  return count;
}

static int terminal_write(void* dev, const char* buf, unsigned int size)
{
  terminal_stream_t* stream = (terminal_stream_t*)dev;
  return stream->serial_ops->Write(stream->serial_stream, buf, size);
}

static int terminal_close(void* dev)
{
  terminal_stream_t* stream = (terminal_stream_t*)dev;
  stream->serial_ops->Close(stream->serial_stream);
  free(stream);
  return 0;
}

static file_ops terminal_fops = {
  .Open = terminal_open,
  .Read = terminal_read,
  .Write = terminal_write,
  .Close = terminal_close
};


/***********************************

  The device table

***********************************/

DCB devtable[DEV_MAX];



void initialize_devices()
{

  devtable[DEV_NULL].type = DEV_NULL;
  devtable[DEV_NULL].devnum = 1;
  devtable[DEV_NULL].dev_fops = nulldev_fops;

  devtable[DEV_SERIAL].type = DEV_SERIAL;
  devtable[DEV_SERIAL].devnum = bios_serial_ports();
  devtable[DEV_SERIAL].dev_fops = serial_fops;

  devtable[DEV_TERMINAL].type = DEV_TERMINAL;
  devtable[DEV_TERMINAL].devnum = bios_serial_ports();
  devtable[DEV_TERMINAL].dev_fops = terminal_fops;

  /* Initialize the serial devices */
  for(int i=0; i<bios_serial_ports(); i++) {
    serial_dcb[i].devno = i;
    serial_dcb[i].rx_ready = COND_INIT;
    serial_dcb[i].spinlock = MUTEX_INIT;
  }

  cpu_interrupt_handler(SERIAL_RX_READY, serial_rx_handler);
  cpu_interrupt_handler(SERIAL_TX_READY, serial_tx_handler);
}


int device_open(Device_type major, uint minor, void** obj, file_ops** ops)
{
  assert(major < DEV_MAX);  
  if(minor >= devtable[major].devnum)
    return -1;
  *obj = devtable[major].dev_fops.Open(minor);
  *ops = &devtable[major].dev_fops;
  return 0;
}

uint device_no(Device_type major)
{
  return devtable[major].devnum;
}


