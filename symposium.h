
#ifndef __SYMPOSIUM__H
#define __SYMPOSIUM__H


/**
	@file symposium.h
	@brief An implementation of Dining Philosophers.

	The Dining Philisophers are sitting around a Symposium
	roundtable. Each philosopher comes to the table, cycles 
	between thinking and eating a number of times, and leaves the 
	table. 
	However, 
	- in order to eat she needs to hold a fork in each hand, and
	- there is only one fork between two philosophers.
	Therefore, a philosopher may wait hungry for forks to become
	available. 
	
	In our simulation, a philosopher is a thread. The symposium is
	implemented as a monitor, with each philosopher waiting at
	their own condition variable.

	During each thinking or eating period, a philosopher computes 
	Fibonacci numbers using an exponentially
	expensive recursion:
	\f[  F(n+2) = F(n+1) + F(n) \f]
	with \f$F(0) = 0\f$ and \f$F(1)=1\f$.
	The complexity of this recursion is \f$ O( \phi^n )\f$ where
	\f$\phi=\frac{1+\sqrt{5}}{2}\approx 1.618\f$ is the __golden ratio__.

	For each thinking session, an integer \f$n\f$ is drawn uniformly at random
	from the set \f$ [f_\text{min}, f_\text{max}] \f$. 

	Overall, a symposium is specified by four numbers:
	- @c N, the number of philosophers
	- `bites`, the number of times each of them eats
	- \f$f_\text{min}, f_\text{max}\f$ which determine 
	  the time of each thinking and eating period.

	In order to make symposia with a large number of philosophers or number of bites,
	we can compute suitable values of \f$f_\text{min}, f_\text{max}\f$. We use the 
	following simple formulas:
	\f[  f_\text{min} =  F_\text{BASE} - \log_\phi( 2*N*\text{bites} ) \f]
	and
	\f[  f_\text{max} = f_\text{min} + F_\text{GAP}. \f]
	
	The constants \f$F_\text{BASE}\f$ and \f$F_\text{GAP}\f$ are defined in the source
	code. 

	@see FBASE
	@see FGAP
*/

#include "tinyos.h"

/** @brief The default for constant \f$F_\text{BASE}\f$ */
#define FBASE 35

/** @brief The default for constant \f$F_\text{GAP}\f$ */
#define FGAP  10

/** @brief Compute the n-th Fibonacci number recursively. 

	The purpose of this function is to burn CPU cycles. Its complexity
	is \f$ O( \phi^n )\f$ where
	\f$\phi=\frac{1+\sqrt{5}}{2}\approx 1.618\f$ is the golden ratio.

	@param n the index of the Fibonacci number
	@returns the n-th Fibonacci number
*/
extern unsigned int fibo(unsigned int n);

/** @brief A philosopher's state. */
typedef enum { NOTHERE=0, THINKING, HUNGRY, EATING } PHIL;


/** @brief A symposium definition.

	The four numbers defining a symposium.
*/
typedef struct {
	int N;				/**< Number of philosophers */
	int bites;			/**< Number of bites each philosopher takes. */
	int fmin, fmax;		/**< Values used by the Fibbonacci routines */
} symposium_t;


/** @brief Adjust a symposium's duration. 

	This function computes \f$f_\text{min}, f_\text{max}\f$ based
	on the values 
	\f[  F_\text{BASE} = \text{FBASE}+\text{dBASE}  \f]
	and 
	\f[  F_\text{GAP} = \text{FGAP}+\text{dGAP}.  \f]
	
	The computed values are stored in @c table.

	@param table the symposium table whose \f$f\f$-values are computed.
	@param dBASE added to @ref FBASE 
	@param dGAP  added to @ref FGAP
	@see FBASE
	@see FGAP
*/
void adjust_symposium(symposium_t* table, int dBASE, int dGAP);


/** @brief A symposium monitor.

	Such an object must be shared between all philosopher
	threads/processes.
*/
typedef struct {
	Mutex mx;			/**< Monitor mutex */
	symposium_t* symp; 	/**< The symposium definition */
	PHIL* state;		/**< state[i] i=1...N]: Philosopher state */
	CondVar* hungry;    /**< hungry[i] i=...N: condition var for philosophers */
} SymposiumTable;


/** @brief Initialize a symposium monitor.

	Note: this method allocates memory.
	Therefore, @ref SymposiumTable_destroy must be called 

	@param table the monitor
	@param symp the symposium
*/
void SymposiumTable_init(SymposiumTable* table, symposium_t* symp);

/** @brief Destroy a symposium monitor.

	@param table the monitor
*/
void SymposiumTable_destroy(SymposiumTable* table);


/** @brief The philosopher.

	This function implements philosopher logic. A symposium
	consists of @c N threads or processes executing this function.

	@param table the symposium monitor
	@param i the philosopher index
*/
void SymposiumTable_philosopher(SymposiumTable* table, int i);


/** @brief Run a symposium using threads.

	In this implememntation, each philosopher is a thread.

	This program can be called as follows:
	@code
	symposium_t symp = ...;
	Exec(SymposiumOfThreads, sizeof(symp), &symp);
	@endcode
*/
int SymposiumOfThreads(int argl, void* args);


/** @brief Run a symposium using processes.

	In this implememntation, each philosopher is a process.

	This program can be called as follows:
	@code
	symposium_t symp = ...;
	Exec(SymposiumProcesses, sizeof(symp), &symp);
	@endcode
*/
int SymposiumOfProcesses(int argl, void* args);


#endif