#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>

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

/* Array of Core objects, one per core */
static Core CORE[MAX_CORES];

/* Number of cores */
static unsigned int ncores = 0;



/* Used to store the set of core threads' signal mask */
static sigset_t core_signal_set;

/* Used to store the singleton set containing SIGUSR1 */
static sigset_t sigusr1_set;

/* Core barrier */
static pthread_barrier_t system_barrier, core_barrier;



/* PIC daemon statistics */
static unsigned long PIC_loops;

/* Save the sigaction for SIGUSR1 */
static struct sigaction USR1_saved_sigaction;

/* The sigaction for SIGUSR1 (core interrupts) */
static struct sigaction USR1_sigaction;

/* This gives a rough serial port timeout of 300 msec */
#define SERIAL_TIMEOUT 300000



/* Forward decl. of per-core signal handler */
static void sigusr1_handler(int signo, siginfo_t* si, void* ctx);

/* Forward decl. of routine that signals PIC shutdown */
static void pic_signal();



/* Initialize static vars. This is called via pthread_once() */
static pthread_once_t init_control = PTHREAD_ONCE_INIT;
static void initialize()
{
	/* 
		Sigaction object for core interrupts.
		A core interrupt is delivered as signal USR1.

		Note that USR1 will be blocked during execution
		of interrupt handlers; 
	 */
	USR1_sigaction.sa_sigaction = sigusr1_handler;
	USR1_sigaction.sa_flags = SA_SIGINFO;
	sigemptyset(& USR1_sigaction.sa_mask);


	/* Create the sigmask to block all signals, except USR1 */
	CHECK(sigfillset(&core_signal_set));
	CHECK(sigdelset(&core_signal_set, SIGUSR1));

	/* Create the mask for blocking SIGUSR1 */
	CHECK(sigemptyset(&sigusr1_set));
	CHECK(sigaddset(&sigusr1_set, SIGUSR1));
}


/***
 *
 *   CPU simulation
 *
 ****/


/*
	Static func to access the thread-local Core.
*/
_Thread_local uint cpu_core_id;
static inline Core* curr_core() {
	return CORE+cpu_core_id;
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

	/* Initialize the thread-local id variable */
	cpu_core_id = core->id;

	/* Set core signal mask */
	CHECKRC(pthread_sigmask(SIG_BLOCK, &core_signal_set, NULL));

	/* sync with all cores and PIC */
	pthread_barrier_wait(& system_barrier);

	/* execute the boot code */
	core->bootfunc();

	/* Reset interrupt handlers to null, to stop processing interrupts. */
	for(int i=0; i<maximum_interrupt_no; i++) {
		core->intvec[i] = NULL;
	}		

	/* sync with all cores */
	pthread_barrier_wait(& core_barrier);

	/* Stop PIC daemon */
	if(core->id==0) pic_signal();

	/* sync with all cores and PIC */
	pthread_barrier_wait(& system_barrier);

	return _core;
}



/* 
	Send SIGUSR1 to the specified core.

	This function does not raise any interrupt flags.
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

	This is called mainly by the PIC (except for ICI...)
 */
static inline void raise_interrupt(Core* core, Interrupt intno) 
{
	uint32_t imask = 1<<intno;

	/* Atomically set the intno bit and return previous value */
	uint32_t prev = __atomic_fetch_or(& core->intr_pending, imask, __ATOMIC_ACQ_REL);

	if(! (prev & imask)) {
		/* 
			We only signal the core on a 0->1 transition of the
			irq bit.
		*/

#if defined(CORE_STATISTICS)
		core->irq_raised[intno] ++;
#endif

		interrupt_core(core);
	}
}



/*
	This is called by the SIGUSR1 signal handler to
	dispatch a pending interrupt, lowest first.
	Therefore, it is called with SIGUSR1 masked.

	If there are more interrupts, a new SIGUSR1 will be
	sent to the core, before the chosen handler is called.
 */
static inline void dispatch_interrupts(Core* core)
{
	assert(cpu_core_id==core->id);

	/*
		We loop until we have some handler, or no irqs are pending
	 */
	while(1) {

		Interrupt irq;
		uint32_t ipnvec;

		/* Get the lowest interrupt with an atomic operation */
		uint32_t ipvec = core->intr_pending;
		do {

			/* Nothing, return */
			if(! ipvec) return;

			irq = __builtin_ctz(ipvec);
			assert(irq < maximum_interrupt_no);

			ipnvec = ipvec & ~(1 << irq);

		} while(! __atomic_compare_exchange_n(& core->intr_pending, &ipvec, ipnvec, 
					1, /* weak CAS */
					__ATOMIC_ACQ_REL, 
					__ATOMIC_RELAXED)
		);

#if defined(CORE_STATISTICS)
		core->irq_delivered[irq]++;
#endif

		interrupt_handler* handler =  core->intvec[irq];
		if(handler != NULL) {

			/* 
				If more interrupts are pending, we need to
				deliver them. To this effect, send yourself
				another signal
			*/
			if(ipnvec) 
				interrupt_core(core);

			/* 
				Now, call the handler 
			 */
			handler();
			return;
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


/* Return time in nanoseconds from given clock */
static inline uint64_t get_clock_nsec(clockid_t clk_id)
{
	struct timespec curtime;
	CHECK(clock_gettime(clk_id, &curtime));
	return curtime.tv_nsec + curtime.tv_sec*1000000000ull;	
}


/* Coarse clock return time in microseconds */
static TimerDuration get_coarse_time()
{
	return get_clock_nsec(CLOCK_MONOTONIC_COARSE) / 1000ull;
}




/*****************************************

	I/O subsystem
	--------------

	The I/O subsystem consists of io_device objects.

	An io_device handles a file descriptor that is connected to some
	'peripheral'.  The file descriptor must be 'pollable-able' (i.e. not a disk file) 
	and support non-blocking mode.

	Model outline:

	A fd (e.g., a FIFO) is operated at two ends: the cores, via I/O *transfers*,
	and the 'actual device' (e.g., a terminal), via I/O *operations*.

	An io_device is ready if I/O (read/write) operations may succeed (as reported by epoll).
	When epoll returns devices as ready, interrupts are raised. 
	For regular IPC facilities (pipes etc), epoll shoud be used in Edge-Triggered mode,
	vs.  Level-Triggered mode, in order to save CPU.


 *****************************************/


typedef enum io_direction
{
	IODIR_RX = 1,         /* Data incoming from peripheral */
	IODIR_TX = 2,         /* Data outgoing to peripheral */
	IODIR_BX = IODIR_RX|IODIR_TX   /* Bidirectional flow */
} io_direction;



struct io_device;

/*
	An event handler, called by the event loop to process an I/O event
	on a io_device's fd.

	In particular, the event loop calls handlers of all reported events,
	as follows:
	(a)  the event is described by a  struct epoll_event object E (see epoll)
	(b)  E->data.ptr of the event is cast to  io_device  D
	(c)  D->handler( D, E ) is called
 */
typedef void (*event_handler)(struct io_device*, 
	struct epoll_event* evt);

/*
	This is the main abstraction of a simulated peripheral device
 */
typedef struct io_device {
	int fd;              		/* file descriptor */
	io_direction iodir;  		/* device direction */
	int events;					/* epoll events */

	event_handler handler;		/* called by the event loop */
	Core* volatile int_core;	/* core to receive interrupts */
	Interrupt irq;				/* interrupt to send */

	/* Status of the last I/O operation */
	int ok;						/* 1 if ok, 0 if error */
	int errcode;				/* if !ok, the error code from errno */
} io_device;



/**************************

	Some utilities 

  
  *************************/


/*
	This very basic handler simply raises an interrupt
	as determined by the fields of the io_device.

 */
static void io_basic_handler(io_device* dev, 
							  struct epoll_event* evt)
{
	if(evt->events & (EPOLLHUP|EPOLLERR)) {
		return;
	}

	int eflag = dev->iodir==IODIR_RX ? EPOLLIN : EPOLLOUT;

	if(evt->events & eflag) {
		raise_interrupt(dev->int_core, dev->irq);
	}
}


/*
	This function performs a  read()/write() on the file descriptor of the
	given device, depending on the argument iodir:
	   if iodir==IODIR_RX  a read is performed.
	   if iodir==IODIR_TX  a write is performed.
	   else abort().

	Success is defined as either returning data, or End-of-stream (for read),
	or EAGAIN status is returned. 
	Any other outcome to the operation is a serious error and should not happen
	(note that on EINTR the operation repeats)
 */
static ssize_t io_xfer(io_device* dev, io_direction iodir, void* buffer, size_t count)
{
	ssize_t (*io_syscall)(int,void*,size_t);

	if(iodir==IODIR_RX) 
		io_syscall = (void*)read;
	else if(iodir==IODIR_TX)
		io_syscall = (void*)write;
	else
		abort();
	
	ssize_t rc;
	while((rc=io_syscall(dev->fd, buffer, count))==-1 && errno == EINTR);

	dev->ok = (rc>=0) || (rc==-1 && (errno==EAGAIN || errno==EWOULDBLOCK));
	if(! dev->ok) {
		dev->errcode = errno;
		perror("io_tranfer:");
	}

	return rc;
}


/*
	A convenience function, calls io_xfer with dev->iodir (the "default" direction).
	This MUST NOT be IODIR_BX
 */
static ssize_t io_transfer(io_device* dev, void* buffer, size_t count)
{
	return io_xfer(dev, dev->iodir, buffer, count);
}


/*
	Checks the return value of close for errors.
 */
static int checked_close(int fd)
{
	int rc;
	while((rc = close(fd))==-1 && errno==EINTR);
	if(rc==-1) perror("checked_xclose: ");
	return rc;
}




/*********************

	PIC   (Programmable Interrupt Controller)

 *********************/

/*
	A virtual PIC, implemented as an event loop dispatching interrupts
	to cores.
 */
struct PIC {

	int active;		   /* flags this pic as active */
	int epfd;		   /* epoll fd    */
	io_device evt;    /* event device, used to signal termination */

};

static struct PIC pic;


/* 
	Register a device to the PIC.

	This is a convenience function.
 */
static void pic_register(io_device* dev)
{
	struct epoll_event evt;
	evt.events = dev->events;
	evt.data.ptr = dev;

	CHECK(epoll_ctl(pic.epfd, EPOLL_CTL_ADD, dev->fd, &evt));
}

/*
	Delivers an event that causes the PIC to deactivate.
 */
static void pic_signal()
{
	uint64_t sigval = 1ul;

	CHECK(io_xfer(&pic.evt, IODIR_TX, &sigval, sizeof(sigval)));
}


static void pic_evt_handler(io_device* dev, struct epoll_event* evt)
{
	assert(dev==&pic.evt);
	pic.active = 0;
}


/* Initialize the PIC */
static void initialize_pic()
{
	pic.epfd = epoll_create1(0);
	CHECK(pic.epfd);

	pic.active = 1;

	pic.evt.fd = eventfd(0, EFD_NONBLOCK);
	CHECK(pic.evt.fd);

	pic.evt.iodir = IODIR_RX;
	pic.evt.events = EPOLLIN;
	pic.evt.handler = pic_evt_handler;
	pic.evt.int_core = NULL;
	pic.evt.irq = maximum_interrupt_no;

	pic_register(& pic.evt);
}


static void finalize_pic()
{
	CHECK(checked_close(pic.evt.fd));
	CHECK(checked_close(pic.epfd));
}


static void pic_event_loop()
{
#define EVENT_QUEUE_SIZE 16

	while(pic.active) {
		struct epoll_event equeue[EVENT_QUEUE_SIZE];

		int eqlen;

		while( (eqlen = epoll_wait(pic.epfd, equeue, EVENT_QUEUE_SIZE, 100000))==-1 
			&& errno==EINTR) {}
		CHECK(eqlen);

		PIC_loops ++;

		for(int i=0; i<eqlen; i++) {
			/* Invoke handler in each event */
			io_device* dev = equeue[i].data.ptr;

			dev->handler(dev, &equeue[i]);
		}
	}
}



/*****

	Timers

	Used to deliver ALARM interrupts to cores. Based on the
	linux-specific 'timerfd' API.

 ****/
static io_device TIMER[MAX_CORES];


static void initialize_timers(vm_config* vmc)
{
	for(uint c=0; c<vmc->cores; c++) {
		int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);

		CHECK(TIMER[c].fd = fd);
		TIMER[c].iodir = IODIR_RX;

		TIMER[c].handler = io_basic_handler;
		TIMER[c].int_core = CORE+c;
		TIMER[c].irq = ALARM;

		TIMER[c].events = EPOLLIN;

		pic_register(&TIMER[c]);
	}
}


static void finalize_timers(vm_config* vmc)
{
	for(uint c=0; c<vmc->cores; c++) {
		CHECK(checked_close(TIMER[c].fd));
	}	
}


/*****

	Serial devices

	Each serial device is implemented as a pair of pipes

 ****/

/* Current number of terminals */
static uint nterm = 0;

static io_device CON[MAX_TERMINALS];  /* The consoles */

static io_device KBD[MAX_TERMINALS];  /* The keyboards */


static void init_serial_device(io_device* dev, int fd, io_direction iodir)
{
	dev->fd = fd;
	/* Set file descriptor to non-blocking */
	CHECK(fcntl(fd, F_SETFL, O_NONBLOCK));
	dev->iodir = iodir;

	dev->handler = io_basic_handler;
	dev->int_core = & CORE[0];
	dev->irq = (iodir==IODIR_RX)? SERIAL_RX_READY : SERIAL_TX_READY;

	dev->events = (iodir==IODIR_RX)? EPOLLIN : EPOLLOUT;

	/* Set edge-triggered behaviour */
	dev->events |= EPOLLET;
	dev->ok = 1;

	pic_register(dev);
}


static void initialize_terminals(vm_config* vmc) 
{
	for(uint i=0; i < vmc->serialno; i++) {
		init_serial_device(&KBD[i], vmc->serial_in[i], IODIR_RX);
		init_serial_device(&CON[i], vmc->serial_out[i], IODIR_TX);
	}
}

static void finalize_terminals(vm_config* vmc)
{
	for(uint i=0; i < vmc->serialno; i++) {
		CHECK(checked_close(CON[i].fd));
		CHECK(checked_close(KBD[i].fd));
	}
}



/*
	I/O system initialization

	This is run by the main thread.
 */

static void pic_daemon(vm_config* vmc)
{

	/* Change the thread name */
	char oldname[16];
	CHECKRC(pthread_getname_np(pthread_self(), oldname, 16));
	CHECKRC(pthread_setname_np(pthread_self(), "tinyos_vm"));

	/* Set signal mask to block SIGUSR1 (just in case!) */
	sigset_t saved_mask;
	CHECKRC(pthread_sigmask(SIG_BLOCK, &sigusr1_set, &saved_mask));

	/* Setup the pic */
	initialize_pic();

	/* Setup the devices */
	initialize_timers(vmc);
	initialize_terminals(vmc);

	/* Initialize PIC statistics */
	PIC_loops = 0;

	/* sync with all cores */
	pthread_barrier_wait(& system_barrier);

	/* enter the event loop */
	pic_event_loop();

	/* sync with all cores */
	pthread_barrier_wait(& system_barrier);

	/* Cleanup pic */
	finalize_pic();

	/* Cleanup devices */
	finalize_timers(vmc);
	finalize_terminals(vmc);


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


int vm_config_serial(vm_config* vmc, uint serialno, int nowait)
{
	if(serialno>MAX_TERMINALS) return -1;

	/* If nowait is requested, we will open fifos with O_NONBLOCK.
	   This will fail (for serial_out) if the fifos are not already open 
	   on the terminal emulator side */
	int BLOCK = nowait ? O_NONBLOCK : 0;

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

		if(! open_fifo("con", i, O_WRONLY | BLOCK)) return -1;
		if(! open_fifo("kbd", i, O_RDONLY | BLOCK)) return -1;
		/* By opening O_RDWR we are guaranteed that even if the other
		   end (terminals) closes the connection, we will not
		   get SIGPIPE or read 0.
		  */
		//if(! open_fifo("con", i, O_RDWR | BLOCK)) return -1;
		//if(! open_fifo("kbd", i, O_RDWR | BLOCK)) return -1;
	}

	/* Everything was successful, initialize vmc */
	vmc->serialno = serialno;
	for(uint i=0; i<serialno; i++) {
		vmc->serial_out[i] = fds[2*i];		
		vmc->serial_in[i] = fds[2*i+1];
	}

	return 0;
}



void vm_boot(interrupt_handler bootfunc, uint cores, uint serialno)
{
	vm_config VMC;

	/* Prepare the VMC */
	VMC.bootfunc = bootfunc;
	VMC.cores = cores;
	CHECK(vm_config_serial(&VMC, serialno, 0));

	/* Run ! */
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


	/* Initialize terminals */
	nterm = vmc->serialno;
#if 0
	for(uint i=0; i<nterm; i++)
		terminal_init(& TERM[i], vmc->serial_in[i], vmc->serial_out[i]);
#endif

	/* Init the cores */
	ncores = vmc->cores;

	/* Initialize the barriers */
	pthread_barrier_init(& system_barrier, NULL, ncores+1);
	pthread_barrier_init(& core_barrier, NULL, ncores);


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

	/* Run the interrupt controller daemon on this thread */	
	pic_daemon(vmc);

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
#if 0
	for(uint i=0; i<nterm; i++)
		CHECK(terminal_destroy(& TERM[i]));
#endif
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
		fprintf(stderr, "  hlt(rst): %4tu(%4tu)", CORE[c].hlt_count, CORE[c].rst_count);
		fprintf(stderr, "  hltt: %7.3lf", 1E-6*CORE[c].hlt_time);
		double util = 100.0 - 100.0 * CORE[c].hlt_time / (double)CORE[c].run_time ;
		total_util += util;
		fprintf(stderr, "  util %%: %5.1lf", util);		
		fprintf(stderr,"\n");
	}
	fprintf(stderr,"Avg(util)=%8.2lf\n", total_util);
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
#if defined(CORE_STATISTICS)
	/* 
		First, close interrupts so we do not miss a restart, and
		we are not rescheduled...
	 */
	sigset_t saved_mask;
	CHECKRC(pthread_sigmask(SIG_BLOCK, &sigusr1_set, &saved_mask));

	Core* core = curr_core();

	core->hlt_count ++;
	TimerDuration stime0 = get_clock_nsec(CLOCK_MONOTONIC);
#endif

	/* 
		Sleep until a SIGUSR1 signal interrupts us.
	 */
	int rc __attribute__((unused)) 
		= pselect(0, NULL, NULL, NULL, NULL, &core_signal_set);
	if(  errno != EINTR ) perror("cpu_core_halt:" );
	assert(rc==-1 && errno == EINTR);


#if defined(CORE_STATISTICS)
	core->hlt_time += (get_clock_nsec(CLOCK_MONOTONIC)-stime0)/1000ull;

	if(sigismember(&saved_mask, SIGUSR1))
		CHECKRC(pthread_sigmask(SIG_UNBLOCK, &sigusr1_set, NULL));
#endif
}




void cpu_core_restart(uint c)
{
#if defined(CORE_STATISTICS)		
	__atomic_fetch_add(& CORE[c].rst_count, 1 , __ATOMIC_RELAXED);
#endif

	interrupt_core(CORE+c);
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

  /* Interrupts are blocked when context is first executed */
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
	
	timerfd_settime(TIMER[cpu_core_id].fd, 0, &newtime, &oldtime);

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
		KBD[serial].int_core = core;
	else 
		CON[serial].int_core = core;
}


/*
	Try to read a byte from serial port 'serial' and store it into the location
	pointed by 'ptr'.  If the operation succeds, 1 is returned. If not, 0 is returned.
 */
int bios_read_serial(uint serial, char* ptr)
{
	return io_transfer(& KBD[serial], ptr, 1)==1;
}


/*
	Try to write byte 'value' to serial port 'serial'. If the operation succeds, 
	1 is returned. If not, 0 is returned.
 */
int bios_write_serial(uint serial, char value)
{
	return io_transfer(& CON[serial], &value, 1)==1;
}


