
#include "util.h"
#include "tinyos.h"
#include "kernel_cc.h"
#include "kernel_streams.h"
#include "kernel_sched.h"
#include "kernel_proc.h"

#define MAX_FILES MAX_PROC

FCB FT[MAX_FILES];
rlnode FCB_freelist;


void initialize_files()
{
  rlnode_init(&FCB_freelist,NULL);
  for(int i=0;i<MAX_FILES;i++) {

    FT[i].refcount = 0;
    rlnode_init(& FT[i].freelist_node, &FT[i]);
    rlist_push_back(&FCB_freelist, & FT[i].freelist_node);
  }
}


FCB* acquire_FCB()
{
  return rlist_pop_front(& FCB_freelist)->fcb;
}

void release_FCB(FCB* fcb)
{
  rlist_push_back(& FCB_freelist, & fcb->freelist_node);
}


void FCB_incref(FCB* fcb)
{
  fcb->refcount++;
}

int FCB_decref(FCB* fcb)
{
  fcb->refcount --;
  if(fcb->refcount==0) {
    int retval = fcb->streamfunc->Close(fcb->streamobj);
    release_FCB(fcb);
    return retval;
  }
  else
    return 0;
}



/*
 *
 *   I/O routines
 *
 */


static FCB* get_fcb(Fid_t fid)
{
  if(fid < 0 || fid >= MAX_FILEID) return NULL;

  return CURPROC->FIDT[fid];
}


int Read(Fid_t fd, char *buf, unsigned int size)
{
  int retcode = -1;
  int (*devread)(void*,char*,uint);
  void* sobj;

  Mutex_Lock(&kernel_mutex);
  
  /* Get the fields from the stream */
  FCB* fcb = get_fcb(fd);

  if(fcb) {
    sobj = fcb->streamobj;
    devread = fcb->streamfunc->Read;

    /* make sure that the stream will not be closed (by another thread) 
       while we are using it! */
    FCB_incref(fcb);
  
    /* We must not go into non-preemptive domain with kernel_mutex locked */
    Mutex_Unlock(&kernel_mutex);  

    if(devread)
      retcode = devread(sobj, buf, size);

    /* Need to decrease the reference to FCB */
    Mutex_Lock(& kernel_mutex);
    FCB_decref(fcb);

  }
  
  Mutex_Unlock(&kernel_mutex);  

  /* We must not go into non-preemptive domain with kernel_mutex locked */


  return retcode;
}


int Write(Fid_t fd, const char *buf, unsigned int size)
{
  int retcode = -1;
  int (*devwrite)(void*, const char*, uint) = NULL;
  void* sobj = NULL;

  Mutex_Lock(&kernel_mutex);
  
  /* Get the fields from the stream */
  FCB* fcb = get_fcb(fd);

  if(fcb) {

    sobj = fcb->streamobj;
    devwrite = fcb->streamfunc->Write;

    /* make sure that the stream will not be closed (by another thread) 
       while we are using it! */
    FCB_incref(fcb);
  
    /* We must not go into non-preemptive domain with kernel_mutex locked */
    Mutex_Unlock(&kernel_mutex);  

    if(devwrite)
      retcode = devwrite(sobj, buf, size);

    /* Need to decrease the reference to FCB */
    Mutex_Lock(& kernel_mutex);
    FCB_decref(fcb);

  }

  Mutex_Unlock(& kernel_mutex);

  return retcode;
}


int Close(int fd)
{
  int retcode = (fd>=0 && fd<MAX_FILEID) ? 0 : -1;  /* Closing a closed fd is legal! */
  Mutex_Lock(&kernel_mutex);

  FCB* fcb = get_fcb(fd);

  if(fcb) {
    CURPROC->FIDT[fd] = NULL;
    retcode = FCB_decref(fcb);    
  }

  Mutex_Unlock(&kernel_mutex);  
  return retcode;
}


/*
  Copy file descriptor oldfd into file descriptor newfd.

  This call returns 0 on success and -1 on failure.
  Possible reasons for failure:
  - Either oldfd or newfd is invalid.
 */
int Dup2(int oldfd, int newfd)
{
  int retcode=0;
  if(oldfd<0 || newfd<0 || oldfd>=MAX_FILEID || newfd>=MAX_FILEID)
    return -1;
  Mutex_Lock(&kernel_mutex);

  FCB* old = get_fcb(oldfd);
  FCB* new = get_fcb(newfd);

  if(old==NULL) {
    retcode = -1;
  }
  else if(old!=new) {
    if(new) FCB_decref(new);
    CURPROC->FIDT[newfd] = old;
  }

  Mutex_Unlock(&kernel_mutex);  
  return retcode;
}



unsigned int GetTerminalDevices()
{
  return device_no(DEV_SERIAL);
}


/**
  Open a stream for the given device.
  */
Fid_t open_stream(Device_type major, unsigned int minor)
{
  Fid_t fid;
  Mutex_Lock(&kernel_mutex);

  /* Try to get a free fid in FIDT */
  for(fid=0; fid<MAX_FILEID; fid++)
    if(CURPROC->FIDT[fid]==NULL) break;

  /* Report an error */
  if(fid==MAX_FILEID) 
    goto finerr;

  /* See if we can acquire a FCB */
  FCB* fcb = acquire_FCB();
  if(fcb==NULL) goto finerr;

  if(device_open(major, minor, & fcb->streamobj, &fcb->streamfunc)) 
    goto finerr;

  CURPROC->FIDT[fid] = fcb;

  goto finok;
finerr:
  fid = NOFILE;
finok:
  Mutex_Unlock(&kernel_mutex);
  return fid;
}


int OpenNull()
{
  return open_stream(DEV_NULL, 0);
}


Fid_t OpenTerminal(unsigned int termno)
{
  return open_stream(DEV_SERIAL, termno);
}

