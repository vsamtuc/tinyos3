
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

  tinyos_replace_stdio();

  if(GetTerminalDevices()>0) {
    int fid = OpenTerminal(0);
    if(fid!=NOFILE && fid!=0) {
      Dup2(fid,0);
      Close(fid);
    }

    /* Open standard output */
    fid = OpenTerminal(0);
    if(fid!=NOFILE && fid!=1) {
      Dup2(fid,1);
      Close(fid);
    }
  } else {
    tinyos_pseudo_console();
  }
  /* Open standard input */

  /* Just start task Symposium */
  Exec(SymposiumOfProcesses, argl, args);

  Close(0);
  Close(1);

  while( WaitChild(NOPROC, NULL)!=NOPROC ); /* Wait for all children */

  tinyos_restore_stdio();

  return 0;
}

/****************************************************/

void usage(const char* pname)
{
  printf("usage:\n  %s <ncores> <nterm> <philosophers> <bites> [<Dbase>] [<Dgap>]\n\n  \
    where:\n\
    <ncores> is the number of cpu cores to use,\n\
    <nterm> is the number of terminals to use,\n\
    <philosiphers> is from 1 to %d\n\
    <bites> is the number of times each philisopher eats.\n\n\
    <Dbase> integers (maybe negative) control \n\
    <Dgap>  the hardness of the computation (0 if omitted)\n",
	 pname, MAX_PROC);
  exit(1);
}



int FMIN= FBASE;
int FMAX= FBASE+FGAP;

int main(int argc, const char** argv) 
{
  unsigned int ncores, nterm;
  int nphil, bites;
  int dBase = 0, dGap = 0;

  if(argc < 5 || argc > 7) usage(argv[0]); 
  ncores = atoi(argv[1]);
  nterm = atoi(argv[2]);
  nphil = atoi(argv[3]);
  bites = atoi(argv[4]);
  if(argc>=6) dBase = atoi(argv[5]);
  if(argc==7) dGap = atoi(argv[6]);

  /* check arguments */

  if( (nphil <= 0) || (nphil > MAX_PROC) ) usage(argv[0]); 
  if( (bites <= 0) ) usage(argv[0]); 

  /* adjust work per fibo call (to adapt to many philosophers/bites) */
  symposium_t symp;
  symp.N = nphil;
  symp.bites = bites;
  adjust_symposium(&symp, dBase, dGap);

  /* boot TinyOS */
  printf("*** Booting TinyOS\n");
  boot(ncores, nterm, boot_symposium, sizeof(symp), &symp);
  fprintf(stderr,"FMIN = %d    FMAX = %d\n",symp.fmin,symp.fmax);
  printf("*** TinyOS halted. Bye!\n");

  return 0;
}


