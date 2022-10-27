#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include "util.h"
#include "bios.h"

/*
	Implementation of bios.h API


	Basic idea:
	- Each core is simulated by a pthread
	- One POSIX timer per core thread
	- Core threads mask all signals except for USR1.
	- The PIC thread receives all signals and dispatches them to
	the right core thread by raising SIGUSR1.

 */


#if 0
#define CORE_STATISTICS
#endif


/*
	Per-core data.
 */
typedef struct core
{
	uint id;
	interrupt_handler* bootfunc;
	pthread_t thread;

	struct sigevent timer_sigevent;
	timer_t timer_id;

	volatile uint32_t intr_pending;
	interrupt_handler* intvec[maximum_interrupt_no];


#if defined(CORE_STATISTICS)
	/* Statistics */
	volatile uintptr_t irq_count;
	volatile uintptr_t irq_raised[maximum_interrupt_no];
	volatile uintptr_t irq_delivered[maximum_interrupt_no];
	volatile uintptr_t hlt_count;
	volatile uintptr_t rst_count;
	volatile TimerDuration hlt_time;
	volatile TimerDuration run_time;
#endif

} Core;


/* Used to store the set of core threads' signal mask */
static sigset_t core_signal_set;

/* Uset to store the singleton set containing SIGUSR1 */
static sigset_t sigusr1_set;

/* Uset to store the singleton set containing SIGALRM */
static sigset_t sigalrm_set;

/* Used to create the signalfd */
static sigset_t signalfd_set;

/* Array of Core objects, one per core */
static Core CORE[MAX_CORES];

/* Number of cores */
static unsigned int ncores = 0;

/* Core barrier */
static pthread_barrier_t system_barrier, core_barrier;

/* Flag that signals that PIC daemon should be active */
static volatile sig_atomic_t PIC_active;

/* Bit vector denoting halted cores */
static _Atomic uint32_t halt_vector;

/* PIC thread id */
static pthread_t PIC_thread;

/* Save the sigaction for SIGUSR1 */
static struct sigaction USR1_saved_sigaction;

/* The sigaction for SIGUSR1 (core interrupts) */
static struct sigaction USR1_sigaction;

/* This gives a rough serial port timeout of 300 msec */
#define SERIAL_TIMEOUT 300000

/* Forward decl. of per-core signal handler */
static void sigusr1_handler(int signo, siginfo_t* si, void* ctx);

/* PIC daemon statistics */
static unsigned long PIC_loops;

/* Physical cores (needed for some heuristics) */
static unsigned int physical_cores;


/* Initialize static vars. This is called via pthread_once() */
static pthread_once_t init_control = PTHREAD_ONCE_INIT;
static void initialize()
{
	physical_cores = get_nprocs();

	USR1_sigaction.sa_sigaction = sigusr1_handler;
	USR1_sigaction.sa_flags = SA_SIGINFO;
	sigemptyset(& USR1_sigaction.sa_mask);

	/* Create the sigmask to block all signals, except USR1 */
	CHECK(sigfillset(&core_signal_set));
	CHECK(sigdelset(&core_signal_set, SIGUSR1));

	/* Create the mask for blocking SIGUSR1 */
	CHECK(sigemptyset(&sigusr1_set));
	CHECK(sigaddset(&sigusr1_set, SIGUSR1));

	/* Create the mask for  SIGALRM */
	CHECK(sigemptyset(&sigalrm_set));
	CHECK(sigaddset(&sigalrm_set, SIGALRM));


	/* Create signaldf_set */
	CHECK(sigemptyset(&signalfd_set));
	CHECK(sigaddset(&signalfd_set, SIGUSR1));
	CHECK(sigaddset(&signalfd_set, SIGALRM));
}




/*
	Static func to access the thread-local Core.
*/
_Thread_local uint cpu_core_id;
static inline Core* curr_core() {
	return CORE+cpu_core_id;
}


/*
	Cause PIC daemon to loop. This needs to happen when we wish 
	the PIC daemon to refresh the list of fds it is polling.
 */
static inline void interrupt_pic_thread()
{
	union sigval coreval;
	coreval.sival_ptr = NULL; /* This is silly, but silences valgrind */
	coreval.sival_int = -1;
	CHECKRC(pthread_sigqueue(PIC_thread, SIGUSR1, coreval));
}



/*
	Helper pthread-startable function to launch a core thread.
*/
static void* core_thread(void* _core)
{
	Core* core = (Core*)_core;

	/* Clear pending bitvec */
	core->intr_pending = 0;

	/* Default interrupt handlers */
	for(int i=0; i<maximum_interrupt_no; i++) 
		core->intvec[i] = NULL;

	cpu_core_id = core->id;

	/* Set core signal mask */
	CHECKRC(pthread_sigmask(SIG_BLOCK, &core_signal_set, NULL));

	/* create a thread-specific timer */
	core->timer_sigevent.sigev_notify = SIGEV_SIGNAL;
	core->timer_sigevent.sigev_signo = SIGALRM;
	core->timer_sigevent.sigev_value.sival_int = core->id;
	// Could also be CLOCK_REALTIME
	CHECK(timer_create(CLOCK_MONOTONIC, & core->timer_sigevent, & core->timer_id));

	/* sync with all cores */
	pthread_barrier_wait(& system_barrier);

	/* execute the boot code */
	core->bootfunc();

	/* Reset interrupt handlers to null, to stop processing interrupts. */
	for(int i=0; i<maximum_interrupt_no; i++) {
		core->intvec[i] = NULL;
	}		

	/* Delete the core timer */
	CHECK(timer_delete(core->timer_id));

	pthread_barrier_wait(& core_barrier);

	/* Stop PIC daemon */
	if(core->id==0) {
		PIC_active = 0;
		interrupt_pic_thread();
	}

	/* sync with all cores */
	pthread_barrier_wait(& system_barrier);

	return _core;
}


/*
	Set pending interrupt, return previous value
 */
static inline int intr_fetch_set(Core* core, Interrupt intno)
{
	uint32_t sel = 1<<intno;
	uint32_t old = __atomic_fetch_or(& core->intr_pending, sel, __ATOMIC_ACQ_REL);
	return (old & sel) != 0;
}

/*
	Clear pending interrupt for core, return previous value
 */
static inline int intr_fetch_clear(Core* core, Interrupt intno)
{
	uint32_t sel = 1<<intno;
	uint32_t old = __atomic_fetch_and(& core->intr_pending, ~sel, __ATOMIC_ACQ_REL);
	return (old & sel) != 0;
}


/*
	If there are pending interrupts for core, clear lowest pending interrupt,
	store in intp and return 1, else return 0.
 */
static inline int intr_fetch_lowest(Core* core, Interrupt* intp)
{
	Interrupt irq;
	uint32_t ipnvec;
	uint32_t ipvec = core->intr_pending;

	do {
		if(! ipvec) return 0;

		irq = __builtin_ctz(ipvec);
		assert(irq < maximum_interrupt_no);

		ipnvec = ipvec & ~(1 << irq);

	} while(! __atomic_compare_exchange_n(& core->intr_pending, &ipvec, ipnvec, 0, 
		__ATOMIC_ACQ_REL, __ATOMIC_RELAXED));

	*intp = irq;
	return 1;
}


/* 
	Cause the given core to be interrupted in the future.
	This function does not add a pending interrupt, but
	causes a signal to be sent to the core.
 */
static inline void interrupt_core(Core* core)
{
	union sigval coreval;
	coreval.sival_ptr = NULL; /* This is to silence valgrind */
	coreval.sival_int = core->id;	

	CHECKRC(pthread_sigqueue(core->thread, SIGUSR1, coreval));
}


/*
	Raise an interrupt to a core.

	Adds intno as pending for the core and causes a signal to
	be delivered.
 */
static inline void raise_interrupt(Core* core, Interrupt intno) 
{
	if(! intr_fetch_set(core, intno) ) {

#if defined(CORE_STATISTICS)
		core->irq_raised[intno] ++;
#endif

		interrupt_core(core);
	}
}




/*
	Dispatch any pending interrupts, lowest first.
	Cease if an interrupt causes core change.
 */
static inline void dispatch_interrupts(Core* core)
{
	assert(cpu_core_id==core->id);

	while(1) {

		Interrupt irq;
		if(! intr_fetch_lowest(core, &irq)) break;
	
		assert(0 <= irq  && irq < maximum_interrupt_no);
#if defined(CORE_STATISTICS)
		core->irq_delivered[irq]++;
#endif
		interrupt_handler* handler =  core->intvec[irq];
		if(handler != NULL) handler();
	
		/* 
			Note: after a successful dispatch, we may not
			be running on the core any more (!), if the
			dispatch action has been scheduled...
		*/
		if(cpu_core_id != core->id) {
			//if(core->intr_pending) interrupt_core(core);
			break;
		}
	}
}


/*
	This is the signal handler for core threads, to handle interrupts.
 */
static void sigusr1_handler(int signo, siginfo_t* si, void* ctx)
{
	Core* core = & CORE[si->si_value.sival_int];

#if defined(CORE_STATISTICS)
	core->irq_count++;
#endif

	dispatch_interrupts(core);
}


/*
	Peripherals
 */


/* Coarse clock */
static TimerDuration get_coarse_time()
{
	struct timespec curtime;
	CHECK(clock_gettime(CLOCK_REALTIME_COARSE, &curtime));
	return curtime.tv_nsec / 1000ul + curtime.tv_sec*1000000ull;
}



/*
	An io_device handles a file descriptor that is connected to some
	'peripheral' in stream (byte-oriented) mode. The file descriptor must be
	'select-able' (i.e. not a disk file) and support non-blocking mode.

	Model outline:

	A fd (e.g., a FIFO) is operated at two ends: the cores, via I/O *transfers*,
	and the 'actual device' (e.g., a terminal), via I/O *operations*.

	Each io_device is unidirectional: the fd is either only read or only written to,
	by this program (bidirectional fds, such as sockets, can be handled by a pair of
	io_device objects).  

	An io_device is ready if I/O operations may succeed (as reported by select()).

	A not-ready device is made ready when select() returns it as such.

	A ready device is made not-ready on each failed attempt to do an I/O transfer.

	When a not-ready device becomes ready, an interrupt is raised.
 */

typedef enum io_direction
{
	IODIR_RX = 0,
	IODIR_TX = 1
} io_direction;



/*
	An io_device is a file descriptor from which we either read or write bytes.
 */
typedef struct io_device
{
	int fd;              		/* file descriptor */
	io_direction iodir;  		/* device direction */

	Core* volatile int_core;	/* core to receive interrupts */
	volatile int ready;  		/* ready flag */
	TimerDuration last_int;	    /* used by PIC for timeouts */
} io_device;


/*
	Determine device readiness without blocking
 */
static int io_device_ready(int fd, io_direction dir) {
	struct pollfd pfd;
	pfd.fd = fd;
	int evt = (dir==IODIR_RX) ? POLLIN : POLLOUT;
	pfd.events = evt;
	CHECK(poll(&pfd, 1, 0));
	return (pfd.revents & evt) ? 1 : 0;
}


/*
	Check that the a device is connected and in a good state.
 */
static int io_device_check(io_device* dev)
{
	struct pollfd fds = { .fd=dev->fd, .events=POLLIN };
	int rc;
	do {
		rc=poll(&fds, 1, 0);
	} while( rc == -1 && errno==EINTR );
	CHECK(rc);
	rc = (fds.revents & (POLLHUP|POLLERR))==0;
	assert(rc);
	return rc;
}



/*
	Initialize device
 */
static void io_device_init(io_device* this, int fd, io_direction iodir)
{
	this->fd = fd;
	this->iodir = iodir;
	this->int_core = &CORE[0];
	this->ready = io_device_ready(fd, iodir);
	this->last_int = get_coarse_time();

	/* Set file descriptor to non-blocking */
	CHECK(fcntl(fd, F_SETFL, O_NONBLOCK));
}

/*
	Destroy device
 */
static int io_device_destroy(io_device* this)
{
	int rc;
	while((rc = close(this->fd))==-1 && errno==EINTR);
	if(rc==-1) perror("io_device_destroy: ");
	return rc;
}


static int io_device_read(io_device* this, char* ptr)
{
	assert(this->iodir == IODIR_RX);
	int rc;
	while((rc=read(this->fd, ptr, 1))==-1 && errno == EINTR);

	int ok = rc==0 || rc==1 || (rc==-1 && (errno==EAGAIN || errno==EWOULDBLOCK));
	if(!ok) perror("io_device_read:");
	assert(ok);

	if(rc!=1 && this->ready) {
		this->ready = 0;
		interrupt_pic_thread();
	}
	return rc==1;
}


static int io_device_write(io_device* this, char value)
{
	assert(this->iodir == IODIR_TX);

	/* Try to write */
	int rc;
	while((rc = write(this->fd, &value, 1))==-1 && errno == EINTR);

	int ok = rc==1 || (rc==-1 && (errno == EAGAIN || errno==EWOULDBLOCK || errno == EPIPE));
	if(! ok) perror("io_device_write:");
	assert(ok);

	if(rc!=1 && this->ready) {
		this->ready = 0;
		interrupt_pic_thread();
	} 

	return rc==1;
}





/*
	A terminal encapsulates two io_devices: a console and a keyboard
 */
typedef struct terminal
{
	io_device con, kbd;            /* fds for terminal fifos */
} terminal;

/* The terminal table */
static terminal TERM[MAX_TERMINALS];

/* Current number of terminals */
static uint nterm = 0;

/*
	Init the devices for this terminal
 */
static void terminal_init(terminal* this, int fdin, int fdout)
{
	io_device_init(& this->kbd, fdin, IODIR_RX);
	io_device_init(& this->con, fdout, IODIR_TX);
}

/*
	Destroy the terminal devices
 */
static int terminal_destroy(terminal* this)
{
	return  io_device_destroy(& this->con)
	      | io_device_destroy(& this->kbd);
}





/*
	The PIC daemon dispatches interrupts to core threads,
	by calling raise_interrupt().

	Interrupts sent include
	(a) ALARM, when the core timer expires
	(b) SERIAL_RX_READY  &  SERIAL_TX_READY, when some 
		io_device becomes ready.

	Implementation:
	- Use Linux signal file descriptors to receive signals. Currently,
	  two signals are used:
	  * SIGUSR1 is sent by io_device to signify that some io_device is NOT READY.
	    Otherwise it is discarded. The signal simply wakes up the PIC_daemon thread.
	    This however causes the PIC loop to include the devices to the ones monitored.

	  * SIGALRM is sent to indicate that some core timer has expired. This
	    results to an interrupt on the core.

	- Monitor these fds together with the fds of the terminals.
	
	- At each loop dispatch interrupts as needed:
	  * ALARM interrupts to those cores whose timer has expired
	  * SERIAL_RX/TX_READY to those cores handling the interrupts of
	    an io_device which is now READY.		
 */



/******************************

	Helpers for signal fds

 ******************************/


static inline int read_signalfd(int sfd, struct signalfd_siginfo* sfdinfo)
{
	int rc = read(sfd, sfdinfo, sizeof(struct signalfd_siginfo));

	assert( (rc==-1 || rc==sizeof(struct signalfd_siginfo))
		&&  (rc!=-1 || errno==EAGAIN || errno==EWOULDBLOCK) );

	return rc;
}

static inline void drain_signalfd(int sfd)
{
	struct signalfd_siginfo sfdinfo;
	while(read_signalfd(sfd, &sfdinfo)!=-1);
}


static int open_signalfd(sigset_t* set)
{
	int fd = signalfd(-1, set, SFD_NONBLOCK);
	CHECK(fd);
	return fd;
}

static void close_signalfd(int sfd)
{
	drain_signalfd(sfd);
	CHECK(close(sfd));
}



/********************************

	PIC loop state and helpers

 ********************************/

typedef struct pic_selector
{
	fd_set fds[2];
	int maxfd;
	TimerDuration system_clock;
} pic_selector;


static void pic_selector_reset(pic_selector* ps)
{
	FD_ZERO(& ps->fds[0]);
	FD_ZERO(& ps->fds[1]);
	ps->maxfd = 0;
}


static inline void pic_add_fd(pic_selector* ps, io_direction dir, int fd)
{
	FD_SET(fd, ps->fds + dir);
	if(fd >= ps->maxfd) ps->maxfd = fd+1;
}



static int pic_select(pic_selector* ps)
{
	/* select will sleep for about SLOW_HZ usec (half the system_clock res.) */
	struct timeval sleeptime = { .tv_sec=0, .tv_usec = SERIAL_TIMEOUT };
	int selcode = select(ps->maxfd, &ps->fds[IODIR_RX], &ps->fds[IODIR_TX], NULL, &sleeptime);

	if(selcode == -1)  {
		/* An error is likely EINTR */
		if(errno != EINTR)  perror("PIC_loops: "); else perror("PIC_select:");
	} else {
		/* update system clock */
		ps->system_clock = get_coarse_time();
	}
	return selcode;
}


static inline int pic_is_ready(pic_selector* ps, io_direction dir, int fd)
{
	return FD_ISSET(fd, & ps->fds[dir]);
}




static inline void pic_add_io_device(pic_selector* ps, io_device* dev)
{
	if(! dev->ready) pic_add_fd(ps, dev->iodir, dev->fd);
}


static inline void pic_add_terminal(pic_selector* ps, terminal* term)
{
	/* First check that terminal is connected, without blocking.
	   This is done by polling for errors on the kbd device. */
	if(io_device_check(& term->kbd)) {
		pic_add_io_device(ps, & term->kbd);
		pic_add_io_device(ps, & term->con);
	}
}


static void term_dev_raise_if_ready(io_device* dev, pic_selector* ps)
{
	if(    pic_is_ready(ps, dev->iodir, dev->fd) 
		|| (ps->system_clock - dev->last_int) > SERIAL_TIMEOUT 
		)
	{
		dev->ready = 1;
		dev->last_int = ps->system_clock;
		Core* core = (Core*) dev->int_core;
		switch(dev->iodir) {
			case IODIR_RX:
				raise_interrupt(core, SERIAL_RX_READY); break;
			case IODIR_TX:
				raise_interrupt(core, SERIAL_TX_READY); break;
		}
	}
}




static void PIC_daemon(void)
{

	/* Change the thread name */
	char oldname[16];
	CHECKRC(pthread_getname_np(pthread_self(), oldname, 16));
	CHECKRC(pthread_setname_np(pthread_self(), "tinyos_vm"));

	/* Open signal queues */
	int sigusr1fd = open_signalfd(&sigusr1_set);
	int sigalrmfd = open_signalfd(&sigalrm_set);

	/* Set signal mask to block the signals monitored by signalfd */
	sigset_t saved_mask;
	CHECKRC(pthread_sigmask(SIG_BLOCK, &signalfd_set, &saved_mask));
		
	/* sync with all cores */
	pthread_barrier_wait(& system_barrier);
	
	/* The PIC multiplexing loop */
	while(PIC_active) {

		pic_selector ps;

		pic_selector_reset(&ps);

		for(uint i=0; i<nterm; i++)
			pic_add_terminal(&ps, & TERM[i]);

		pic_add_fd(&ps, IODIR_RX, sigalrmfd);
		pic_add_fd(&ps, IODIR_RX, sigusr1fd);

		if(pic_select(&ps) == -1)
			continue;

		PIC_loops++ ;

		if( pic_is_ready(&ps, IODIR_RX, sigalrmfd)!=-1 ) {
			struct signalfd_siginfo sfdinfo;

			while(read_signalfd(sigalrmfd, &sfdinfo) != -1) {
				Core* core = & CORE[sfdinfo.ssi_int];
				raise_interrupt(core, ALARM);
			}
		}


		if( pic_is_ready(&ps, IODIR_RX, sigusr1fd)!=-1 ) {
			drain_signalfd(sigusr1fd);
		}


		for(uint i=0; i<nterm; i++) {
			terminal* term = & TERM[i];			

			term_dev_raise_if_ready(& term->con, &ps);
			term_dev_raise_if_ready(& term->kbd, &ps);
		}


	}


	/* sync with all cores */
	pthread_barrier_wait(& system_barrier);

	/* Close signal fds */
	close_signalfd(sigusr1fd);
	close_signalfd(sigalrmfd);

	/* Restore sigmask */
	CHECKRC(pthread_sigmask(SIG_SETMASK, &saved_mask, NULL));

	/* Reset name */
	CHECKRC(pthread_setname_np(pthread_self(), oldname));
}




/*****************************************
	Public API
 *****************************************/

/*
	VM boot functions.
 */


int vm_config_terminals(vm_config* vmc, uint serialno, int nowait)
{
	if(serialno>MAX_TERMINALS) return -1;

	/* If nowait is requested, we will open fifos with O_NONBLOCK.
	   This will fail (for serial_out) if the fifos are not already open 
	   on the terminal emulator side */
	//int BLOCK = nowait ? O_NONBLOCK : 0;
	int BLOCK = O_NONBLOCK;

	/* Used to store the fifo fds temporarily */
	unsigned int fdno = 0;
	int fds[2*MAX_TERMINALS];

	/* Helper to open a FIFO */
	int open_fifo(const char* name, uint no, int flags) {
		char fname[16];
		snprintf(fname,16,"%s%u", name, no);

		int fd = open(fname, flags);
		if(fd==-1) {
			for(uint i=0; i<fdno; i++)  close(fds[i]);
			return 0;
		} else {
			fds[fdno++] = fd;
			return 1;
		}
	}


	for(uint i=0; i<serialno; i++) {
		/* Open serial port. Order is important!!! */
		//if(! open_fifo("con", i, O_WRONLY | BLOCK)) return -1;
		//if(! open_fifo("kbd", i, O_RDONLY | BLOCK)) return -1;
		if(! open_fifo("con", i, O_RDWR | BLOCK)) return -1;
		if(! open_fifo("kbd", i, O_RDWR | BLOCK)) return -1;
	}

	/* Everything was successful, initialize vmc */
	vmc->serialno = serialno;
	for(uint i=0; i<serialno; i++) {
		vmc->serial_out[i] = fds[2*i];		
		vmc->serial_in[i] = fds[2*i+1];
	}

	return 0;
}


void vm_configure(vm_config* vmc, interrupt_handler bootfunc, uint cores, uint serialno)
{
	vmc->bootfunc = bootfunc;
	vmc->cores = cores;
	CHECK(vm_config_terminals(vmc, serialno, 0));
}



void vm_boot(interrupt_handler bootfunc, uint cores, uint serialno)
{
	vm_config VMC;
	vm_configure(&VMC, bootfunc, cores, serialno);
	vm_run(&VMC);
}



void vm_run(vm_config* vmc)
{

	CHECK_CONDITION(vmc->cores > 0 && vmc->cores <= MAX_CORES);
	CHECK_CONDITION(ncores==0);
	CHECK_CONDITION(vmc->serialno <= MAX_TERMINALS);

	/* This is called only once in the life of the process. */
	CHECKRC(pthread_once(&init_control, initialize));

	/* Install signal handler for SIGUSR1 */
	CHECK(sigaction(SIGUSR1, &USR1_sigaction, &USR1_saved_sigaction));

	/* Set pic_active to 1 */
	PIC_thread = pthread_self();
	PIC_active = 1;	

	/* Initialize terminals */
	nterm = vmc->serialno;
	for(uint i=0; i<nterm; i++)
		terminal_init(& TERM[i], vmc->serial_in[i], vmc->serial_out[i]);

	/* Init the cores */
	ncores = vmc->cores;

	/* Initialize the barriers */
	pthread_barrier_init(& system_barrier, NULL, ncores+1);
	pthread_barrier_init(& core_barrier, NULL, ncores);

	/* Initialize the halted vector */
	halt_vector = 0;

	/* Launch the core threads */
	for(uint c=0; c < ncores; c++) {
		/* Initialize Core */
		CORE[c].bootfunc = vmc->bootfunc;
		CORE[c].id = c;


#if defined(CORE_STATISTICS)
		/* Initialize Core statistics */
		CORE[c].irq_count = 0;
		for(uint intno=0; intno<maximum_interrupt_no;intno++) {
			CORE[c].irq_delivered[intno] = 0;
			CORE[c].irq_raised[intno] = 0;
			CORE[c].hlt_count = 0;
			CORE[c].rst_count = 0;
			CORE[c].hlt_time = 0;
			CORE[c].run_time = get_coarse_time();
		}
#endif

		/* Create the core thread */
		CHECKRC(pthread_create(& CORE[c].thread, NULL, core_thread, &CORE[c]));
		char thread_name[16];
		CHECK(snprintf(thread_name,16,"core-%d",c));
		CHECKRC(pthread_setname_np(CORE[c].thread, thread_name));
	}

	/* Initialize PIC statistics */
	PIC_loops = 0;

	/* Run the interrupt controller daemon on this thread */	
	PIC_daemon();

	/* Wait for core threads to finish */
	for(uint c=0; c<ncores; c++) {
		CHECKRC(pthread_join(CORE[c].thread, NULL));

#if defined(CORE_STATISTICS)
		CORE[c].run_time = get_coarse_time() - CORE[c].run_time;
#endif
	}

	/* Delete the Core table */
	ncores = 0;

	/* Destroy the core barrier */
	pthread_barrier_destroy(& system_barrier);
	pthread_barrier_destroy(& core_barrier);

	/* Finalize terminals */
	for(uint i=0; i<nterm; i++)
		CHECK(terminal_destroy(& TERM[i]));
	nterm = 0;

	/* Restore signal mask before VM execution */
	CHECK(sigaction(SIGUSR1, &USR1_saved_sigaction, NULL));


	/* print statistics */
#if defined(CORE_STATISTICS)
	fprintf(stderr,"PIC loops: %lu \n", PIC_loops);
	double total_util = 0.0;
	for(uint c=0; c < vmc->cores; c++) {
		fprintf(stderr,"Core %3d: irq_count=%6tu. deliv(raised):  ",
			c, CORE[c].irq_count);
		for(uint i=0;i<maximum_interrupt_no;i++) 
			fprintf(stderr," %tu(%tu)",CORE[c].irq_delivered[i], CORE[c].irq_raised[i]);
		fprintf(stderr, "  hlt(rst): %tu(%tu)", CORE[c].hlt_count, CORE[c].rst_count);
		fprintf(stderr, "  hltt: %2.3lf", 1E-6*CORE[c].hlt_time);
		double util = 100.0 - 100.0 * CORE[c].hlt_time / (double)CORE[c].run_time ;
		total_util += util;
		fprintf(stderr, "  util %%: %3.2lf", util);		
		fprintf(stderr,"\n");
	}
	fprintf(stderr,"Avg(util)=%6.2lf\n", total_util);
#endif
}



/*
	CPU functions.
 */


uint cpu_cores()
{
	return ncores;
}



void cpu_core_halt()
{
	CHECKRC(pthread_sigmask(SIG_BLOCK, &sigusr1_set, NULL));

	Core* core = curr_core();
	uint32_t cmask = 1 << cpu_core_id;

#if defined(CORE_STATISTICS)
	TimerDuration stime0 = get_coarse_time();
#endif

	/* Set halt bit */
	__atomic_fetch_or(& halt_vector, cmask, __ATOMIC_RELAXED);

#if defined(CORE_STATISTICS)
	core->hlt_count ++;
#endif

	siginfo_t info;

	/* Sleep for 10 msec */
	//struct timespec halt_time = {.tv_sec=0l, .tv_nsec=10000000l};
	//int rc = sigtimedwait(&sigusr1_set, &info, &halt_time);
	int rc = sigwaitinfo(&sigusr1_set, &info);

	if(rc>0) {
		/* Got signal, dispatch */
		dispatch_interrupts(core);
	}
	else {
		assert(rc==-1 &&  (errno == EINTR || errno == EAGAIN));
	}

#if defined(CORE_STATISTICS)
	/* Unset halt bit */
	core->hlt_time += get_coarse_time()-stime0;
#endif

	__atomic_fetch_and(& halt_vector, ~cmask, __ATOMIC_RELAXED);

	CHECKRC(pthread_sigmask(SIG_UNBLOCK, &sigusr1_set, NULL));
}

static int __core_restart(uint c)
{
	uint32_t cmask = 1 << c;

	uint32_t prevhv = __atomic_fetch_and(& halt_vector, ~cmask, __ATOMIC_RELAXED);
	if( prevhv & cmask ) {
		interrupt_core(CORE+c);
#if defined(CORE_STATISTICS)		
		__atomic_fetch_add(& CORE[c].rst_count, 1 , __ATOMIC_RELAXED);
#endif

		return 1;
	} else 
		return 0;
}


void cpu_core_restart(uint c)
{
	__core_restart(c);
}


void cpu_core_restart_one()
{
	/* Only restart if core_id < physical_cores */
	uint32_t hv = halt_vector;

	if( (hv=halt_vector)!=0 ) {
		uint c = __builtin_ctz(hv);
		if(c < physical_cores)
			__core_restart(c);
	}

}

void cpu_core_restart_all()
{
	for(uint c=0; c < ncores; c++)
		__core_restart(c);
}

void cpu_core_barrier_sync()
{
	pthread_barrier_wait(& core_barrier);
}

void cpu_ici(uint core)
{
	assert(core < ncores);
	raise_interrupt(& CORE[core], ICI);
}

void cpu_interrupt_handler(Interrupt interrupt, interrupt_handler handler)
{
	sigset_t curss;
	CHECKRC(pthread_sigmask(SIG_BLOCK, &sigusr1_set, &curss));
	curr_core()->intvec[interrupt] = handler;
	CHECKRC(pthread_sigmask(SIG_SETMASK, &curss, NULL));
}

int cpu_interrupts_enabled()
{
	sigset_t curss;
	CHECKRC(pthread_sigmask(SIG_BLOCK, NULL, & curss));
	return sigismember(&curss, SIGUSR1)==0;
}

int cpu_disable_interrupts()
{
	sigset_t curss;
	CHECKRC(pthread_sigmask(SIG_BLOCK, &sigusr1_set, & curss));
	return sigismember(&curss, SIGUSR1)==0;
}

void cpu_enable_interrupts()
{
	CHECKRC(pthread_sigmask(SIG_UNBLOCK, &sigusr1_set, NULL));
}


void cpu_initialize_context(cpu_context_t* ctx, void* ss_sp, size_t ss_size, void (*ctx_func)())
{
  /* Init the context from this context! */
  getcontext(ctx);
  ctx->uc_link = NULL;

  /* initialize the context stack */
  ctx->uc_stack.ss_sp = ss_sp;
  ctx->uc_stack.ss_size = ss_size;
  ctx->uc_stack.ss_flags = 0;

  //CHECKRC(pthread_sigmask(0, NULL, & ctx->uc_sigmask));  /* We don't want any signals changed */
  sigfillset( & ctx->uc_sigmask );
  makecontext(ctx, (void*) ctx_func, 0);
}


void cpu_swap_context(cpu_context_t* oldctx, cpu_context_t* newctx)
{
	swapcontext(oldctx, newctx);
}



/*
	BIOS functions
 */


TimerDuration bios_set_timer(TimerDuration usec)
{
	time_t sec = usec / 1000000;
	long nsec = (usec % 1000000) * 1000ull;
	
	struct itimerspec newtime = {
		.it_value = {.tv_sec=sec, .tv_nsec=nsec},
		.it_interval = {.tv_sec=0, .tv_nsec=0}
	};

	struct itimerspec oldtime;
	
	timer_settime(curr_core()->timer_id, 0, &newtime, &oldtime);

	assert(oldtime.it_interval.tv_sec ==0 && oldtime.it_interval.tv_nsec==0);
	return 1000000*oldtime.it_value.tv_sec + oldtime.it_value.tv_nsec/1000ull;
}

TimerDuration bios_cancel_timer()
{
	return bios_set_timer(0);
}


TimerDuration bios_clock()
{
	return get_coarse_time();
}	



uint bios_serial_ports()
{
	return nterm;
}


/*
	Make interrupts of type 'intno' for serial port port 'serial' be sent
	to 'core'.  By default, initially all interrupts are sent to core 0.
 */
void bios_serial_interrupt_core(uint serial, Interrupt intno, uint coreid)
{
	if(!(serial < nterm)) return;
	if(!(intno==SERIAL_RX_READY || intno==SERIAL_TX_READY)) return;
	if(!(coreid < ncores)) return;

	Core* core = & CORE[coreid];

	if(intno==SERIAL_RX_READY)
		TERM[serial].kbd.int_core = core;
	else 
		TERM[serial].con.int_core = core;
}


/*
	Try to read a byte from serial port 'serial' and store it into the location
	pointed by 'ptr'.  If the operation succeds, 1 is returned. If not, 0 is returned.
 */
int bios_read_serial(uint serial, char* ptr)
{
	return io_device_read(& TERM[serial].kbd, ptr);
}


/*
	Try to write byte 'value' to serial port 'serial'. If the operation succeds, 
	1 is returned. If not, 0 is returned.
 */
int bios_write_serial(uint serial, char value)
{
	return io_device_write(& TERM[serial].con, value);
}


