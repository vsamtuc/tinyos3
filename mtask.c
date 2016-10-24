
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>

#include "tinyoslib.h"
#include "symposium.h"


/*
 	A standalone program to launch the Dining Philisophers
 	symposium implementation.
 */


/*
 * This is the initial task, which starts all the other tasks (except for the idle task). 
 */
int boot_symposium(int argl, void* args)
{
  /* Open standard input */
  int fid = OpenTerminal(0);
  assert(GetTerminalDevices()>0 ? (fid>=0 && fid<MAX_FILEID) : fid==NOFILE);
  if(fid!=NOFILE && fid!=0) {
    Dup2(fid,0);
    Close(fid);
  }

  /* Open standard output */
  fid = OpenTerminal(0);
  assert(GetTerminalDevices()>0 ? (fid>=0 && fid<MAX_FILEID) : fid==NOFILE);
  if(fid!=NOFILE && fid!=1) {
    Dup2(fid,1);
    Close(fid);
  }

  tinyos_replace_stdio();

  /* Just start task Symposium */
  Exec(Symposium, argl, args);

  Close(0);
  Close(1);

  while( WaitChild(NOPROC, NULL)!=NOPROC ); /* Wait for all children */

  tinyos_restore_stdio();

  return 0;
}

/****************************************************/

void usage(const char* pname)
{
  printf("usage:\n  %s <ncores> <nterm> <philosophers> <bites>\n\n  \
    where:\n\
    <ncores> is the number of cpu cores to use,\n\
    <nterm> is the number of terminals to use,\n\
    <philosiphers> is from 1 to %d\n\
    <bites> is the number of times each philisopher eats.\n",
	 pname, MAX_PROC);
  exit(1);
}



int FMIN= FBASE;
int FMAX= FBASE+FGAP;

int main(int argc, const char** argv) 
{
  unsigned int ncores, nterm;
  int nphil, bites;

  if(argc!=5) usage(argv[0]); 
  ncores = atoi(argv[1]);
  nterm = atoi(argv[2]);
  nphil = atoi(argv[3]);
  bites = atoi(argv[4]);

  /* check arguments */

  if( (nphil <= 0) || (nphil > MAX_PROC) ) usage(argv[0]); 
  if( (bites <= 0) ) usage(argv[0]); 

  /* adjust work per fibo call (to adapt to many philosophers/bites) */
  if(1) {
    FMIN = FBASE - (int)( log((double)(2*nphil*bites))/log(.5+.5*sqrt(5.)));
    FMAX = FMIN + FGAP;
    printf("FMIN = %d    FMAX = %d\n",FMIN,FMAX);
  }

  /* boot TinyOS */
  {
    int args[4];
    args[0] = nphil; args[1] = bites;
    args[2] = FMIN; args[3] = FMAX;

    printf("*** Booting TinyOS\n");
    boot(ncores, nterm, boot_symposium, sizeof(args), args);
  }
  printf("*** TinyOS halted. Bye!\n");

  return 0;
}


