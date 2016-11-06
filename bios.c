#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/signalfd.h>
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

	interrupt_handler* intvec[maximum_interrupt_no];
	sig_atomic_t intpending[maximum_interrupt_no];

	sig_atomic_t int_disabled;
	sig_atomic_t halted;
	rlnode halted_node;
	pthread_cond_t halt_cond;

	/* Statistics */
	int irq_count;
	int irq_raised[maximum_interrupt_no];
	int irq_delivered[maximum_interrupt_no];
} Core;


/* Per-core thread-local Core */
static pthread_key_t Core_key;

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

/* Mutex for implementing core halt */
static pthread_mutex_t core_halt_mutex = PTHREAD_MUTEX_INITIALIZER;

/* List of halted cores */
static rlnode halted_list;

/* PIC thread id */
static pthread_t PIC_thread;

/* Save the sigaction for SIGUSR1 */
static struct sigaction USR1_saved_sigaction;

/* The sigaction for SIGUSR1 (core interrupts) */
static struct sigaction USR1_sigaction;

/* A simulated coarse clock measuring time with a res. of 0.1 sec,
   since "boot". Used for serial device timeouts. */
typedef unsigned long coarse_clock_t;
static volatile coarse_clock_t  system_clock;

/* This is how fast the coarse clock is updated (in usec) */
#define SLOW_HZ 100000

/* This gives a rough serial port timeout of 300 msec */
#define SERIAL_TIMEOUT 3

static void sigusr1_handler(int signo, siginfo_t* si, void* ctx);


/* PIC daemon statistics */
static unsigned long PIC_loops, PIC_usr1_drained, PIC_usr1_queued;


/* Initialize static vars. This is called via pthread_once() */
static pthread_once_t init_control = PTHREAD_ONCE_INIT;
static void initialize()
{
	/* Create the thread-local var for core no. */
	CHECKRC(pthread_key_create(&Core_key, NULL));

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




static void PIC_daemon();  /* forward def */


/*
	Static func to access the thread-local Core.
*/
_Thread_local uint cpu_core_id;
static inline Core* curr_core() {
	return CORE+cpu_core_id;
}


/*
	Cause PIC daemon to loop.
 */
static inline void interrupt_pic_thread()
{
	union sigval coreval;
	coreval.sival_ptr = NULL; /* This is silly, but silences valgrind */
	coreval.sival_int = -1;
	CHECKRC(pthread_sigqueue(PIC_thread, SIGUSR1, coreval));
	__atomic_fetch_add(&PIC_usr1_queued,1,__ATOMIC_RELAXED);
}



/*
	Helper pthread-startable function to launch a core thread.
*/
static void* bootfunc_wrapper(void* _core)
{
	Core* core = (Core*)_core;

	/* Default interrupt handlers */
	for(int i=0; i<maximum_interrupt_no; i++) {
		core->intvec[i] = NULL;
		core->intpending[i] = 0;
	}

	/* Mark interrupts as enabled */
	core->int_disabled = 0;

	/* establish the thread-local id */
	CHECKRC(pthread_setspecific(Core_key, core));
	cpu_core_id = core->id;

	/* Set core signal mask */
	CHECKRC(pthread_sigmask(SIG_BLOCK, &core_signal_set, NULL));

	/* create a thread-specific timer */
	core->timer_sigevent.sigev_notify = SIGEV_SIGNAL;
	core->timer_sigevent.sigev_signo = SIGALRM;
	core->timer_sigevent.sigev_value.sival_int = core->id;
	CHECK(timer_create(CLOCK_REALTIME, & core->timer_sigevent, & core->timer_id));

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
		__atomic_store_n(&PIC_active, 0, __ATOMIC_RELEASE);
		interrupt_pic_thread();
	}

	/* sync with all cores */
	pthread_barrier_wait(& system_barrier);

	return _core;
}


/*
	Raise an interrupt to a core.
 */
static inline void raise_interrupt(Core* core, Interrupt intno) 
{
	union sigval coreval;
	coreval.sival_ptr = NULL; /* This is to silence valgrind */
	coreval.sival_int = core->id;
	core->intpending[intno] = 1;
	core->irq_raised[intno] ++;
	CHECKRC(pthread_sigqueue(core->thread, SIGUSR1, coreval));
	cpu_core_restart(core->id);
}


/*
	Dispatch the pending iterrupts for the given core.
 */
static void dispatch_interrupts(Core* core)
{
	for(int intno = 0; intno < maximum_interrupt_no; intno++) {
		if(core->int_disabled) break; /* will continue at
										 cpu_interrupt_enable()*/
		if(core->intpending[intno]) {
			core->intpending[intno] = 0;
			core->irq_delivered[intno]++;
			interrupt_handler* handler =  core->intvec[intno];
			if(handler != NULL) { 
				handler();
			}
		}
	}	
}


/*
	This is the handler run by core threads to handle interrupts.
 */
static void sigusr1_handler(int signo, siginfo_t* si, void* ctx)
{
	Core* core = & CORE[si->si_value.sival_int];

	core->irq_count++;
	if(core->int_disabled) return;
	dispatch_interrupts(core);
}


/*
	Peripherals
 */


/* Coarse clock */
coarse_clock_t get_coarse_time()
{
	struct timespec curtime;
	CHECK(clock_gettime(CLOCK_REALTIME, &curtime));
	return curtime.tv_nsec / 100000000 + curtime.tv_sec*10;
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
	IODIR_RX,
	IODIR_TX
} io_direction;

/*
	An io_device is a file descriptor from which we either read or write bytes.
 */
typedef struct io_device
{
	int fd;              		/* file descriptor */
	io_direction iodir;  		/* device direction */

	volatile Core* int_core;		/* core to receive interrupts */
	volatile int ready;  		/* ready flag */
	coarse_clock_t last_int;	/* used for timeouts */
} io_device;


static int io_ready(int fd, io_direction dir) {
	struct pollfd pfd;
	pfd.fd = fd;
	int evt = (dir==IODIR_RX) ? POLLIN : POLLOUT;
	pfd.events = evt;
	CHECK(poll(&pfd, 1, 0));
	return (pfd.revents & evt) ? 1 : 0;
}

static void io_device_init(io_device* this, int fd, io_direction iodir)
{
	this->fd = fd;
	this->iodir = iodir;
	this->int_core = &CORE[0];
	this->ready = io_ready(fd, iodir);
	this->last_int = system_clock;

	/* Set file descriptor to non-blocking */
	CHECK(fcntl(fd, F_SETFL, O_NONBLOCK));
}


static int io_device_read(io_device* this, char* ptr)
{
	assert(this->iodir == IODIR_RX);
	int rc;
	while((rc=read(this->fd, ptr, 1))==-1 && errno == EINTR);
	assert(rc==0 || rc==1 || (rc==-1 && (errno==EAGAIN || errno==EWOULDBLOCK)));

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

	assert(rc==1 || (rc==-1 && (errno == EAGAIN || errno==EWOULDBLOCK || errno == EPIPE))); 

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
	Open the FIFOs for this terminal
 */
static int terminal_init(terminal* this, int no)
{
	char fname[32];
	int fd;

	sprintf(fname, "con%d", no);
	fd = open(fname, O_WRONLY);
	if(fd==-1) return -1;
	io_device_init(& this->con, fd, IODIR_TX);

	sprintf(fname, "kbd%d", no);
	fd = open(fname, O_RDONLY);
	if(fd==-1) return -1;
	io_device_init(& this->kbd, fd, IODIR_RX);

	return 0;
}

static int terminal_destroy(terminal* this)
{
	int rc;

	while((rc = close(this->con.fd))==-1 && errno==EINTR);
	if(rc==-1) return -1;

	while((rc = close(this->kbd.fd))==-1 && errno==EINTR);
	if(rc==-1) return -1;

	return 0;
}


/*
	Just a couple of helpers.
 */
static void open_terminal(terminal* term, uint serno)
{
	CHECK(terminal_init(term, serno));	
}
static void close_terminal(terminal* term)
{
	CHECK(terminal_destroy(term));
}
static inline void fdset_add(fd_set* set, int fd, int* nfds)
{
	FD_SET(fd, set);
	if(fd >= *nfds) *nfds = fd+1;
}


static int check_terminal(terminal* term)
{
	/* poll the read side */
	struct pollfd fds = { .fd=term->kbd.fd, .events=POLLIN };
	int rc;
	while( (rc=poll(&fds, 1, 0)) == -1 && errno==EINTR );
	CHECK(rc);
	return (fds.revents & (POLLHUP|POLLERR))==0;
}



/* Helper for PIC_daemon */
static void pic_drain_sigusr1(int sigusr1fd)
{
	struct signalfd_siginfo sfdinfo;
	while(1) {
		int rc = read(sigusr1fd, &sfdinfo, sizeof(sfdinfo));
		if(rc==-1) {
			assert(errno==EAGAIN || errno==EWOULDBLOCK);
			break;
		}
		assert(rc==sizeof(sfdinfo));
		__atomic_fetch_add(&PIC_usr1_drained,1,__ATOMIC_RELAXED);
	}				
}


/*
	The PIC daemon is the dispatcher on interrupts to core threads,
	by calling raise_interrupt().

	Interrupts sent include
	(a) ALARM, when the per-core timer expires
	(b) SERIAL_RX_READY  &  SERIAL_TX_READY, when some 
		io_device becomes ready.

 */
static void PIC_daemon(uint serialno)
{
	nterm = serialno;

	/* establish the thread-local id */
	CHECKRC(pthread_setspecific(Core_key, NULL));

	/* Change the thread name */
	char oldname[16];
	CHECKRC(pthread_getname_np(pthread_self(), oldname, 16));
	CHECKRC(pthread_setname_np(pthread_self(), "tinyos_vm"));

	for(uint i=0; i<nterm; i++)
		open_terminal(& TERM[i], i);

	sigset_t saved_mask;

	int sigusr1fd = signalfd(-1, &sigusr1_set, SFD_NONBLOCK);
	CHECK(sigusr1fd);
	int sigalrmfd = signalfd(-1, &sigalrm_set, SFD_NONBLOCK);
	CHECK(sigalrmfd);

	CHECKRC(pthread_sigmask(SIG_BLOCK, &signalfd_set, &saved_mask));
		
	/* sync with all cores */
	pthread_barrier_wait(& system_barrier);
	
	/* The PIC multiplexing loop */
	while(__atomic_load_n(&PIC_active, __ATOMIC_ACQUIRE)) {
		int maxfd = 0;
		fd_set readfds, writefds;

		/* Prepare to select */
		FD_ZERO(&readfds);
		FD_ZERO(&writefds);

		for(uint i=0; i<nterm; i++) {
			terminal* term = & TERM[i];
			if(!check_terminal(term)) continue;
			if(! term->kbd.ready) fdset_add(&readfds, term->kbd.fd, &maxfd);
			if(! term->con.ready) fdset_add(&writefds, term->con.fd, &maxfd);
		}

		fdset_add(&readfds, sigalrmfd, &maxfd);
		fdset_add(&readfds, sigusr1fd, &maxfd);

		/* select will sleep for about SLOW_HZ usec (half the system_clock res.) */
		struct timeval sleeptime = { .tv_sec=0, .tv_usec = SLOW_HZ };
		int selcode = select(maxfd, &readfds, &writefds, NULL, &sleeptime);

		/* process */
		if(selcode<0) continue;
		__atomic_fetch_add(&PIC_loops,1,__ATOMIC_RELAXED);

		/* update system clock */
		system_clock = get_coarse_time();

		/* First raise ALRM as needed (timers have priority :-) */
		if( FD_ISSET(sigalrmfd, &readfds) ) {
			struct signalfd_siginfo sfdinfo;

			while(1) {
				int rc = read(sigalrmfd, &sfdinfo, sizeof(sfdinfo));
				if(rc==-1) {
					assert(errno==EAGAIN || errno==EWOULDBLOCK);
					break;
				}
				assert(rc==sizeof(sfdinfo));

				Core* core = & CORE[sfdinfo.ssi_int];
				raise_interrupt(core, ALARM);
			}
		}

		/* Discard any USR1 signals to PIC (their purpose was to unblock PIC 
		   from select) */
		if( FD_ISSET(sigusr1fd, &readfds) ) {
			pic_drain_sigusr1(sigusr1fd);
		}

		/* Handle the devices */
		for(uint i=0; i<nterm; i++) {

			terminal* term = & TERM[i];
			if( FD_ISSET(term->con.fd, &writefds) 
				|| (system_clock-term->con.last_int)>SERIAL_TIMEOUT
				) 
			{
				term->con.ready = 1;
				term->con.last_int = system_clock;
				Core* core = (Core*) term->con.int_core;
				raise_interrupt(core, SERIAL_TX_READY);
			}


			if( FD_ISSET(term->kbd.fd, &readfds) 
				|| (system_clock-term->kbd.last_int)>SERIAL_TIMEOUT
				) 
			{
				term->kbd.ready = 1;
				term->kbd.last_int = system_clock;
				Core* core = (Core*) term->kbd.int_core;
				raise_interrupt(core, SERIAL_RX_READY);
			}
		}
	}

	/* sync with all cores */
	pthread_barrier_wait(& system_barrier);

	/* Close signal fds */
	pic_drain_sigusr1(sigusr1fd);
	CHECK(close(sigalrmfd));
	CHECK(close(sigusr1fd));

	/* Restore sigmask */
	CHECKRC(pthread_sigmask(SIG_SETMASK, &saved_mask, NULL));

	/* destroy terminals */
	for(uint i=0; i<nterm; i++)
		close_terminal(& TERM[i]);
	nterm = 0;

	/* Reset name */
	CHECKRC(pthread_setname_np(pthread_self(), oldname));
}




/*****************************************
	Public API
 *****************************************/

/*
	CPU functions.
 */

void vm_boot(interrupt_handler bootfunc, uint cores, uint serialno)
{

	CHECK_CONDITION(cores > 0 && cores <= MAX_CORES);
	CHECK_CONDITION(ncores==0);
	CHECK_CONDITION(serialno <= MAX_TERMINALS);

	/* This is called only once in the life of the process. */
	CHECKRC(pthread_once(&init_control, initialize));

	/* Install signal handler for SIGUSR1 */
	CHECK(sigaction(SIGUSR1, &USR1_sigaction, &USR1_saved_sigaction));

	/* Set pic_active to 1 */
	PIC_thread = pthread_self();
	PIC_active = 1;	

	/* Initialize system_clock */
	system_clock = get_coarse_time();

	/* Initialize the barriers */
	pthread_barrier_init(& system_barrier, NULL, cores+1);
	pthread_barrier_init(& core_barrier, NULL, cores);

	/* Initialize the halted list */
	rlnode_init(&halted_list, NULL);

	/* Launch the core threads */
	ncores = cores;
	for(uint c=0; c < cores; c++) {
		/* Initialize Core */
		CORE[c].bootfunc = bootfunc;
		CORE[c].id = c;

		pthread_cond_init(& CORE[c].halt_cond, NULL);
		CORE[c].halted = 0;
		rlnode_init(& CORE[c].halted_node, &CORE[c]);

		/* Initialize Core statistics */
		CORE[c].irq_count = 0;
		for(uint intno=0; intno<maximum_interrupt_no;intno++) {
			CORE[c].irq_delivered[intno] = 0;
			CORE[c].irq_raised[intno] = 0;
		}

		/* Create the core thread */
		CHECKRC(pthread_create(& CORE[c].thread, NULL, bootfunc_wrapper, &CORE[c]));
		char thread_name[16];
		CHECK(snprintf(thread_name,16,"core-%d",c));
		CHECKRC(pthread_setname_np(CORE[c].thread, thread_name));
	}

	/* Initialize PIC statistics */
	PIC_loops = 0; PIC_usr1_queued = PIC_usr1_drained = 0;

	/* Run the interrupt controller daemon on this thread */	
	PIC_daemon(serialno);

	/* Wait for threads to finish */
	for(uint c=0; c<cores; c++) {
		CHECKRC(pthread_join(CORE[c].thread, NULL));
	}

	/* Destroy the core barrier */
	pthread_barrier_destroy(& system_barrier);
	pthread_barrier_destroy(& core_barrier);

	/* Restore signal mask before VM execution */
	CHECK(sigaction(SIGUSR1, &USR1_saved_sigaction, NULL));

	/* Delete the Core table */
	ncores = 0;

	/* emit statistics */
#if 0
	fprintf(stderr,"PIC loops: %lu  queued/drained= %lu / %lu\n", 
		PIC_loops, PIC_usr1_queued, PIC_usr1_drained);
	for(uint c=0;c<cores;c++) {
		fprintf(stderr,"Core %3d: irq_count=%6d. deliv(raised):\t",
			c, CORE[c].irq_count);
		for(uint i=0;i<maximum_interrupt_no;i++) 
			fprintf(stderr," %d(%d)",CORE[c].irq_delivered[i], CORE[c].irq_raised[i]);
		fprintf(stderr,"\n");
	}
#endif
}


uint cpu_cores()
{
	return ncores;
}

void cpu_core_halt()
{
	/* unmask signals and call sigsuspend */
	Core* core = curr_core();
	assert(! core->int_disabled);
	CHECKRC(pthread_sigmask(SIG_BLOCK, &sigusr1_set, NULL));
	pthread_mutex_lock(& core_halt_mutex);
	core->halted = 1;
	rlist_push_front(&halted_list, & core->halted_node);
	while(core->halted)
		pthread_cond_wait(& core->halt_cond, & core_halt_mutex);
	assert(! core->halted);
	pthread_mutex_unlock(& core_halt_mutex);
	CHECKRC(pthread_sigmask(SIG_UNBLOCK, &sigusr1_set, NULL));
	dispatch_interrupts(core);
}

static inline void core_restart(Core* core)
{
	if(core->halted) {
		core->halted = 0;
		rlist_remove(& core->halted_node);
		pthread_cond_signal(& core->halt_cond);
	}	
}

void cpu_core_restart(uint c)
{
	pthread_mutex_lock(& core_halt_mutex);
	core_restart(CORE+c);
	pthread_mutex_unlock(& core_halt_mutex);	
}

void cpu_core_restart_one()
{
	pthread_mutex_lock(& core_halt_mutex);
	if(! is_rlist_empty(&halted_list)) {
		core_restart((Core*) rlist_pop_front(&halted_list)->obj);
	}	
	pthread_mutex_unlock(& core_halt_mutex);	
}

void cpu_core_restart_all()
{
	pthread_mutex_lock(& core_halt_mutex);
	for(uint c=0; c<ncores; c++)
		core_restart(CORE+c);
	pthread_mutex_unlock(& core_halt_mutex);	
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
	curr_core()->intvec[interrupt] = handler;
}


void cpu_disable_interrupts()
{
	Core* core = curr_core();
	if(! core->int_disabled) {
		CHECKRC(pthread_sigmask(SIG_BLOCK, &sigusr1_set, NULL));
		core->int_disabled = 1;
	}
}

void cpu_enable_interrupts()
{
	Core* core = curr_core();
	if(core->int_disabled) {        
		core->int_disabled = 0;
		CHECKRC(pthread_sigmask(SIG_UNBLOCK, &sigusr1_set, NULL));      
		dispatch_interrupts(curr_core());
	}
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
	curr_core()->intpending[ALARM] = 0;

	assert(oldtime.it_interval.tv_sec ==0 && oldtime.it_interval.tv_nsec==0);
	return 1000000*oldtime.it_value.tv_sec + oldtime.it_value.tv_nsec/1000ull;
}

TimerDuration bios_cancel_timer()
{
	return bios_set_timer(0);
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
	assert(serial < nterm);
	assert(intno==SERIAL_RX_READY || intno==SERIAL_TX_READY);
	assert(coreid < ncores);

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


