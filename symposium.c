#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>

#include "bios.h"
#include "tinyos.h"
#include "symposium.h"

#define QUIET 0  /* Use 1 for supperssing printing (for timing tests), 0 for normal printing */

/*
  This file contains a number of example programs for tinyos.
*/

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

typedef enum { NOTHERE=0, THINKING, HUNGRY, EATING } PHIL;

typedef struct {
  int N;                    /* Number of philosophers */
  PHIL* state;              /* state[N]: Philosopher state */
  Mutex mx;                 /* Mutual exclusion among philosophers (on
                                  array 'state') */
  CondVar *hungry;          /* hungry[N}: put hungry philosophers to sleep */

  int fmin, fmax;           /* Values used by the Fibbonacci routines */
} SymposiumTable;


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
  int N = S->N;
  PHIL* state = S->state;

  if(state[i]==HUNGRY && state[LEFT(i,N)]!=EATING && state[RIGHT(i,N)]!=EATING) {
    state[i] = EATING;
    print_state(N, state, "     %d is eating\n",i);
    Cond_Signal(&(S->hungry[i]));
  }
}


typedef struct { int i; int bites; SymposiumTable* S; } philosopher_args;

/* Philosopher process */
int Philosopher(int argl, void* args)
{
  int i,j;
  int bites;
  SymposiumTable* S;
  PHIL* state;
  int N;

  /* Read arguments */
  assert(argl == sizeof(philosopher_args));
  i = ((philosopher_args*)args)->i;
  bites = ((philosopher_args*)args)->bites; 
  S = ((philosopher_args*)args)->S;
  N = S->N;
  state = S->state;

  Mutex_Lock(& S->mx);		/* Philosopher arrives in thinking state */
  state[i] = THINKING;
  print_state(N, state, "     %d has arrived\n",i);
  Mutex_Unlock(& S->mx);

  for(j=0; j<bites; j++) {	/* Number of bites (mpoykies) */

    think(S->fmin, S->fmax);

    Mutex_Lock(& S->mx);
    state[i] = HUNGRY;
    trytoeat(S,i);		/* This may not succeed */
    while(state[i]==HUNGRY) {
      print_state(N, state, "     %d waits hungry\n",i);
      Cond_Wait(& S->mx, &(S->hungry[i])); /* If hungry we sleep. trytoeat(i) will wake us. */
    }
    assert(state[i]==EATING); 
    Mutex_Unlock(& S->mx);
    
    eat(S->fmin, S->fmax);

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

  return i;
}


/*
  This process executes a "symposium" for a number of philosophers.
 */
int Symposium(int argl, void* args)
{
  int N, bites;			/* Bites (mpoykies) per philosopher */
  int fmin, fmax;
  int i;

  assert(argl == sizeof(int[4]));
  N = ((int*)args)[0];		/* get the arguments */
  bites = ((int*)args)[1];
  fmin  = ((int*)args)[2];
  fmax = ((int*)args)[3];

  /* Initialize structures */
  SymposiumTable S;

  S.N = N;
  S.fmin = fmin;
  S.fmax = fmax;

  S.mx = MUTEX_INIT;
  S.state = (PHIL*) malloc(sizeof(PHIL)*N);
  S.hungry = (CondVar*) malloc(sizeof(CondVar)*N);
  for(i=0;i<N;i++) {
    S.state[i] = NOTHERE;
    S.hungry[i] = COND_INIT;
  }
  
  /* Execute philosophers */
  for(i=0;i<N;i++) {
    philosopher_args Args;
    Args.i = i;
    Args.bites = bites;
    Args.S = &S;
    Exec(Philosopher, sizeof(Args), &Args);
  }  

  /* Wait for philosophers to exit */  
  for(i=0;i<N;i++) {
    WaitChild(NOPROC, NULL);
  }

  free(S.state);
  free(S.hungry);

  return 0;
}


/* Compute good values for fmin and fmax and call symposium */
int Symposium_adjusted(int argl, void* args)
{  
  assert(argl == sizeof(int[4]));
  int N = ((int*)args)[0];    /* get the arguments */
  int bites = ((int*)args)[1];
  int dBASE = ((int*)args)[2];
  int dGAP = ((int*)args)[3];

  int fmin = FBASE + dBASE - (int)( log((double)(2*N*bites))/log(.5+.5*sqrt(5.)));
  int fmax = fmin + FGAP + dGAP;
  if(fmax<fmin) fmax = fmin;

  int Args[4] = { N, bites, fmin, fmax };

  return Symposium(sizeof(Args), Args);
}

