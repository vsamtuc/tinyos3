
#include "util.h"
#include "tinyos.h"
#include "kernel_cc.h"
#include "kernel_streams.h"
#include "kernel_sched.h"
#include "kernel_proc.h"

#define MAX_FILES MAX_PROC

FCB FT[MAX_FILES];
FCB* FCB_freelist;


FCB* acquire_FCB()
{
	FCB* fcb = FCB_freelist;

	if(fcb) {
		FCB_freelist = fcb->streamobj;
		fcb->refcount = 0;
		fcb->streamobj = NULL;
		fcb->streamfunc = NULL;
	}
	return fcb;
}

void release_FCB(FCB* fcb)
{
	fcb->streamobj = FCB_freelist;
	FCB_freelist = fcb;
}

void initialize_files()
{
	FCB_freelist = NULL;
	for(int i=0;i<MAX_FILES;i++) {

		FT[i].refcount = 0;
		release_FCB(&FT[i]);	
	}
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
    int retval = fcb->streamfunc->Release(fcb->streamobj);
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
		if(f==MAX_FILEID) 
			break;
		fid[i] = f; f++;
    }

    if(i<num) { set_errcode(EMFILE); return 0; }

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
		set_errcode(ENFILE);
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

intptr_t sys_Seek(Fid_t fd, intptr_t offset, int whence)
{
	/* Get the fields from the stream */
	FCB* fcb = get_fcb(fd);
	if(!fcb) { set_errcode(EBADF); return -1; }

	intptr_t (*seekfunc)(void*, intptr_t, int) = fcb->streamfunc->Seek;
	if(seekfunc) {
		return fcb->streamfunc->Seek(fcb->streamobj, offset, whence);
	} else {
		set_errcode(ESPIPE);
		return -1;
	}
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
  
    if(devread) {
      retcode = devread(sobj, buf, size);
    } else {
      set_errcode(EINVAL);
    }

    /* Need to decrease the reference to FCB */
    FCB_decref(fcb);

  } else {
    set_errcode(EBADF);
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
  
    if(devwrite) {
      retcode = devwrite(sobj, buf, size);
    } else {
      set_errcode(EINVAL);
    }

    /* Need to decrease the reference to FCB */
    FCB_decref(fcb);

  } else {
    set_errcode(EBADF);
  }

  return retcode;
}


int sys_Close(int fd)
{
	if(fd<0 || fd>=MAX_FILEID) {
		set_errcode(EBADF);
		return -1;
	}
	int retcode = 0;
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
	if(oldfd<0 || newfd<0 || oldfd>=MAX_FILEID || newfd>=MAX_FILEID) {
		set_errcode(EBADF);
		return -1;  	
	}

	FCB* old = get_fcb(oldfd);
	FCB* new = get_fcb(newfd);

	if(old==NULL) {
		set_errcode(EBADF);
		return -1;
	}

	if(old!=new) {
		if(new) 
			FCB_decref(new);
		FCB_incref(old);
		CURPROC->FIDT[newfd] = old;
	}
	return 0;
}



/*
  Copy file descriptor oldfd.

  This call returns 0 on success and -1 on failure.
  Possible reasons for failure:
  - Either oldfd or newfd is invalid.
 */
int sys_Dup(int oldfd)
{
	if(oldfd<0 || oldfd>=MAX_FILEID) {
		set_errcode(EBADF);
		return NOFILE;	
	}

	FCB* old = get_fcb(oldfd);

	if(old==NULL) {
		set_errcode(EBADF);
		return NOFILE;
	}

	Fid_t newfd = 0;
	while(newfd<MAX_FILEID && get_fcb(newfd)!=NULL)
		newfd++;
	if(newfd==MAX_FILEID) {
		set_errcode(EMFILE);
	}

	CURPROC->FIDT[newfd] = old;
	FCB_incref(old);

	return newfd;
}



