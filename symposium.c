#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>

#include "util.h"
#include "bios.h"
#include "tinyos.h"
#include "symposium.h"

#define QUIET 0  /* Use 1 for supperssing printing (for timing tests), 0 for normal printing */

/*
  This file contains a number of example programs for tinyos.
*/


void adjust_symposium(symposium_t* symp, int dBASE, int dGAP)
{
	symp->fmin = FBASE + dBASE - 
		(int)( log((double)(2*symp->N*symp->bites))/log(.5+.5*sqrt(5.)));
	symp->fmax = symp->fmin + FGAP +dGAP;
}



/* We use the recursive definition of Fibonacci numbers, implemented by the routines below,
  to "burn" CPU cycles. The complexity of the routine is exponential in n.
*/

int fiborand(int fmin, int fmax) { return lrand48() % (fmax-fmin+1) + fmin; }
unsigned int fibo(unsigned int n) /* Very slow routine */
{
  if(n<2) return n;
  else return fibo(n-1)+fibo(n-2);
}

/* 
  Dining Philosophers.

  In this implementation of the Dining Philosohpers problem, each philosopher
  is thinking about Fibonacci numbers...

  The implementation follows the monitor pattern.
 */


/* utilities */
int LEFT(int i, int N) { return (i+1) % N; }
int RIGHT(int i, int N) { return (i+N-1) % N; }


/* Prints the current state given a change (described by fmt) for
 philosopher ph */
void print_state(int N, PHIL* state, const char* fmt, int ph)
{
#if QUIET==0
  int i;
  if(N<100) {
    for(i=0;i<N;i++) {
      char c= (".THE")[state[i]];
      if(i==ph) printf("[%c]", c);
      else printf(" %c ", c);
    }
  }
  printf(fmt, ph);
#endif
}

/* Functions think and eat (just burn CPU cycles). */
void think(int fmin, int fmax) { fibo(fiborand(fmin, fmax)); }
void eat(int fmin, int fmax)  { think(fmin, fmax); }

/* Attempt to make a (hungry) philosopher i to start eating */
void trytoeat(SymposiumTable* S, int i)
{
  int N = S->symp->N;
  PHIL* state = S->state;

  if(state[i]==HUNGRY && state[LEFT(i,N)]!=EATING && state[RIGHT(i,N)]!=EATING) {
    state[i] = EATING;
    print_state(N, state, "     %d is eating\n",i);
    Cond_Signal(&(S->hungry[i]));
  }
}


void SymposiumTable_philosopher(SymposiumTable* S, int i)
{
  /* cache locally for convenience */
  int N = S->symp->N;
  int bites = S->symp->bites;
  int fmin = S->symp->fmin;
  int fmax = S->symp->fmax;
  PHIL* state = S->state;

  Mutex_Lock(& S->mx);		/* Philosopher arrives in thinking state */
  state[i] = THINKING;
  print_state(N, state, "     %d has arrived\n",i);
  Mutex_Unlock(& S->mx);

  for(int j=0; j<bites; j++) {	/* Number of bites (mpoykies) */
    think(fmin, fmax);

    Mutex_Lock(& S->mx);
    state[i] = HUNGRY;
    trytoeat(S,i);		/* This may not succeed */
    while(state[i]==HUNGRY) {
      print_state(N, state, "     %d waits hungry\n",i);
      Cond_Wait(& S->mx, &(S->hungry[i])); /* If hungry we sleep. trytoeat(i) will wake us. */
    }
    assert(state[i]==EATING); 
    Mutex_Unlock(& S->mx);
    
    eat(fmin, fmax);

    Mutex_Lock(& S->mx);
    state[i] = THINKING;	/* We are done eating, think again */
    print_state(N, state, "     %d is thinking\n",i);
    trytoeat(S, LEFT(i,N));		/* Check if our left and right can eat NOW. */
    trytoeat(S, RIGHT(i,N));
    Mutex_Unlock(& S->mx);
  }

  Mutex_Lock(& S->mx);
  state[i] = NOTHERE;		/* We are done (eaten all the bites) */
  print_state(N, state, "     %d is leaving\n",i);
  Mutex_Unlock(& S->mx);
}



void SymposiumTable_init(SymposiumTable* table, symposium_t* symp)
{
	table->symp = symp;
	table->mx = MUTEX_INIT;
	table->state = (PHIL*) xmalloc(symp->N * sizeof(PHIL));
	table->hungry = (CondVar*) xmalloc(symp->N * sizeof(CondVar));
	for(int i=0; i<symp->N; i++) {
		table->state[i] = NOTHERE;
		table->hungry[i] = COND_INIT;
	}
}

void SymposiumTable_destroy(SymposiumTable* table)
{
	free(table->state);
	free(table->hungry);
}



typedef struct { int i; SymposiumTable* S; } philosopher_args;

/* Philosopher process */
int PhilosopherProcess(int argl, void* args)
{
	assert(argl == sizeof(philosopher_args));
	philosopher_args* A = args;
	SymposiumTable_philosopher(A->S, A->i);
	return 0;
}


/*
  This process executes a "symposium" for a number of philosophers.
 */
int SymposiumOfProcesses(int argl, void* args)
{
  assert(argl == sizeof(symposium_t));
  symposium_t* symp = args;
  int N = symp->N;

  /* Initialize structures */
  SymposiumTable S;
  SymposiumTable_init(&S, symp);
  
  /* Execute philosophers */
  for(int i=0;i<N;i++) {
    philosopher_args Args;
    Args.i = i;
    Args.S = &S;
    Exec(PhilosopherProcess, sizeof(Args), &Args);
  }  

  /* Wait for philosophers to exit */  
  for(int i=0;i<N;i++) {
    WaitChild(NOPROC, NULL);
  }

  SymposiumTable_destroy(&S);
  return 0;
}


int PhilosopherThread(int i, void* symp)
{
	SymposiumTable_philosopher((SymposiumTable*) symp, i);
	return 0;
}

/*
  This process executes a "symposium" for a number of philosophers.
  Each philosopher is a thread.
 */
int SymposiumOfThreads(int argl, void* args)
{
	assert(argl == sizeof(symposium_t));
	symposium_t* symp = args;
	int N = symp->N;

	/* Initialize structures */
	SymposiumTable S;
	SymposiumTable_init(&S, symp);

	/* Execute philosophers */
	Tid_t thread[symp->N];
	for(int i=0;i<N;i++) {
		thread[i] = CreateThread(PhilosopherThread, i, &S);
	}  

	/* Wait for philosophers to exit */  
	for(int i=0;i<N;i++) {
		ThreadJoin(thread[i],NULL);
	}

	SymposiumTable_destroy(&S);

	return 0;
}



