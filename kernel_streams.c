
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
  if(! is_rlist_empty(& FCB_freelist)) {
    FCB* fcb = rlist_pop_front(& FCB_freelist)->fcb;
    fcb->refcount = 0;
    return fcb;
  }
  else
    return NULL;
}

void release_FCB(FCB* fcb)
{
  rlist_push_back(& FCB_freelist, & fcb->freelist_node);
}


void FCB_incref(FCB* fcb)
{
  assert(fcb);
  fcb->refcount++;
}

int FCB_decref(FCB* fcb)
{
  assert(fcb);
  fcb->refcount --;
  if(fcb->refcount==0) {
    int retval = fcb->streamfunc->Close(fcb->streamobj);
    release_FCB(fcb);
    return retval;
  }
  else
    return 0;
}



int FCB_reserve(size_t num, Fid_t *fid, FCB** fcb)
{
    PCB* cur = CURPROC;
    size_t f=0;
    uint i;

    /* Find distinct fids */
    for(i=0; i<num; i++) {
	while(f<MAX_FILEID && cur->FIDT[f]!=NULL)
	    f++;
	if(f==MAX_FILEID) break;
	fid[i] = f; f++;
    }
    if(i<num) return 0;
    /* Allocate FCBs */
    for(i=0;i<num;i++)
	if((fcb[i] = acquire_FCB()) == NULL)
	    break;
    if(i<num) {
	/* Roll back */
	while(i>0) {
	    release_FCB(fcb[i-1]);
	    i--;
	}
	return 0;
    }
    /* Found all */
    for(i=0;i<num;i++) {
	cur->FIDT[fid[i]]=fcb[i];
	FCB_incref(fcb[i]);
    }
    return 1;
}



void FCB_unreserve(size_t num, Fid_t *fid, FCB** fcb)
{
    PCB* cur = CURPROC;
    for(size_t i=0; i<num ; i++) {
	assert(cur->FIDT[fid[i]]==fcb[i]);
	cur->FIDT[fid[i]] = NULL;
	release_FCB(fcb[i]);
    }
}






/*
 *
 *   I/O routines
 *
 */


FCB* get_fcb(Fid_t fid)
{
  if(fid < 0 || fid >= MAX_FILEID) return NULL;

  return CURPROC->FIDT[fid];
}


int sys_Read(Fid_t fd, char *buf, unsigned int size)
{
  int retcode = -1;
  int (*devread)(void*,char*,uint);
  void* sobj;

  
  /* Get the fields from the stream */
  FCB* fcb = get_fcb(fd);

  if(fcb) {
    sobj = fcb->streamobj;
    devread = fcb->streamfunc->Read;

    /* make sure that the stream will not be closed (by another thread) 
       while we are using it! */
    FCB_incref(fcb);
  
    if(devread)
      retcode = devread(sobj, buf, size);

    /* Need to decrease the reference to FCB */
    FCB_decref(fcb);
  }
  
  /* We must not go into non-preemptive domain with kernel_mutex locked */


  return retcode;
}


int sys_Write(Fid_t fd, const char *buf, unsigned int size)
{
  int retcode = -1;
  int (*devwrite)(void*, const char*, uint) = NULL;
  void* sobj = NULL;

  
  /* Get the fields from the stream */
  FCB* fcb = get_fcb(fd);

  if(fcb) {

    sobj = fcb->streamobj;
    devwrite = fcb->streamfunc->Write;

    /* make sure that the stream will not be closed (by another thread) 
       while we are using it! */
    FCB_incref(fcb);
  

    if(devwrite)
      retcode = devwrite(sobj, buf, size);

    /* Need to decrease the reference to FCB */
    FCB_decref(fcb);

  }


  return retcode;
}


int sys_Close(int fd)
{
  int retcode = (fd>=0 && fd<MAX_FILEID) ? 0 : -1;  /* Closing a closed fd is legal! */

  FCB* fcb = get_fcb(fd);

  if(fcb) {
    CURPROC->FIDT[fd] = NULL;
    retcode = FCB_decref(fcb);    
  }

  return retcode;
}


/*
  Copy file descriptor oldfd into file descriptor newfd.

  This call returns 0 on success and -1 on failure.
  Possible reasons for failure:
  - Either oldfd or newfd is invalid.
 */
int sys_Dup2(int oldfd, int newfd)
{
  int retcode=0;
  if(oldfd<0 || newfd<0 || oldfd>=MAX_FILEID || newfd>=MAX_FILEID)
    return -1;

  FCB* old = get_fcb(oldfd);
  FCB* new = get_fcb(newfd);

  if(old==NULL) {
    retcode = -1;
  }
  else if(old!=new) {
    if(new)
      FCB_decref(new);
    FCB_incref(old);
    CURPROC->FIDT[newfd] = old;
  }

  return retcode;
}



unsigned int sys_GetTerminalDevices()
{
  return device_no(DEV_SERIAL);
}


/**
  Open a stream for the given device.
  */
Fid_t open_stream(Device_type major, unsigned int minor)
{
  Fid_t fid;
  FCB* fcb;


  if(! FCB_reserve(1, &fid, &fcb))
      goto finerr;
  
  if(device_open(major, minor, & fcb->streamobj, &fcb->streamfunc)) {
      FCB_unreserve(1, &fid, &fcb);
      goto finerr;
  }
  
  goto finok;
finerr:
  fid = NOFILE;
finok:
  return fid;
}


int sys_OpenNull()
{
  return open_stream(DEV_NULL, 0);
}


Fid_t sys_OpenTerminal(unsigned int termno)
{
  return open_stream(DEV_SERIAL, termno);
}

