#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <error.h>
#include <mqueue.h>
#include <limits.h>
#include <pthread.h>
#include <poll.h>
#include <argp.h>
#include <ctype.h>

#include <assert.h>
#include <stdarg.h>

#include "unit_testing.h"
#include "util.h"


/*
	Global variables
*/


struct program_arguments ARGS = {
	.show_tests = 0,
	.verbose = 0,
	.use_color = 1,
	.fork = 1,
	.ncore_list = 1 , .core_list = { 1, }, 
	.nterm_list = 1 , .term_list = { 0, },

	.ntests = 0,
	.tests = { }
};


#define INDENT_STACK_MAX 128
#define INDENT_STEP 8                   /* How much INDENT increases the space */


#define BLACK   "\033[30;7m"
#define RED     "\033[31;1m"
#define GREEN   "\033[32;1m"
#define YELLOW  "\033[33;1m"
#define BLUE    "\033[34;1m"
#define MAGENTA "\033[35;1m"
#define CYAN    "\033[36;1m"
#define WHITE   "\033[37;1m"
#define NORMAL  "\033[0m"


static int INDENT_STACK[INDENT_STACK_MAX];     /* Saved indents */
static int INDENT_STACK_SIZE=0;                /* Current no fo saved indents */
static int INDENT_POS = 0;                 	/* Current indent */
static int CURRENT_POS = 0;					/* Current print pos beyond the indent */
static int EMIT_INDENT = 1;					/* Whether wwe should emit an indent */
int FLAG_FAILURE=0;                     /* Flag failure in assert macros. */


static inline void INDENT() {
	assert(INDENT_STACK_SIZE < INDENT_STACK_MAX);
	INDENT_STACK[INDENT_STACK_SIZE++] = INDENT_POS;
	INDENT_POS += INDENT_STEP;
}
static inline void UNINDENT() {
	assert(INDENT_STACK_SIZE>0);
	INDENT_POS = INDENT_STACK[--INDENT_STACK_SIZE];
}
static inline void TAB() {
	assert(INDENT_STACK_SIZE < INDENT_STACK_MAX);
	INDENT_STACK[INDENT_STACK_SIZE++] = INDENT_POS;
	INDENT_POS += CURRENT_POS;
}


static inline const char* COLOR(const char* msg, const char* color)
{
	static char buffer[256];
	int tty = ARGS.use_color && isatty(fileno(stderr));

	snprintf(buffer,256, "%s%s%s", 
		tty? color : "", 
		msg,
		tty ? "\033[0m" : "");
	return buffer;
}


void MSG(const char* format, ...)
{
	/* put the output in a memory buffer */
	char* buffer = NULL;
	size_t buffer_size;

	FILE* output = open_memstream(&buffer, &buffer_size);
	va_list ap;
	va_start (ap, format);
	vfprintf (output, format, ap);
	va_end (ap);
	fclose(output);

	/* Now, write to stderr */
	for(size_t i=0;i<buffer_size;i++) {
		if(EMIT_INDENT) {
			for(int c=0; c< INDENT_POS; c++)
				fputc(' ', stderr);
			EMIT_INDENT = 0;
		}
		if(buffer[i]=='\n') {
			EMIT_INDENT=1;
			CURRENT_POS = 0;
		} else {
			CURRENT_POS ++;
		}
		fputc(buffer[i], stderr);
	}
	free(buffer);
}






/* 
	Execution utilities
 */



/*
    Terminal proxy.
 	---------------
     
    This class is used to test terminals. Usage in a test:
 

	term_proxy tp;
	term_proxy_init(&tp, 1);     // test proxy terminal 1
	file1 = OpenTerminal(1);     // open terminal 1

	sendme(&tp, "hello");

	char buffer[5];
	Read(file1, buffer, 5); 
	ASSERT( memcmp(buffer, "hello", 5) == 0 );


	expect(&tp, "hi there");
	Write(file1, "hi there", 8);

	term_proxy_close(&tp);     // will signal errror if expect(...) failed.
 */


/* Open a terminal fifo:  e.g.,  open_fifo("con",2) */
int open_fifo(const char* name, uint n)
{
	char fname[NAME_MAX];
	snprintf(fname, NAME_MAX, "%s%d", name, n);
	int fd;
	CHECK(fd = open(fname, O_RDWR|O_NONBLOCK));
	/* drain fifo */
	char buf[256];
	while(1) {
		int rc = read(fd, buf, 256);
		if(rc==-1) {
			if(errno==EAGAIN)
				break;
			else {
				assert(errno==EINTR);
				continue;
			}
		}
	}
	return fd;
}

struct proxy_daemon;
typedef void (*PatternProc)(struct proxy_daemon*, const char*);

/* A thread that "uses" the terminal fifos */
typedef struct proxy_daemon {
	pthread_t thread;	/* Daemon thread */
	PatternProc proc;	/* Pattern processor function */
	int complete;     	/* Flag that the VM will not access the terminal any more. */
	int fd;				/* The fd */
	rlnode pattern; 	/* The pattern list */
	pthread_mutex_t mx; /* Monitor mutex */
	pthread_cond_t pat; /* Signal that there is a new pattern, or that the VM is done. */
} proxy_daemon;

/* A proxy has a terminal and a keyboard daemon */
typedef struct term_proxy
{
	uint term;
	proxy_daemon con, kbd;
} term_proxy;


void* term_proxy_daemon(void*);

void term_proxy_daemon_init(proxy_daemon* this, const char* fifoname, uint fifono, PatternProc proc)
{
	this->proc = proc;
	this->complete = 0;
	this->fd = open_fifo(fifoname, fifono);
	rlnode_init(&this->pattern, NULL);
	CHECKRC(pthread_mutex_init(& this->mx, NULL));
	CHECKRC(pthread_cond_init(& this->pat, NULL));

	/* Create daemon thread with a full signal mask */
	sigset_t fullmask, oldmask;
	CHECK(sigfillset(&fullmask));
	CHECKRC(pthread_sigmask(SIG_SETMASK, &fullmask, &oldmask));
	CHECKRC(pthread_create(& this->thread, NULL, term_proxy_daemon, this));
	char thread_name[16];
	CHECK(snprintf(thread_name, 16, "%s%d",fifoname,fifono));
	CHECKRC(pthread_setname_np(this->thread, thread_name));

	/* Restore signal mask */
	CHECKRC(pthread_sigmask(SIG_SETMASK, &oldmask, NULL));
}

void term_proxy_daemon_add(proxy_daemon* this, const char* pattern)
{
	CHECKRC(pthread_mutex_lock(& this->mx));
	assert(pattern!=NULL);
	assert(! this->complete);
	rlnode* newnode = (rlnode*)malloc(sizeof(rlnode));
	if(newnode==NULL)  FATAL("Out of memory!");
	rlnode_init(newnode, strdup(pattern));
	rlist_push_back(& this->pattern , newnode);
	CHECKRC(pthread_cond_signal(& this->pat));
	CHECKRC(pthread_mutex_unlock(& this->mx));	
}

char* term_proxy_daemon_get(proxy_daemon* this)
{
	char* pattern = NULL;
	CHECKRC(pthread_mutex_lock(& this->mx));
	/* Wait for pattern(s) or completion */
	while( (! this->complete) && is_rlist_empty(& this->pattern) )
		CHECKRC(pthread_cond_wait(&this->pat, &this->mx));

	/* If there is a pattern, return it */
	if(! is_rlist_empty(& this->pattern)) {
		rlnode* node = rlist_pop_front(& this->pattern);
		pattern = (char*) (node->obj);
		free(node);
	}
	CHECKRC(pthread_mutex_unlock(& this->mx));
	return pattern;
}

void term_proxy_daemon_close(proxy_daemon* this)
{
	CHECKRC(pthread_mutex_lock(& this->mx));
	this->complete = 1;
	CHECKRC(pthread_cond_signal(& this->pat));
	CHECKRC(pthread_mutex_unlock(& this->mx));	
	CHECKRC(pthread_join(this->thread, NULL));	
}


void* term_proxy_daemon(void* arg)
{
	proxy_daemon* this = (proxy_daemon*)arg;
	while(1) {
		char* pattern = term_proxy_daemon_get(this); 
		if(pattern==NULL) break;
		this->proc(this, pattern);
		free(pattern);
	}
	CHECK(close(this->fd));
	return NULL;
}

void term_proxy_close(term_proxy* this)
{
	term_proxy_daemon_close(& this->con);
	term_proxy_daemon_close(& this->kbd);
}

void term_proxy_expect(term_proxy* this, const char* pattern)
{
	term_proxy_daemon_add(& this->con, pattern);
}

void term_proxy_sendme(term_proxy* this, const char* pattern)
{
	term_proxy_daemon_add(& this->kbd, pattern);
}


int term_proxy_daemon_complete(proxy_daemon* this)
{
	int complete;
	CHECKRC(pthread_mutex_lock(& this->mx));
	complete = this->complete;
	CHECKRC(pthread_mutex_unlock(& this->mx));
	return complete;
}


/* 
	Read fd and check that it matches pattern. 
	Return when there is a mismatch, or there is no available
	input and the daemon is marked 'complete'. 
*/
void con_proc(proxy_daemon* this, const char* pattern)
{
	const char* pat = pattern;
	int plen = strlen(pat);
	int patlen = plen;
	int complete = 0; 
	struct pollfd fdp = { .fd = this->fd, .events = POLLIN };

	char coninput[1024];
	int rc;

/* completion is stable, so we only need to check it if it is false */
#define COMPLETE  (complete || (complete=term_proxy_daemon_complete(this)))

	while(plen > 0) {

		/* Poll and if we are not complete poll again, for 100ms */
		int  have_data;

		not_ready:
		have_data = 0;
		do {
			int timeout = (COMPLETE)?0:100;

			poll(&fdp, 1, timeout);
			assert( (fdp.revents & (POLLERR|POLLHUP|POLLNVAL)) == 0  );
			have_data = fdp.revents & POLLIN;
		} while(! (have_data || COMPLETE ));

		if(! have_data) {
			break;
		}

		assert(have_data);

		/* Read input and check it, skipping EINTR */
		while( (rc = read(this->fd, coninput, (plen<1024)? plen : 1024)) == -1  
			&& errno==EINTR) ;

		if(rc==-1 && errno==EAGAIN)
			goto not_ready;

		CHECK(rc);  /* This is fatal on error! */
		assert(rc>0); /* We should not get rc==0 ! */
		assert(rc<=1024); /* We should not get rc>1024 ! */

		/* Mismatch ? */
		int matched = (memcmp(pat, coninput, rc) == 0);
		if(! matched) {
			break_mismatch: __attribute__((unused));				
			break;
		}

		/* ok, either we are not 'complete' or we are matched */
		pat += rc;
		plen -= rc;
	}

	checking_time: __attribute__((unused));
	ASSERT_MSG(plen==0, "Mismatched expect(\"%.20s%s\") at pos %d\n", 
		   pattern,
		   (strlen(pattern)>20)?"...":"",
		   patlen - plen
		   );

#undef COMPLETE
}

/* Write pattern to fd, check that the whole thing is written before 'complete'. */
void kbd_proc(proxy_daemon* this, const char* pattern)
{
	size_t lpattern = strlen(pattern);
	const char* pat = pattern;
	size_t lpat = lpattern;
	struct pollfd fdp = { .fd = this->fd, .events = POLLOUT };

	while(*pat != '\0') {

		/* If we are not complete, poll the fd for reading, for 100ms */
		while(! term_proxy_daemon_complete(this)) {
			poll(&fdp, 1, 100);
			assert( (fdp.revents & (POLLERR|POLLHUP|POLLNVAL)) == 0  );
			if(fdp.revents & POLLOUT) break;
		}

		/* Save complete status */
		int oldcomplete = term_proxy_daemon_complete(this);
		
		/* Write output, skipping EINTR, until EAGAIN, or input exhausted. */
		while(*pat != '\0') {
			int rc;
			while( (rc = write(this->fd, pat, lpat))==-1 && errno==EINTR );

			if(rc>0) {
				pat += rc;
				lpat -= rc;
			}
			else {
				assert(rc==-1);
				assert(errno==EAGAIN || errno==EPIPE);

				if(errno==EPIPE) {
					ASSERT_MSG(0, "The kbd fifo was closed!\n");
					abort();
				} 

				assert(errno==EAGAIN);
				if(oldcomplete) goto finish;
				break;
			}
		}

	}

finish:
	ASSERT_MSG(*pat=='\0', "Sendme(\"%.50s%s\") failed\n", 
		   pattern,
		   (lpattern>50)?"...":"");	
}


void term_proxy_init(term_proxy* this, uint term)
{
	assert(term < MAX_TERMINALS);
	this->term = term;

	/* Start the daemons */
	term_proxy_daemon_init(&this->con, "con", term, con_proc);
	term_proxy_daemon_init(&this->kbd, "kbd", term, kbd_proc);
}


term_proxy PROXY[MAX_TERMINALS];


void expect(uint term, const char* pattern)
{
	assert(term<bios_serial_ports());
	term_proxy_expect(& PROXY[term], pattern);
}

void sendme(uint term, const char* pattern)
{
	assert(term<bios_serial_ports());
	term_proxy_sendme(& PROXY[term], pattern);
}



/* Execute procfunc in a subprocess, return 
   1 if it exited normally, 0 otherwise.

   When the subprocess starts, it runs terminal proxies
   on each of the 4 terminals and makes them available to test
   code.

   The subprocess will be killed after 'timeout' seconds, if it
   has not finished already.
*/

int execute_fork(void (*procfunc)(void*), void* arg, unsigned int timeout)
{
	pid_t pid;
	sigset_t waitmask, oldmask;

	/* Block SIGCHLD only */
	CHECK(sigemptyset(&waitmask));
	CHECK(sigaddset(&waitmask, SIGCHLD));
	CHECK(sigprocmask(SIG_BLOCK, &waitmask, &oldmask));

	/* Fork */
	CHECK(pid = fork());	
	if(pid==0) {
		/* Subprocess */
		FLAG_FAILURE=0;

		procfunc(arg);

		if(FLAG_FAILURE) abort();
		exit(129);
	} 

	/* Block SIGALRM also */
	CHECK(sigaddset(&waitmask, SIGALRM));
	CHECK(sigprocmask(SIG_BLOCK, &waitmask, NULL));

	/* Set the alarm */
	alarm(timeout);

	/* Wait for a signal. If SIGCHLD arrives first, all is ok. */
	int signo;
	CHECKRC(sigwait(&waitmask, &signo));

	/* Cancel pending alarm */
	alarm(0);

	/* If the time ran out, kill the child. */
	if(signo==SIGALRM) {
		ASSERT_MSG(0, "Test timed out\n");
		kill(pid, SIGTERM);
	}

	/* Wait the child */
	int status;
	waitpid(pid, &status, 0);

	/* Restore signal mask ?*/
	sigprocmask(SIG_SETMASK, &oldmask, NULL);

	return status;
}


int execute_nofork(void (*procfunc)(void*), void* arg, unsigned int timeout)
{
	FLAG_FAILURE=0;
	/* Note: timeout is ignored, we allow the test to run forever!
	   It is the user's job to interrupt!
	 */
	procfunc(arg);
	/* Here, we could allow tests to continue */
	if(FLAG_FAILURE) {
		/* Here, we could allow tests to continue, still reporting
		   failure. However, since --nofork is most often used for
		   testing in the debugger, actually dumping the process
		   seems more useful!
	    */
		abort();
	}
	return W_EXITCODE(129,0);
}



int execute(void (*procfunc)(void*), void* arg, unsigned int timeout)
{
	if(ARGS.fork)
		return execute_fork(procfunc, arg, timeout);
	else
		return execute_nofork(procfunc, arg, timeout);
}



struct  boot_test_descriptor 
{
	int ncores;
	int nterm;
	Task bootfunc;
	int argl;
	void* args;
};


void boot_test_wrapper(void* arg) 
{
	struct boot_test_descriptor* d = arg;

	for(uint i=0;i<d->nterm; i++)
		term_proxy_init(&PROXY[i], i);

	boot(d->ncores, d->nterm, d->bootfunc, d->argl, d->args);

	for(uint i=0;i<d->nterm; i++)
		term_proxy_close(&PROXY[i]);
}


int execute_boot(int ncores, int nterm, Task bootfunc, int argl, void* args, unsigned int timeout)
{
	struct boot_test_descriptor d = { 
		.ncores=ncores, .nterm=nterm, .bootfunc=bootfunc, .argl=argl, .args=args
	};
	return execute(boot_test_wrapper, &d, timeout);
}

/* Fill in memory with a weird value:  10101010 or 0xAA */
#define FUDGE(var)  memset(&(var), 170, sizeof(var))


/*
	Test organization
 */

/* Macros for declaring static arrays of tests */


int run_boot_test(const Test* test, uint ncores, uint nterm, int argl, void* args)
{
	int result=1;
	int status;
	int skipped = ! ((ncores >= test->minimum_cores) && (nterm >= test->minimum_terminals));

	assert(test->type == BOOT_FUNC);

	if(! skipped) {
		status = execute_boot(ncores, nterm, test->boot, argl, args, test->timeout);
		result = WIFEXITED(status) && WEXITSTATUS(status)==129 ? 1 : 0;
		if(WIFSIGNALED(status))
			MSG("Test crashed, signal=%d (%s)\n", 
				WTERMSIG(status), strsignal(WTERMSIG(status)));
	}

	MSG("%-52s [cores=%2d,term=%1d]:", COLOR(test->name,WHITE), ncores, nterm);
	MSG(" %s\n", skipped ? COLOR("skipped",CYAN) : 
		   (result? COLOR("ok",GREEN) : COLOR("*** FAILED ***",RED)) );

	return result;
}



/** @internal */
typedef struct 
{
	unsigned int number_of_tests;
	unsigned int successful;
} Results;
#define RESULTS_INIT ((Results){ 0, 0 })

/* helper for run_test */
int run_suite(const char* name, const Test** tests, Results* results);


int run_test(const Test* test)
{
	int result=1;
	int status;

	switch(test->type) {
		case BOOT_FUNC:
			for(int i=0; i<ARGS.ncore_list; i++)
				for(int j=0; j<ARGS.nterm_list; j++) 
					result &= run_boot_test(test, ARGS.core_list[i], ARGS.term_list[j], 0, NULL);
			break;
		case BARE_FUNC:
			status = execute(test->bare, NULL, test->timeout);

			result = WIFEXITED(status) && WEXITSTATUS(status)==129  ? 1 : 0;
			if(WIFSIGNALED(status))
				MSG("Test crashed, signal=%d (%s)\n", 
					WTERMSIG(status), strsignal(WTERMSIG(status)));

			MSG("%-70s:", COLOR(test->name,WHITE));
			MSG(" %s\n", (result? COLOR("ok",GREEN) : COLOR("*** FAILED ***",RED)));

			break;
		case SUITE_FUNC:
			result = run_suite(test->name, test->suite, NULL);

			MSG("%-70s:", COLOR(test->name,WHITE));
			MSG(" %s\n", (result? COLOR("ok",GREEN) : COLOR("*** FAILED ***",RED)));

			break;
		case NO_FUNC:
			return 1;		
		default:
			MSG("Internal error: Test: %s: Unknown test type: %d\n", test->name, test->type);
			return 0;
	}


	if(!result && ARGS.verbose>0) {
		/* print doc of failed test */
		INDENT(); 
		MSG("description: ");
		TAB(); MSG("%s\n", test->description); UNINDENT();
		UNINDENT();
	}

	return result;
}

int run_suite(const char* name, const Test** tests, Results* results)
{
	if(results==NULL) {
		Results dummy = RESULTS_INIT;
		return run_suite(name, tests, &dummy);
	}

	MSG("running suite: %s\n", COLOR(name,YELLOW));
	INDENT();
	for(const Test** t = tests; *t!=NULL; t++) {
		int testres = run_test(*t);
		results->number_of_tests ++;
		if(testres) results->successful ++;
	}
	MSG("suite %s completed [tests=%d, failed=%d]\n", COLOR(name,YELLOW), 
		results->number_of_tests, 
		results->number_of_tests - results->successful
		);
	UNINDENT();
	return results->number_of_tests==results->successful;
}






/*
	Testing the test framework itself!
 */


BARE_TEST(internal_success,
	"A test that succeeds."
	)
{
	ASSERT(1);
}

BARE_TEST(internal_failure,
	"A test that fails by assertion."
	)
{
	ASSERT(0);
}

BARE_TEST(internal_timeout,
	"A test that fails by timeout.",
	.timeout = 1
	)
{
	sleep(5);
}

BARE_TEST(internal_skip,
	"A test that is skipped",
	.minimum_terminals = UINT_MAX
	)
{

}

TEST_SUITE(internal,
	"A suite of internal tests, testing the test framework itself."
	)
{
	&internal_success,
	&internal_failure,
	&internal_timeout,
	&internal_skip,
	NULL
};


/*
  User tests go into the user_tests suite
  */


extern const Test user_tests;

/* This is the number of nulls in all_tests_available */
#define MAX_TESTS_AVAILABLE 64
static int current_tests_available = 0;

TEST_SUITE(all_tests_available,"Used by find_test()")
{
	//&internal,
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 
	NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 
	NULL
};

int register_test(const Test* test)
{
	if(current_tests_available < MAX_TESTS_AVAILABLE)
	{
		__suite_all_tests_available[current_tests_available++] = test;
		return 0;
	}
	return -1;
}




/*
	Main: arguments
*/

const char *argp_program_version =
  "validate_api 2.1";
const char *argp_program_bug_address =
  "<vsam@softnet.tuc.gr>";

/* Program documentation. */
static char doc[] =
  "A testing and validation program for the tinyos2 API.\n"
  "\vUse this program, by providing a list of cores, a list of terminals and "
  "a list of tests. All arguments are optional. "
  "For example,\n\n   ./validate_api -c 1,2,4  --term=0,2  basic_tests\n\n"
  "will execute each test in the 'basic_tests' suite, on 6 different virtual machines, "
  "one for each combination of the number of cores and number of terminals."
  ;


/* A description of the arguments we accept. */
static char args_doc[] = " TEST  ... ";




static struct argp_option options [] = {
	{"cores", 'c', "<cores>", 0, "List of number of cores" },
	{"nofork", 'f', 0, 0, "Don't fork tests to a different process" },
	{"fork", 'F', 0, 0, "Force fork for tests to a different process"},
	{"term", 't', "<terminals>", 0, "List of number of terminals" },
	{"list", 'l', 0, 0, "Show a list of available tests" },
	{"verbose", 'v', 0, 0, "Be verbose: show test descriptions"},
	{"nocolor", 'n', 0, 0, "Do not color the output"},
	{ NULL }
};



static const struct Test* find_test(const char* name, const struct Test* test)
{
	if(strcmp(name, test->name)==0)
		return test;
	else if(test->type == SUITE_FUNC) {
		for(const struct Test** T = test->suite; *T !=NULL; T++ ) {
			const struct Test* t = find_test(name, *T);
			if(t!=NULL) return t;
		}
	} 
	return NULL;
}


/* use this comparator for sorting the numbers */
static int cmpint(const void* ap, const void* bp) {
	return *(int*)bp < *(int*)ap;
}


/* 
	Parse a comma-separated list of small integers, such that 
  	each integer n is n>=from and n<=to. 
	
  	If sucessful, sort and store the values starting at nlist, 
  	whose length must be big enough (up to to-from+1), but might
  	be smaller. Duplicate values are only stored once. 

  	On failure (malformed input or out-of-range), do 
  	not disturb the data pointed by nlist. 

  	Return 1 for success and 0 for failure.
 */
static int parse_int_list(char* arg, int* nlen, int* nlist, int from, int to)
{
	int args[to-from+1];  /* Temporary storage of data */
	int i=0;

	for(char* token=strtok(arg,","); token!=NULL; token=strtok(NULL,",")) 
	{
		char* endptr;
		int num = strtol(token, &endptr, 10 );

		/* fail on malformed or out-of-range input */
		if(endptr==token || *endptr!='\0') return 0;
		if(num<from || num > to) return 0;

		/* check for duplicate */
		int j;
		for(j=0; j<i; j++) 
			if(num==nlist[j]) break;
		if(j < i) continue; /* Duplicates are not an error */

		/* store new value */
		args[i++] = num; 
	}



	/*  Now sort, copy and exit */
	qsort(args, i, sizeof(int), cmpint);

	*nlen = i;
	nlist = memcpy(nlist, args, i*sizeof(int));

	return *nlen > 0;
}


static const Test* __default_test;

static error_t
parse_options(int key, char *arg, struct argp_state *state)
{
	const struct Test* test;

	switch(key)
	{
		case 'l':
			ARGS.show_tests = 1;
			break;

		case 'v':
			ARGS.verbose ++;
			break;

		case 'n':
			ARGS.use_color = 0;
			break;

		case 'F':
			ARGS.fork = 1;
			break;

		case 'f':
			ARGS.fork = 0;
			break;

		case 'c':
			if(! parse_int_list(arg, &ARGS.ncore_list, ARGS.core_list, 1, MAX_CORES))
				argp_error(state, "Error in parsing list of cores: %s\n",arg);				
			break;

		case 't':
			if(! parse_int_list(arg, &ARGS.nterm_list, ARGS.term_list, 0, MAX_TERMINALS))
				argp_error(state, "Error in parsing list of terminals: %s\n",arg);				
			break;

		case ARGP_KEY_ARG:
			if(ARGS.ntests >= MAX_TESTS) {
				argp_error(state, "Number of tests too large (maximum=%d)",MAX_TESTS);
				break;				
			}

			test = find_test(arg, &all_tests_available);

			if(test!=NULL)
				ARGS.tests[ARGS.ntests++] = test;
			else 
				argp_error(state, "Unknown test: %s\n",arg);
			break;

		case ARGP_KEY_NO_ARGS:
			ARGS.tests[ARGS.ntests++] = __default_test;
			break;

#if 0
		/* For future versions */
		case ARGP_KEY_ARGS:
			fprintf(stderr,"ARGS arg=%s\n", arg);
			break;

		case ARGP_KEY_INIT:
			fprintf(stderr,"INIT arg=%s\n", arg);
			break;

		case ARGP_KEY_END:
			fprintf(stderr,"END arg=%s\n", arg);
			break;
#endif
	    default:
      		return ARGP_ERR_UNKNOWN;
	}

	return 0;
}


static struct argp argp = { options, parse_options, args_doc, doc };



void show_suite(const Test*);

void show_test(const Test* test)
{
	if(test->type == SUITE_FUNC)
		show_suite(test);
	else {
		MSG("%-40s\n", COLOR(test->name, WHITE));
		if(ARGS.verbose>0) {
			INDENT();
			MSG("%s\n", test->description);
			if(ARGS.verbose > 1) {
				MSG(".timeout = %d sec\n", test->timeout);
				MSG(".minimum cores = %d\n.minimum terminals = %d\n", 
					test->minimum_cores, test->minimum_terminals);
			}
			UNINDENT();
		}
	}
}


void show_suite(const Test* suite)
{
	MSG("%-40s\n", COLOR(suite->name, YELLOW));
	INDENT();
	if(ARGS.verbose) {
		MSG("%s\n", suite->description);		
	}
	for(const Test** test = suite->suite; *test!=NULL; test++) {

		show_test(*test);
	}
	UNINDENT();
}



int run_program(int argc, char**argv, const Test* default_test)
{
	__default_test = default_test;
	ARGS.fork = ! isDebuggerAttached();
	argp_parse(&argp, argc, argv, 0, 0, &ARGS);

	if(ARGS.show_tests)
		show_suite(&all_tests_available);
	else {
				for(int k=0; k< ARGS.ntests; k++)
					run_test(ARGS.tests[k]);
	}
	return 0;
}


/*
	Taken from 
	https://stackoverflow.com/questions/3596781/how-to-detect-if-the-current-process-is-being-run-by-gdb

	Comments: Linux-specific
*/
int isDebuggerAttached()
{
    char buf[4096];

    const int status_fd = open("/proc/self/status", O_RDONLY);
    if (status_fd == -1)
        return 0;

    const ssize_t num_read = read(status_fd, buf, sizeof(buf) - 1);
    if (num_read <= 0)
        return 0;

    buf[num_read] = '\0';
    const char tracerPidString[] = "TracerPid:";
    char* tracer_pid_ptr = strstr(buf, tracerPidString);
    if (!tracer_pid_ptr)
        return 0;

    for (const char* characterPtr = tracer_pid_ptr + sizeof(tracerPidString) - 1; 
    	characterPtr <= buf + num_read; ++characterPtr)
    {
        if (isspace(*characterPtr))
            continue;
        else
            return isdigit(*characterPtr) != 0 && *characterPtr != '0';
    }

    return 0;
}
