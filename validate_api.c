
#include <assert.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <math.h>
#include <setjmp.h>

#include "util.h"
#include "symposium.h"
#include "tinyoslib.h"
#include "unit_testing.h"


/*
 *
 *   TESTS
 *
 */


/*
	test_boot

	Test that the boot function executes the boot task and returns.
 */

struct test_cpu_rec {
	uint ncores;
	uint core;
	uint nterm;
	int argl;
	void* args;
	struct test_cpu_rec* rec;
};


int test_boot_boot(int argl, void* args) {
	typedef struct test_cpu_rec* REC;
	ASSERT(argl==sizeof(REC));
	REC rec;
	memcpy(&rec, args, argl);
	rec->rec = rec;
	rec->args = args;
	rec->argl = argl;
	rec->ncores = cpu_cores();
	rec->core = cpu_core_id;
	rec->nterm = bios_serial_ports();
	return 0;
}

BARE_TEST(test_boot, 
	"Test that the boot(...) function initializes the VM\n"
	"and passes arguments to the init task correctly.")
{
	struct test_cpu_rec cpu_rec;
	struct test_cpu_rec* cpu_rec_ptr = &cpu_rec;

	FUDGE(cpu_rec);
	boot(1,0, test_boot_boot, sizeof(cpu_rec_ptr), &cpu_rec_ptr);

	ASSERT(cpu_rec.argl == sizeof(cpu_rec_ptr));
	ASSERT( (struct test_cpu_rec **)cpu_rec.args != &cpu_rec_ptr);
	ASSERT( cpu_rec.rec == &cpu_rec );
	ASSERT(cpu_rec.nterm == 0);

	ASSERT(cpu_rec.ncores == 1);
	ASSERT(cpu_rec.core == 0);
}




/*********************************************
 *
 *
 *
 *  Process tests
 *
 *
 *
 *********************************************/



/*
	Test that the child process created, gets the same pid as the
	parent got returned from exec.
 */


BOOT_TEST(test_pid_of_init_is_one, 
	"Test that the pid of the init task is 1. This may\n"
	"not be according to spec, but this is something\n"
	"we will correct in the next update."
	)
{
	ASSERT(GetPid()==1);
	return 0;
}



static void waitchild_error()
{
	/* Cannot wait on myself */
	ASSERT(WaitChild(GetPid(),NULL)==NOPROC);
	ASSERT(WaitChild(MAX_PROC, NULL)==NOPROC);
	ASSERT(WaitChild(GetPid()+1, NULL)==NOPROC);
}
static int subprocess(int argl, void* args) 
{
	ASSERT(GetPid()!=1);
	waitchild_error();
	return 0;
}


BOOT_TEST(test_waitchild_error_on_invalid_pid,
	"Test that WaitChild returns an error when the pid is invalid."
	)
{
	waitchild_error();
	Pid_t cpid = Exec(subprocess, 0, NULL);
	ASSERT(WaitChild(NOPROC, NULL)==cpid);
	return 0;
}


static int void_child(int argl, void* args) { return 0; }

static int bad_child(int argl, void* args)
{
	Pid_t cpid = *(Pid_t*)args;
	ASSERT(WaitChild(cpid, NULL)==NOPROC);
	return 0;
}


BOOT_TEST(test_waitchild_error_on_nonchild,
	"Test that WaitChild returns an error when the process is not\n"
	"its child."
	)
{
	
	Pid_t cpid = Exec(void_child, 0, NULL);
	Pid_t badpid = Exec(bad_child, sizeof(cpid), &cpid);

	ASSERT(badpid != NOPROC);
	ASSERT(WaitChild(badpid, NULL)==badpid);
	ASSERT(WaitChild(cpid, NULL)==cpid);
	return 0;
}


/* used to pass information to parent */
struct test_pid_rec {
	Pid_t pid;
	int level;
};


BOOT_TEST(test_exec_getpid_wait, 
	"Test that Exec returns the same pid as the child sees\n"
	"by calling GetPid(). Also, that WaitChild with a given pid\n"
	"returns the correct status.",
	.timeout=20
	)
{
	struct test_pid_rec  myrec;  /* only used by init task */
	struct test_pid_rec* prec;

	if(argl==0) {
		ASSERT(GetPid()==1);
		prec = &myrec;
		prec->level = 7;      /* 4^7 = 2^14 = 16384 children will be run */
	} else {
		ASSERT(argl==sizeof(struct test_pid_rec*));
		prec = *(struct test_pid_rec**)args;
	}
	prec->pid = GetPid();

	if(prec->level>0) {
		for(int i=0;i<3;i++) {
			/* Prepare rec for child */
			struct test_pid_rec rec;
			rec.level = prec->level - 1;
			/* Exec child */
			struct test_pid_rec* arg = &rec;
			Pid_t cpid = Exec(test_exec_getpid_wait.boot, sizeof(arg), &arg);
			ASSERT(cpid != NOPROC);
			/* Wait for child and verify */
			if(cpid != NOPROC) {
				int status;
				Pid_t wpid = WaitChild(cpid, &status);
				ASSERT(wpid==cpid);
				ASSERT(status==cpid);
			}
		}
	}
	return GetPid();
}


static int copyarg_child(int argl, void* args)
{
	*(int*)args = 1;
	return 0;
}


BOOT_TEST(test_exec_copies_arguments,
	"Test that Exec creates of copy of the arguments of the new process."
	)
{

	Pid_t cpid;
	int value = 0;
	ASSERT((cpid = Exec(copyarg_child, sizeof(value), &value))!=NOPROC);
	WaitChild(cpid, NULL);
	ASSERT(value==0);
	return 0;
}


BOOT_TEST(test_wait_for_any_child, 
	"Test WaitChild when called to wait on any child."
	)
{
#define NCHILDREN 5
#define NLEVELS 3
	struct test_pid_rec myrec;
	struct test_pid_rec* prec;
	if(argl==0) {
		ASSERT(GetPid()==1);
		prec = &myrec;
		prec->level = NLEVELS;
	} else {
		prec = *(struct test_pid_rec**)args;
		prec->pid = GetPid();
	}

	if(prec->level>0) {
		struct test_pid_rec rec[NCHILDREN];

		/* Test many execs */
		for(int i=0; i<NCHILDREN; i++) {
			struct test_pid_rec* arg = &rec[i];
			rec[i].level = prec->level - 1;
			Pid_t cpid = Exec(test_wait_for_any_child.boot, sizeof(arg), &arg);
			ASSERT(cpid!=NOPROC);
		}

		for(int i=0; i<NCHILDREN; i++) {
			Pid_t cpid = WaitChild(NOPROC, NULL);
			ASSERT(cpid != NOPROC);

			/* try to find cpid in array */
			int j;
			for(j=0; j<NCHILDREN;j++) 
				if(rec[j].pid == cpid) break;
			ASSERT(j < NCHILDREN);
			rec[j].pid = NOPROC;  /* Reset it so we don't find it again! */
		}

		ASSERT(WaitChild(NOPROC, NULL)==NOPROC);
	}
	return 0;
#undef NCHILDREN
#undef NLEVELS
}


int exiting_child(int arg, void* args) {
	Exit(GetPid());
	ASSERT(0);
	return 1;
}


BOOT_TEST(test_exit_returns_status,
	"Test that the exit status is returned by Exit"
	)
{
	Pid_t children[100];
	for(int i=0;i<100;i++)
		children[i] = Exec(exiting_child, 0, NULL);

	for(int i=0;i<100;i++) {
		int status;
		WaitChild(children[i], &status);
		ASSERT(status==children[i]);
	}
	return 0;
}


static int pid_returning_child(int arg, void* args) {
	return GetPid();
}


BOOT_TEST(test_main_return_returns_status,
	"Test that the exit status is returned by return from main task"
	)
{
	const int N=10;
	Pid_t children[N];
	for(int i=0;i<N;i++)
		children[i] = Exec(pid_returning_child, 0, NULL);
	for(int i=0;i<N;i++) {
		int status;
		WaitChild(children[i], &status);
		ASSERT(status==children[i]);
	}
	return 0;
}


static int orphan_grandchild(int argl, void* args)
{
	return 1;
}
static int dying_child(int arg, void* args)
{
	for(int i=0;i<5;i++)
		ASSERT(Exec(orphan_grandchild, 0, NULL)!=NOPROC);
	return 100;
}


BOOT_TEST(test_orphans_adopted_by_init,
	"Test that when a process exits leaving orphans, init becomes the new parent."
	)
{

	for(int i=0;i<3; i++)
		ASSERT(Exec(dying_child,0,NULL)!=NOPROC);


	/* Now wait for 18 children (3 child + 15 grandchild) */
	int sum = 0;
	for(int i=0;i<18;i++) {
		int status;		
		ASSERT(WaitChild(NOPROC, &status) != NOPROC);
		sum += status;
	}
	

	/* Check that we have no more */
	ASSERT(WaitChild(NOPROC, NULL) == NOPROC);

	ASSERT(sum == 315);

	return 0;
}



/*********************************************
 *
 *
 *
 *  Synchronization tests
 *
 *
 *
 *********************************************/


/*
	Test that a timed wait on a condition variable terminates after the timeout.
 */

static unsigned long tspec2msec(struct timespec t)
{
	return 1000ul*t.tv_sec + t.tv_nsec/1000000ul;
}


static int do_timeout(int argl, void* args) {
	timeout_t t = *((timeout_t *) args);

	Mutex mx = MUTEX_INIT;
	CondVar cv = COND_INIT;

	struct timespec t1, t2;
	clock_gettime(CLOCK_REALTIME, &t1);

	Mutex_Lock(&mx);
	Cond_TimedWait(&mx, &cv, t);

	clock_gettime(CLOCK_REALTIME, &t2);

	unsigned long Dt = tspec2msec(t2)-tspec2msec(t1);

	/* Allow a large, 20% error */
	ASSERT(abs(Dt-t)*5 <= Dt);

	return 0;
}

BOOT_TEST(test_cond_timedwait_timeout, 
	"Test that timed waits on a condition variable terminate without blocking after the timeout."
	)
{

	for(timeout_t t=500; t < 1000; t+=100) {
		Exec(do_timeout, sizeof(t), &t);
	}
	for(timeout_t t=550; t < 1000; t+=100) {
		Exec(do_timeout, sizeof(t), &t);
	}

	/* 
		Wait all child processes, before leaving the current stack frame!
		Else, the local functions may cause a crash!
	*/
	while(WaitChild(NOPROC,NULL)!=NOPROC);
	return 0;
}


/*
	Test that a timed wait on a condition variable terminates at a signal.
 */

struct long_blocking_args {
	Mutex* m;
	CondVar* cv;
	CondVar* pcv;
	int* flag;
};

static int long_blocking(int argl, void* args)
{
	struct long_blocking_args A = *(struct long_blocking_args*)args;

	Mutex_Lock(A.m);
	* A.flag = 1;
	Cond_Signal(A.cv);
	Cond_TimedWait(A.m, A.cv, 10000000); // 3 hour wait
	Mutex_Unlock(A.m);
	return 0;
}

BOOT_TEST(test_cond_timedwait_signal,
	"Test that timed waits on a condition variable terminates immediately on signal."
	)
{
	Mutex m = MUTEX_INIT;
	CondVar cv = COND_INIT;
	int flag=0;

	struct long_blocking_args A = {.m = &m, .cv=&cv, .flag=&flag};
	
	Pid_t child = Exec(long_blocking, sizeof(A), &A);

	Mutex_Lock(&m);
	while(! flag)
		Cond_Wait(&m, &cv);
	Cond_Signal(&cv);
	Mutex_Unlock(&m);

	WaitChild(child, NULL);
	return 0;
}


static int long_blocking2(int argl, void* args)
{
	struct long_blocking_args A = *(struct long_blocking_args*)args;
	Mutex_Lock(A.m);
	(* A.flag) ++;
	Cond_Signal(A.pcv);
	Cond_TimedWait(A.m, A.cv, 10000000); // 3 hour wait
	Mutex_Unlock(A.m);
	return 0;
}



BOOT_TEST(test_cond_timedwait_broadcast,
	"Test that timed waits on a condition variable terminate immediately on broadcast."
	)
{
	Mutex m = MUTEX_INIT;
	CondVar cv = COND_INIT;
	CondVar pcv = COND_INIT;
	int flag=0;

	const int N=100;  // spawn 100 children

	struct long_blocking_args A = {.m=&m, .cv=&cv, .pcv=&pcv, .flag=&flag };

	// create N children
	for(int i=0; i<N; i++) Exec(long_blocking2, sizeof(A), &A);

	Mutex_Lock(&m);
	// wait for all children to sleep
	while(flag!=N) Cond_Wait(&m, &pcv);
	// wake all children up!
	Cond_Broadcast(&cv);
	Mutex_Unlock(&m);

	// wait all children
	while(WaitChild(NOPROC, NULL)!=NOPROC);
	return 0;
}



/*********************************************
 *
 *
 *
 *  I/O  tests
 *
 *
 *
 *********************************************/




BOOT_TEST(test_get_terminals,
	"Test that the number returned by GetTerminalDevices() is equal to the\n"
	"number of serial ports in the VM."
	)
{
	ASSERT(bios_serial_ports()==GetTerminalDevices());
	return 0;
}

BOOT_TEST(test_dup2_error_on_nonfile,
	"Test that Dup2 will return an error if oldfd is not a file.")
{
	for(Fid_t fid = 0; fid < MAX_FILEID; fid++)
		ASSERT(Dup2(fid, MAX_FILEID-1-fid)==-1);
	return 0;
}

BOOT_TEST(test_dup2_error_on_invalid_fid,
	"Test that Dup2 returns error when some fid is invalid."
	)
{
	ASSERT(Dup2(NOFILE, 3)==-1);
	ASSERT(Dup2(MAX_FILEID, 3)==-1);
	Fid_t fid = OpenNull(0);
	assert(fid!=NOFILE);
	ASSERT(Dup2(fid, NOFILE)==-1);
	ASSERT(Dup2(fid, MAX_FILEID)==-1);		
	return 0;
}


BOOT_TEST(test_open_terminals,
	"Test that every legal terminal can be opened."
	)
{
	Fid_t term[MAX_TERMINALS];
	for(uint i=0; i<GetTerminalDevices(); i++) {
		term[i] = OpenTerminal(i);
		ASSERT(term[i]!=NOFILE);
	}
	return 0;
}


BOOT_TEST(test_close_error_on_invalid_fid,
	"Test that Close returns error on invalid fid."
	)
{
	ASSERT(Close(NOFILE)==-1);
	ASSERT(Close(MAX_FILEID)==-1);
	return 0;
}

BOOT_TEST(test_close_success_on_valid_nonfile_fid,
	"Test that Close returns success on valid fid, even if there is no\n"
	"open file for this id."
	)
{
	for(Fid_t i=0; i<MAX_FILEID; i++)
		ASSERT(Close(i)==0);
	return 0;
}

BOOT_TEST(test_close_terminals,
	"Test that terminals can be opened and then closed without error."
	)
{
	Fid_t term[MAX_TERMINALS];
	for(uint i=0; i<GetTerminalDevices(); i++) {
		term[i] = OpenTerminal(i);
		ASSERT(term[i]!=NOFILE);
	}
	for(uint i=0; i<GetTerminalDevices(); i++) {
		ASSERT(Close(term[i])==0);
	}	
	return 0;	
}



void checked_read(Fid_t fid, const char* message)
{
	int mlen = strlen(message);
	char buffer[mlen];
	ASSERT(Read(fid, buffer, mlen)==mlen);
	ASSERT(memcmp(buffer, message, mlen)==0);
}



BOOT_TEST(test_read_kbd,
	"Test that we can read a few bytes from the keyboard on terminal 0.",
	.minimum_terminals = 1
	)
{
	assert(GetTerminalDevices()>0);
	Fid_t fterm = OpenTerminal(0);
	ASSERT(fterm!=NOFILE);

	sendme(0, "Hello");
	checked_read(fterm, "Hello");
	return 0;
}


BOOT_TEST(test_read_kbd_big,
	"Test that we can read massively from the keyboard on terminal 0.",
	.minimum_terminals = 1, .timeout = 20
	)
{
	assert(GetTerminalDevices()>0);
	Fid_t fterm = OpenTerminal(0);
	ASSERT(fterm!=NOFILE);

	char bytes[1025];
	FUDGE(bytes);
	bytes[1024]='\0';

	/* send me 1Mbyte */
	for(int i=0; i<1024; i++)
		sendme(0, bytes);

	/* Read 16kb bytes at a time */
	char buffer[16384];
	uint count = 0;
	uint total = 1<<20;	
	while(count < total)
	{
		int remain = total-count;

		int rc = Read(fterm, buffer, (remain<16384)? remain: 16384);
		ASSERT(rc>0);
		count += rc;
	}

	return 0;
}


BOOT_TEST(test_dup2_copies_file,
	"This test copies that Dup2 copies the file to another file descriptor.",
	.minimum_terminals = 1
	)
{
	Fid_t fterm = OpenTerminal(0);
	ASSERT(fterm!=NOFILE);

	if(fterm!=0) {
		ASSERT(Dup2(fterm, 0)==0);
		Close(fterm);		
	}

	sendme(0, "zavarakatranemia");

	ASSERT(Dup2(0,1)==0);
	ASSERT(Dup2(0,2)==0);
	ASSERT(Dup2(0,3)==0);
	ASSERT(Dup2(0,4)==0);

	checked_read(1, "zava");
	checked_read(3, "raka");
	checked_read(2, "trane");
	checked_read(4, "mia");

	return 0;
}


BOOT_TEST(test_read_error_on_bad_fid,
	"Test that Read will return an error when called on a bad fid"
	)
{
	char buffer[10];
	ASSERT(Read(0, buffer, 10)==-1);
	return 0;
}


BOOT_TEST(test_read_from_many_terminals,
	"Test that Read can read from all terminals",
	.minimum_terminals = 2
	)
{
	Fid_t term[MAX_TERMINALS];
	for(uint i = 0; i < GetTerminalDevices(); i++) {
		term[i] = OpenTerminal(i);
		ASSERT(term[i]!=NOFILE);
	}

	for(uint i = 0; i < GetTerminalDevices(); i++) {
		char message[32];
		sprintf(message, "This is terminal %d", i);

		sendme(i, message);
	}
	for(uint i = 0; i < GetTerminalDevices(); i++) {
		char message[32];
		sprintf(message, "This is terminal %d", i);

		checked_read(term[i], message);
	}


	return 0;
}


static int greeted_child(int argl, void* args)
{
	checked_read(0, "Hello child");
	checked_read(0, "Hello again");
	return 0;
}

BOOT_TEST(test_child_inherits_files,
	"Test that a child process inherits files.",
	.minimum_terminals = 1
	)
{
	Fid_t fterm = OpenTerminal(0);
	ASSERT(fterm!=NOFILE);
	if(fterm!=0)
		ASSERT(Dup2(fterm, 0)==0);

	sendme(0, "Hello child");
	Pid_t cpid = Exec(greeted_child, 0, NULL);
	ASSERT(Close(0)==0);
	ASSERT(Close(fterm)==0);
	sendme(0, "Hello again");
	ASSERT(cpid!=NOPROC);
	ASSERT(WaitChild(NOPROC, NULL)==cpid);
	return 0;
}




BOOT_TEST(test_null_device,
	"Test the null device."
	)
{

	void test_read(Fid_t fid)
	{
		char z[] = "zavarakatranemia";
		char z1[] = "\0\0\0\0\0\0\0\0\0\0anemia";
		ASSERT(Read(fid, z, 10)==10);
		ASSERT(memcmp(z,z1, 17)==0);
	}

	Fid_t fn = OpenNull();
	ASSERT(fn!=-1);

	test_read(fn);
	ASSERT(Write(fn, NULL, 123456)==123456);

	ASSERT(Close(fn)==0);
	return 0;
}



/***********************************************************************************8
*************************************************/




void checked_write(Fid_t fid, const char* message)
{
	int mlen = strlen(message);
	for(int count=0; count < mlen;) {
		int wno = Write(fid, message+count, 1);
		ASSERT(wno>0);
		count += wno;
	}
}



BOOT_TEST(test_write_con,
	"Test that we can write a few bytes to the console on terminal 0.",
	.minimum_terminals = 1
	)
{
	assert(GetTerminalDevices()>0);
	Fid_t fterm = OpenTerminal(0);
	ASSERT(fterm!=NOFILE);

	expect(0, "Hello");
	checked_write(fterm, "Hello");
	return 0;
}


BOOT_TEST(test_write_con_big,
	"Test that we can write massively to the console on terminal 0.",
	.minimum_terminals = 1
	)
{
	assert(GetTerminalDevices()>0);
	Fid_t fterm = OpenTerminal(0);
	ASSERT(fterm!=NOFILE);

	char bytes[1025];
	FUDGE(bytes);
	bytes[1024]='\0';

	/* send me 1Mbyte */
	for(int i=0; i<1024; i++)
		expect(0, bytes);

	/* Create a 16kb block */
	char buffer[16384];
	FUDGE(buffer);

	int total = 1<<20;
	int count = 0;

	while(count < total)
	{
		int remain = total-count;

		int rc = Write(fterm, buffer, (remain<16384)? remain: 16384);
		ASSERT(rc>0);
		count += rc;
	}

	return 0;
}



BOOT_TEST(test_write_error_on_bad_fid,
	"Test that Write will return an error when called on a bad fid"
	)
{
	/* The compiler is a bit overzealous here, so we are forced to suppress the
	warning */
#pragma GCC diagnostic push                             // save the actual diag context
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized" 	
	char buffer[10];
	ASSERT(Write(0, buffer, 10)==-1);
#pragma GCC diagnostic pop
	return 0;
}


BOOT_TEST(test_write_to_many_terminals,
	"Test that Write can send to all terminals",
	.minimum_terminals = 2
	)
{
	Fid_t term[MAX_TERMINALS];
	for(uint i = 0; i < GetTerminalDevices(); i++) {
		term[i] = OpenTerminal(i);
		ASSERT(term[i]!=NOFILE);
	}

	for(uint i = 0; i < GetTerminalDevices(); i++) {
		char message[32];
		sprintf(message, "This is terminal %d", i);

		expect(i, message);
	}
	for(uint i = 0; i < GetTerminalDevices(); i++) {
		char message[32];
		sprintf(message, "This is terminal %d", i);

		checked_write(term[i], message);
	}


	return 0;
}




TEST_SUITE(basic_tests, 
	"A suite of basic tests, focusing on the functional behaviour of the\n"
	"tinyos3 API, but not the operational (concurrency and I/O multiplexing)."
	)
{
	&test_boot,
	&test_pid_of_init_is_one,
	&test_waitchild_error_on_nonchild,
	&test_waitchild_error_on_invalid_pid,
	&test_exec_getpid_wait,
	&test_exec_copies_arguments,
	&test_exit_returns_status,
	&test_main_return_returns_status,
	&test_wait_for_any_child,
	&test_orphans_adopted_by_init,
	&test_cond_timedwait_timeout,
	&test_cond_timedwait_signal,
	&test_cond_timedwait_broadcast,
	&test_null_device,
	&test_get_terminals,
	&test_open_terminals,
	&test_dup2_error_on_nonfile,
	&test_dup2_error_on_invalid_fid,
	&test_dup2_copies_file,
	&test_close_error_on_invalid_fid,
	&test_close_success_on_valid_nonfile_fid,
	&test_close_terminals,
	&test_read_kbd,
	&test_read_kbd_big,
	&test_read_error_on_bad_fid,
	&test_read_from_many_terminals,
	&test_write_con,
	&test_write_con_big,
	&test_write_error_on_bad_fid,
	&test_write_to_many_terminals,
	&test_child_inherits_files,
	NULL
};



/*********************************************
 *
 *
 *
 *  Thread tests
 *
 *
 *
 *********************************************/


void sleep_thread(int sec) {
	Mutex mx = MUTEX_INIT;
	CondVar cond = COND_INIT;

	Mutex_Lock(&mx);
	ASSERT(Cond_TimedWait(&mx,&cond,1000*sec)==0);
}

/* 
	Helper that spawns a process, waits for its completion
	and returns its status.
 */

int run_get_status(Task task, int argl, void* args)
{
	Pid_t pid = Exec(task, argl, args);
	ASSERT(pid!=NOPROC);

	int exitval;
	ASSERT(WaitChild(pid, &exitval)==pid);

	return exitval;
}


BOOT_TEST(test_threadself,
	"Test that ThreadSelf is somewhat sane")
{
	ASSERT(ThreadSelf() != NOTHREAD);
	ASSERT(ThreadSelf() == ThreadSelf());
	return 0;
}



BOOT_TEST(test_join_illegal_tid_gives_error,
	"Test that ThreadJoin rejects an illegal Tid")
{
	int* illegal_ptr = (int*) -1;

	ASSERT(ThreadJoin(NOTHREAD, illegal_ptr)==-1);

	/* Test with random numbers. Since we only have one thread, any call is an illegal call. */
	for(int i=0; i<100; i++) {
		Tid_t random_tid = lrand48();
		ASSERT(ThreadJoin(random_tid, illegal_ptr)==-1);
	}

	/* In addition, we cannot join ourselves ! */
	ASSERT(ThreadJoin(ThreadSelf(), illegal_ptr)==-1);

	return 0;
}


BOOT_TEST(test_detach_illegal_tid_gives_error,
	"Test that ThreadDetach rejects an illegal Tid")
{
	ASSERT(ThreadDetach(NOTHREAD)==-1);

	/* Test with random numbers. Since we only have one thread, any call is an illegal call. */
	for(int i=0; i<100; i++) {
		Tid_t random_tid = lrand48();
		if(random_tid==ThreadSelf()) /* Very unlikely, but still... */
			continue;
		ASSERT(ThreadDetach(random_tid)==-1);
	}

	return 0;
}



static int create_join_thread_flag;

static int create_join_thread_task(int argl, void* args) {
	ASSERT(args == &create_join_thread_flag);
	*(int*)args = 1;
	return 2;
}


BOOT_TEST(test_create_join_thread,
	"Test that a process thread can be created and joined. Also, that "
	"the argument of the thread is passed correctly."
	)
{
	create_join_thread_flag = 0;

	Tid_t t = CreateThread(create_join_thread_task, sizeof(create_join_thread_flag), &create_join_thread_flag);

	/* Success in creating thread */
	ASSERT(t!=NOTHREAD);
	int exitval;
	
	/* Join should succeed */
	ASSERT(ThreadJoin(t, &exitval)==0);

	/* Exit status should be correct */
	ASSERT(exitval==2);

	/* Shared variable should be updates */
	ASSERT(create_join_thread_flag==1);

	/* A second Join should fail! */
	ASSERT(ThreadJoin(t, NULL)==-1);


	return 0;
}

BOOT_TEST(test_detach_self,
	"Test that a thread can detach itself")
{
	ASSERT(ThreadDetach(ThreadSelf())==0);
	return 0;
}






static int tdo_thread(int argl, void* args) {
	fibo(40);
	return 100;
}

/* The detached thread will finish last */
static int myproc(int argl, void* args) {
	Tid_t t = CreateThread(tdo_thread, 0, NULL);
	ASSERT(ThreadDetach(t)==0);
	ASSERT(ThreadJoin(t, NULL)==-1);
	return 42;
}

BOOT_TEST(test_detach_other,
	"Test that a thread can detach another thread.")
{


	ASSERT(run_get_status(myproc, 0, NULL) == 42);

	return 0;
}


BOOT_TEST(test_multiple_detach,
	"Test that a thread can be detached many times.")
{
	ASSERT(ThreadDetach(ThreadSelf())==0);
	ASSERT(ThreadDetach(ThreadSelf())==0);
	ASSERT(ThreadDetach(ThreadSelf())==0);
	ASSERT(ThreadDetach(ThreadSelf())==0);

	return 0;
}




/* A thread to be joined */
static int joined_thread(int argl, void* args) {
	sleep_thread(1);
	return 5213;
}

static int joiner_thread(int argl, void* args) {
	int retval;
	Tid_t joined_tid = *(Tid_t*) args;	
	int rc = ThreadJoin(joined_tid,&retval);
	ASSERT(rc==-1 || retval==5213);
	return (rc==0)? 1 : 0;
}


int join_many_threads_main(int argl, void* args) {
	Tid_t tids[5];

	Tid_t joined_tid = CreateThread(joined_thread, 0, NULL);
	ASSERT(joined_tid != NOTHREAD);

	for(int i=0;i<5;i++) {
		tids[i] = CreateThread(joiner_thread,0,&joined_tid);
		ASSERT(tids[i]!=NOTHREAD);
	}

	/* Wait for all joiner_threads to finish */
	int threads_joined = 0;
	for(int i=0;i<5;i++) {
		int this_thread_joined = -100;
		ASSERT(ThreadJoin(tids[i], &this_thread_joined)==0);
		ASSERT(this_thread_joined >= 0);
		threads_joined += this_thread_joined;
		/* tids[i] should be cleaned by ThreadJoin */
		ASSERT(ThreadDetach(tids[i])==-1);
	}

	/* Check that at least one succeeded at joining */
	ASSERT(threads_joined>0);
	return 0;
}


BOOT_TEST(test_join_many_threads,
	"Test that many threads joining the same thread work ok")
{
	ASSERT(run_get_status(join_many_threads_main, 0, NULL)==0);
	return 0;
}



static Tid_t mttid;

static int join_notmain_thread(int argl, void* args) {
	ASSERT(ThreadJoin(mttid, NULL)==0);
	return 0;
}

static int join_main_thread(int argl, void* args) {
	mttid = ThreadSelf();
	ASSERT(CreateThread(join_notmain_thread,0,NULL)!=NOTHREAD);
	return 42;
}


BOOT_TEST(test_join_main_thread,
	"Test that the main thread can be joined by another thread")
{
	ASSERT(run_get_status(join_main_thread, 0, NULL) == 42);
	return 0;
}



int detach_notmain_thread(int argl, void* args) {
	ASSERT(ThreadJoin(mttid, NULL)==-1);
	return 0;
}

int detach_main_thread(int argl, void* args) {
	mttid = ThreadSelf();
	ASSERT(CreateThread(detach_notmain_thread,0,NULL)!=NOTHREAD);
	sleep_thread(1);
	ASSERT(ThreadDetach(ThreadSelf())==0);
	return 42;
}



BOOT_TEST(test_detach_main_thread,
	"Test that the main thread can be detached")
{
	ASSERT(run_get_status(detach_main_thread, 0, NULL) == 42);
	return 0;
}





/* A thread to be joined */
int detach_after_join_joined_thread(int argl, void* args) {
	sleep_thread(1);
	ThreadDetach(ThreadSelf());
	return 5213;
}

int detach_after_join_joiner_thread(int argl, void* args) 
{
	int retval;
	Tid_t joined_tid = *(Tid_t*) args;
	int rc = ThreadJoin(joined_tid,&retval);
	ASSERT(rc==-1);
	return 0;
}


int detach_after_join_main_thread(int argl, void* args) 
{

 	Tid_t joined_tid = CreateThread(detach_after_join_joined_thread, 0, NULL);
 	ASSERT(joined_tid != NOTHREAD);

	Tid_t tids[5];
	for(int i=0;i<5;i++) {
		tids[i] = CreateThread(detach_after_join_joiner_thread,0, &joined_tid);
		ASSERT(tids[i]!=NOTHREAD);
	}

	for(int i=0;i<5;i++) {
		ASSERT(ThreadJoin(tids[i], NULL)==0);
	}

	return 0;		
}

BOOT_TEST(test_detach_after_join,
	"Test that a thread can be detached after joining")
{

	run_get_status(detach_after_join_main_thread, 0, NULL);
	return 0;
}



static int exit_many_threads_task(int argl, void* args) {
	fibo(40);
	Exit(40 + argl);
	return 0;
}

static int exit_many_threads_mthread(int argl, void* args){
	for(int i=0;i<5;i++)
		ASSERT(CreateThread(exit_many_threads_task, i, NULL) != NOTHREAD);

	/* This thread calls ThreadExit probably before the children all exit */
	ThreadExit(0);
	return 0;
}


BOOT_TEST(test_exit_many_threads,
	"Test that if many process threads call Exit, the process will clean up correctly."
	)
{
	int status = run_get_status(exit_many_threads_mthread, 0, NULL);
	ASSERT(40 <= status && status < 45);
	return 0;
}




static int main_exit_cleanup_task(int argl, void* args) {
	fibo(40);
	ThreadExit(2);
	FAIL("We should not be here");
	return 0;
}

static int main_exit_cleanup_main_thread(int argl, void* args){
	for(int i=0;i<5;i++)
		ASSERT(CreateThread(main_exit_cleanup_task, 0, NULL) != NOTHREAD);

	/* This thread calls exit probably before the children all exit */
	return 42;
}

BOOT_TEST(test_main_exit_cleanup,
	"Test that a process where only the main thread calls Exit, will clean up correctly."
	)
{
	ASSERT(run_get_status(main_exit_cleanup_main_thread, 0, NULL)==42);
	return 0;
}





int noexit_cleanup_task(int argl, void* args) {
	fibo(40);
	ThreadExit(2);
	FAIL("We should not be here");
	return 0;
}

int noexit_cleanup_mthread(int argl, void* args){
	for(int i=0;i<5;i++)
		ASSERT(CreateThread(noexit_cleanup_task, 0, NULL) != NOTHREAD);

	/* This thread calls exit probably before the children all exit */
	ThreadExit(0);
	FAIL("We should not be here");
	return 42;
}

BOOT_TEST(test_noexit_cleanup,
	"Test that a process where no thread calls Exit, will clean up correctly."
	)
{
	run_get_status(noexit_cleanup_mthread, 0, NULL);
	return 0;
}




struct cyclic_joins
{
	unsigned int N;
	barrier* B;
	Tid_t* tids;
};

static int cyclic_joins_join_thread(int argl, void* args) {
	struct cyclic_joins* P = args;
	BarrierSync(P->B, P->N+1);
	ThreadJoin( (P->tids)[argl], NULL);
	return argl;
}

static int cyclic_joins_main_thread(int argl, void* args) 
{
	struct cyclic_joins A = *(struct cyclic_joins*)args;
	unsigned int N = A.N;

	/* spawn all N threads */
	for(unsigned int i=0;i<N;i++) {
		A.tids[i] = CreateThread(cyclic_joins_join_thread, (i+1)%N, &A);
		assert(A.tids[i]!=NOTHREAD); /* small assert! do not proceed unless threads are created! */
	}

	/* allow threads to join */
	BarrierSync(A.B, A.N+1);

	/* Wait for threads to proceed */
	sleep_thread(1);

	/* Now, threads are in deadlock! To break the deadlock,
	   detach thread 0. */
	ThreadDetach(A.tids[0]);

	return 0;
}


BOOT_TEST(test_cyclic_joins,
	"Test that a set of cyclically joined threads will not deadlock once the cycle breaks")
{
	const unsigned int N=5;
	barrier B = BARRIER_INIT;
	Tid_t tids[N];

	struct cyclic_joins A = {.N = N, .B = & B, .tids = tids };

	run_get_status(cyclic_joins_main_thread, sizeof(A), &A);
	return 0;
}


TEST_SUITE(thread_tests, 
	"A suite of tests for threads."
	)
{
	&test_join_illegal_tid_gives_error,
	&test_detach_illegal_tid_gives_error,
	&test_detach_self,
	&test_detach_other,
	&test_multiple_detach,
	&test_join_main_thread,
	&test_detach_main_thread,
	&test_detach_after_join,
	&test_create_join_thread,
	&test_join_many_threads,
	&test_exit_many_threads,
	&test_main_exit_cleanup,
	&test_noexit_cleanup,
	&test_cyclic_joins,
	NULL
};







/*********************************************
 *
 *
 *
 *  Pipe tests
 *
 *
 *
 *********************************************/



BOOT_TEST(test_pipe_open,
	"Open a pipe and put just a little data in it"
	)
{
	pipe_t pipe;
	ASSERT(Pipe(&pipe)==0);	
	int rc;

	for(int i=0;i<3;i++) {
		ASSERT((rc=Write(pipe.write, "Hello world", 12))==12);
	}
	char buffer[12] = { [0] = 0 };
	for(int i=0;i<3;i++) {
		ASSERT((rc=Read(pipe.read, buffer, 12))==12);
		ASSERT(strcmp(buffer, "Hello world")==0);
	}
	return 0;
}


BOOT_TEST(test_pipe_fails_on_exhausted_fid,
	"Test that Pipe will fail if the fids are exhausted."
	)
{
	pipe_t pipe;
	for(uint i=0; i< (MAX_FILEID/2); i++ )
		ASSERT(Pipe(&pipe)==0);
	for(uint i=0; i< (MAX_FILEID/2); i++ )
		ASSERT(Pipe(&pipe)==-1);	
	return 0;
}



BOOT_TEST(test_pipe_close_reader,
	"Open a pipe and put just a little data in it"
	)
{
	pipe_t pipe;
	ASSERT(Pipe(&pipe)==0);	
	int rc;

	for(int i=0;i<3;i++) {
		ASSERT((rc=Write(pipe.write, "Hello world", 12))==12);
	}
	Close(pipe.read);
	for(int i=0;i<3;i++) {
		ASSERT((rc=Write(pipe.write, "Hello world", 12))==-1);
	}
	return 0;
}

BOOT_TEST(test_pipe_close_writer,
	"Open a pipe and put just a little data in it"
	)
{
	pipe_t pipe;
	ASSERT(Pipe(&pipe)==0);	
	int rc;

	for(int i=0;i<3;i++) {
		ASSERT((rc=Write(pipe.write, "Hello world", 12))==12);
	}

	char buffer[12] = { [0] = 0 };
	for(int i=0;i<3;i++) {
		ASSERT((rc=Read(pipe.read, buffer, 12))==12);
		ASSERT(strcmp(buffer, "Hello world")==0);
	}
	Close(pipe.write);
	for(int i=0;i<3;i++) {
		ASSERT((rc=Read(pipe.read, buffer, 12))==0);
	}
	return 0;
}


/* Takes one integer argument, writes that many bytes to stdout.
 */
int data_producer(int argl, void* args)
{
	assert(argl == sizeof(int));
	int nbytes = *(int*)args;

	Close(0);

	char buffer[32768];

	while(nbytes>0) {
		unsigned int n = (nbytes<32768) ? nbytes : 32768;
		int rc = Write(1, buffer, n);
		assert(rc>0);
		nbytes -= rc;
	}
	Close(1);
	return 0;
}

/* Takes one integer argument. Reads its standard input to exhaustion,
   asserts it read that many bytes. */
int data_consumer(int argl, void* args) 
{
	assert(argl == sizeof(int));
	int nbytes = *(int*)args;
	Close(1);

	char buffer[16384];
	int count = 0;

	int rc = 1;
	while(rc) {
		rc = Read(0, buffer, 16384);
		assert(rc>=0);
		count += rc;
	}
	ASSERT(count == nbytes);
	return 0;
}


BOOT_TEST(test_pipe_single_producer,
	"Test blocking in the pipe by a single producer single consumer sending 10Mbytes of data."
	)
{
	pipe_t pipe;
	ASSERT(Pipe(&pipe)==0);	

	/* First, make pipe.read be zero. We cannot just Dup, because we may close pipe.write */
	if(pipe.read != 0) {
		if(pipe.write==0) {
			/* Get a null stream! */
			Fid_t fid = OpenNull();
			assert(fid!=NOFILE);
			Dup2(0, fid);
			pipe.write = fid;
		}
		Dup2(pipe.read, 0);
		Close(pipe.read);
	}
	if(pipe.write!=1)  {
		Dup2(pipe.write, 1);
		Close(pipe.write);
	}

	int N = 10000000;
	ASSERT(Exec(data_consumer, sizeof(N), &N)!=NOPROC);
	ASSERT(Exec(data_producer, sizeof(N), &N)!=NOPROC);

	Close(0);
	Close(1);

	WaitChild(NOPROC,NULL);
	WaitChild(NOPROC,NULL);
	return 0;
}

BOOT_TEST(test_pipe_multi_producer,
	"Test blocking in the pipe by 10 producers and single consumer sending 10Mbytes of data."
	)
{
	pipe_t pipe;
	ASSERT(Pipe(&pipe)==0);	

	/* First, make pipe.read be zero. We cannot just Dup, because we may close pipe.write */
	if(pipe.read != 0) {
		if(pipe.write==0) {
			/* Get a null stream! */
			Fid_t fid = OpenNull();
			assert(fid!=NOFILE);
			Dup2(0, fid);
			pipe.write = fid;
		}
		Dup2(pipe.read, 0);
		Close(pipe.read);
	}
	if(pipe.write!=1)  {
		Dup2(pipe.write, 1);
		Close(pipe.write);
	}

	int N = 1000000;
	for(int i=0;i<10;i++)
		ASSERT(Exec(data_producer, sizeof(N), &N)!=NOPROC);
	N = 10*N;
	ASSERT(Exec(data_consumer, sizeof(N), &N)!=NOPROC);

	Close(0);
	Close(1);

	for(int i=0;i<10;i++)
		WaitChild(NOPROC,NULL);
	WaitChild(NOPROC,NULL);
	return 0;
}


TEST_SUITE(pipe_tests,
	"A suite of tests for pipes. We are focusing on correctness, not performance."
	)
{
	&test_pipe_open,
	&test_pipe_fails_on_exhausted_fid,
	&test_pipe_close_reader,
	&test_pipe_close_writer,
	&test_pipe_single_producer,
	&test_pipe_multi_producer,
	NULL
};




/*********************************************
 *
 *
 *
 *  Socket tests
 *
 *
 *
 *********************************************/

struct connect_sockets
{
	Fid_t sock1, lsock, *sock2;
	port_t port;
};

static int connect_sockets_connect_process(int argl, void* args) {
	struct connect_sockets* A = args;
	ASSERT(Connect(A->sock1, A->port, 1000)==0);
	return 0;
}

void connect_sockets(Fid_t sock1, Fid_t lsock, Fid_t* sock2, port_t port)
{
	struct connect_sockets A = { 
		.sock1=sock1, .lsock=lsock, .sock2=sock2, .port=port
	};

	/* Spawn a child to connect sock1 to port (where lsock must be listening) */
	Pid_t pid = Exec(connect_sockets_connect_process, sizeof(A), &A);
	ASSERT(pid != NOPROC);

	/* accept the child's connection here */
	*sock2 = Accept(lsock);
	ASSERT(*sock2 != NOFILE);

	/* Clean up child */
	ASSERT(WaitChild(pid, NULL)==pid);
}


void check_transfer(Fid_t from, Fid_t to)
{
	char buffer[12] = {[0]=0};
	int rc;
	ASSERT((rc=Write(from,"Hello world", 12))==12);
	ASSERT((rc=Read(to, buffer, 12))==12);
	ASSERT((rc=strcmp("Hello world", buffer))==0);
}


BOOT_TEST(test_socket_constructor_many_per_port,
	"Test that Socket succeeds opening many sockets on the same port"
	)
{
	for(Fid_t f=0; f<MAX_FILEID; f++) {
		ASSERT(Socket(100)!=NOFILE);
	}
	return 0;
}

BOOT_TEST(test_socket_constructor_out_of_fids,
	"Test that the socket constructor fails on running out of Fids"
	)
{
	for(int i=0;i<MAX_FILEID;i++)
		ASSERT(Socket(100)!=NOFILE);
	for(int i=0;i<MAX_FILEID;i++)
		ASSERT(Socket(100)==NOFILE);	
	return 0;
}

BOOT_TEST(test_socket_constructor_illegal_port,
	"Test that the socket constructor fails on illegal port"
	)
{
	ASSERT(Socket(NOPORT)!=NOFILE);
	ASSERT(Socket(1)!=NOFILE);
	ASSERT(Socket(MAX_PORT)!=NOFILE);

	ASSERT(Socket(NOPORT-1)==NOFILE);
	ASSERT(Socket(MAX_PORT+1)==NOFILE);	
	ASSERT(Socket(MAX_PORT+10)==NOFILE);
	return 0;	
}

BOOT_TEST(test_listen_success,
	"Test that Listen succeeds on an unbound socket"
	)
{
	ASSERT(Listen(Socket(100))==0);
	return 0;
}

BOOT_TEST(test_listen_fails_on_bad_fid,
	"Test that Listen fails on an invalid fid"
	)
{
	ASSERT(Listen(7)==-1);
	ASSERT(Listen(OpenNull())==-1);
	ASSERT(Listen(NOFILE)==-1);
	ASSERT(Listen(MAX_FILEID)==-1);	
	return 0;
}

BOOT_TEST(test_listen_fails_on_NOPORT,
	"Test that Listen fails on a socket defined on NOPORT"
	)
{
	ASSERT(Listen(Socket(NOPORT))==-1);
	return 0;
}

BOOT_TEST(test_listen_fails_on_occupied_port,
	"Test that Listen fails on an occupied port"
	)
{
	Fid_t f = Socket(100);
	ASSERT(Listen(f)==0);
	ASSERT(Listen(Socket(100))==-1);
	Close(f);
	ASSERT(Listen(Socket(100))==0);	
	return 0;
}

BOOT_TEST(test_listen_fails_on_initialized_socket,
	"Test that Listen fails on a socket that has been previously initialized by Listen"
	)
{
	Fid_t lsock = Socket(100);
	ASSERT(Listen(lsock)==0);	
	ASSERT(Listen(lsock)==-1);	
	Fid_t sock[2];
	sock[0] = Socket(200);
	connect_sockets(sock[0], lsock, sock+1, 100);
	ASSERT(Listen(sock[0])==-1);
	ASSERT(Listen(sock[1])==-1);
	return 0;
}


BOOT_TEST(test_accept_succeds,
	"Test that accept succeeds on a legal connection"
	)
{
	Fid_t lsock = Socket(100);
	ASSERT(Listen(lsock)==0);
	Fid_t cli = Socket(NOPORT);
	Fid_t srv;
	connect_sockets(cli, lsock, &srv, 100);
	return 0;
}

BOOT_TEST(test_accept_fails_on_bad_fid,
	"Test that Accept fails on an invalid fid"
	)
{
	ASSERT(Accept(7)==-1);
	ASSERT(Accept(OpenNull())==-1);
	ASSERT(Accept(NOFILE)==-1);
	ASSERT(Accept(MAX_FILEID)==-1);
	
	return 0;
}

BOOT_TEST(test_accept_fails_on_unbound_socket,
	"Test that Accept fails on an uninitialized socket"
	)
{
	ASSERT(Accept(Socket(100))==-1);
	return 0;
}

BOOT_TEST(test_accept_fails_on_connected_socket,
	"Test that Accept fails on a connected socket"
	)
{
	Fid_t lsock = Socket(100);
	ASSERT(Listen(lsock)==0);
	Fid_t cli = Socket(NOPORT);
	Fid_t srv;
	connect_sockets(cli, lsock, &srv, 100);
	ASSERT(Accept(srv)==-1);		
	return 0;
}

BOOT_TEST(test_accept_reusable,
	"Test that Accept can be called on the same socket many times to create different connections."
	)
{
	Fid_t lsock = Socket(100);
	ASSERT(lsock!=NOFILE);
	ASSERT(Listen(lsock)==0);
	uint n = MAX_FILEID/2 - 1;
	Fid_t cli[n], srv[n];

	for(uint i=0;i<n;i++) {
		cli[i] = Socket(NOPORT);
		connect_sockets(cli[i], lsock, srv+i, 100);
	}
	for(uint i=0;i<n;i++)
		for(uint j=0;j<n;j++)  {
			ASSERT(cli[i]!=srv[j]);
			if(i==j) continue;
			ASSERT(srv[i]!=srv[j]);
			ASSERT(cli[i]!=cli[j]);
		}

	return 0;
}


/* Helper for test_accept_fails_on_exhausted_fid */
static int accept_connection_assert_fail(int argl, void* args) 
{
	ASSERT(argl==sizeof(Fid_t));
	Fid_t lsock = * (Fid_t*) args;
	ASSERT(Accept(lsock)==NOFILE);
	return 0;
}


BOOT_TEST(test_accept_fails_on_exhausted_fid,
	"Test that Accept will fail if the fids of the process are exhausted."
	)
{
	Fid_t lsock = Socket(100);
	ASSERT(lsock!=NOFILE);
	ASSERT(Listen(lsock)==0);

	/* If MAX_FILEID is odd, allocate an extra fid */
	if( (MAX_FILEID & 1) == 1 )  OpenNull();

	/* Allocate pairs of fids */
	for(uint i=0;i< (MAX_FILEID-1)/2 ; i++) {		
		Fid_t cli = Socket(NOPORT);
		Fid_t srv;
		ASSERT(cli != NOFILE);
		connect_sockets(cli,lsock,&srv,100);
	}

	/* Ok, we should be able to get another client */
	Fid_t cli = Socket(NOPORT); ASSERT(cli!=NOFILE);

	/* Call accept on another process and verify it fails */
	Pid_t pid = Exec(accept_connection_assert_fail, sizeof(lsock), &lsock);
	ASSERT(pid!=NOPROC);

	/* Now, if we try a connection we should fail! */
	ASSERT(Connect(cli, 100, 1000)==-1);
	ASSERT(WaitChild(pid, NULL)==pid);
	return 0;
}


static int unblocking_accept_connection(int argl, void* args) 
{
	Fid_t lsock = argl;
	ASSERT(Accept(lsock)==NOFILE);
	return 0;
}



BOOT_TEST(test_accept_unblocks_on_close,
	"Test that Accept will unblock if the listening socket is closed."
	)
{
	Fid_t lsock = Socket(100);
	ASSERT(lsock!=NOFILE);
	ASSERT(Listen(lsock)==0);

	Tid_t t = CreateThread(unblocking_accept_connection, lsock, NULL);

	/* Here, we just wait some time, (of course, this is technically a race condition :-( */
	fibo(30);
	Close(lsock);

	ThreadJoin(t,NULL);
	return 0;
}



BOOT_TEST(test_connect_fails_on_bad_fid,
	"Test that Connect will fail if given a bad fid."
	)
{
	ASSERT(Accept(7)==-1);
	ASSERT(Accept(OpenNull())==-1);
	ASSERT(Accept(NOFILE)==-1);
	ASSERT(Accept(MAX_FILEID)==-1);

	return 0;
}

BOOT_TEST(test_connect_fails_on_bad_socket,
	"Test that Connect will fail if given a listening or connected socket."
	)
{
	Fid_t lsock = Socket(100);
	ASSERT(lsock!=NOFILE);
	ASSERT(Listen(lsock)==0);

	ASSERT(Connect(lsock, 100, 1000)==-1);
	Fid_t cli, srv;
	cli = Socket(NOPORT);
	ASSERT(cli!=NOFILE);
	connect_sockets(cli, lsock, &srv, 100);

	ASSERT(Connect(cli, 100, 1000)==-1);
	ASSERT(Connect(srv, 100, 1000)==-1);

	return 0;
}

BOOT_TEST(test_connect_fails_on_illegal_port,
	"Test that connect fails if given an illegal port."
	)
{
	Fid_t cli = Socket(10);
	ASSERT(Connect(cli, NOPORT, 100)==-1);
	ASSERT(Connect(cli, MAX_PORT+1, 100)==-1);

	return 0;
}

BOOT_TEST(test_connect_fails_on_non_listened_port,
	"Test that Connect fails on a port that has no listener."
	)
{
	Fid_t cli = Socket(10);
	ASSERT(Connect(cli, 100, 100)==-1);

	return 0;
}

BOOT_TEST(test_connect_fails_on_timeout,
	"Test that connect fails on timeout.",
	.timeout = 2
	)
{
	Fid_t lsock = Socket(100);
	ASSERT(lsock!=NOFILE);
	ASSERT(Listen(lsock)==0);

	Fid_t cli = Socket(10);
	/* Give it a short timeout */
	ASSERT(Connect(cli, 100, 100)==-1);

	return 0;
}



BOOT_TEST(test_socket_small_transfer,
	"Open a socket and put just a little data in it, in both directions, for many times."
	)
{
	Fid_t sock[2], lsock;

	lsock = Socket(100);   ASSERT(lsock!=NOFILE);
	if(lsock!=0) { Dup2(lsock,0); Close(lsock); }

	sock[0] = Socket(NOPORT); ASSERT(sock[0]!=NOFILE);

	ASSERT(Listen(lsock)==0);

	connect_sockets(sock[0], lsock, sock+1, 100);
	for(uint i=0; i< 32768; i++) {
		check_transfer(sock[0], sock[1]);
		check_transfer(sock[1], sock[0]);		
	}

	return 0;
}


BOOT_TEST(test_socket_single_producer,
	"Test blocking in the socket by a single producer single consumer sending 10Mbytes of data."
	)
{
	Fid_t fid = Socket(NOPORT);
	ASSERT(fid!=NOFILE);
	if(fid!=0) {
		ASSERT(Dup2(fid,0)==0);
		ASSERT(Close(fid)==0);
	}

	Fid_t lsock = Socket(100);
	ASSERT(lsock!=NOFILE);
	if(lsock!=2) {
		ASSERT(Dup2(lsock,2)==0);
		ASSERT(Close(lsock)==0);
	}

	ASSERT(Listen(2)==0);

	Fid_t srv;
	connect_sockets(0, 2, &srv, 100);

	if(srv!=1) {
		ASSERT(Dup2(srv,1)==0);
		ASSERT(Close(srv)==0);
	}

	int N = 10000000;
	ASSERT(Exec(data_consumer, sizeof(N), &N)!=NOPROC);
	ASSERT(Exec(data_producer, sizeof(N), &N)!=NOPROC);

	Close(0);
	Close(1);

	WaitChild(NOPROC,NULL);
	WaitChild(NOPROC,NULL);
	return 0;
}

BOOT_TEST(test_socket_multi_producer,
	"Test blocking in the pipe by 10 producers and single consumer sending 10Mbytes of data."
	)
{
	Fid_t fid = Socket(NOPORT);
	ASSERT(fid!=NOFILE);
	if(fid!=0) {
		ASSERT(Dup2(fid,0)==0);
		ASSERT(Close(fid)==0);
	}

	Fid_t lsock = Socket(100);
	ASSERT(lsock!=NOFILE);
	if(lsock!=2) {
		ASSERT(Dup2(lsock,2)==0);
		ASSERT(Close(lsock)==0);
	}

	ASSERT(Listen(2)==0);

	Fid_t srv;
	connect_sockets(0, 2, &srv, 100);

	if(srv!=1) {
		ASSERT(Dup2(srv,1)==0);
		ASSERT(Close(srv)==0);
	}

	int N = 1000000;
	for(int i=0;i<10;i++)
		ASSERT(Exec(data_producer, sizeof(N), &N)!=NOPROC);
	N = 10*N;
	ASSERT(Exec(data_consumer, sizeof(N), &N)!=NOPROC);

	Close(0);
	Close(1);

	for(int i=0;i<10;i++)
		WaitChild(NOPROC,NULL);
	WaitChild(NOPROC,NULL);
	return 0;
}



BOOT_TEST(test_shudown_read,
	"Test that ShutDown with SHUTDOWN_READ blocks Write"
	)
{
	Fid_t lsock;
	lsock = Socket(100);   ASSERT(lsock!=NOFILE);
	if(lsock!=0) { Dup2(lsock,0); Close(lsock); }
	ASSERT(Listen(lsock)==0);

	Fid_t cli = Socket(NOPORT); ASSERT(cli!=NOFILE);
	Fid_t srv;

	connect_sockets(cli, lsock, &srv, 100);

	for(uint i=0; i< 2000; i++) {
		check_transfer(cli, srv);
		check_transfer(srv, cli);
	}

	ASSERT(Write(srv, "Hello world",12)==12);

	ShutDown(cli, SHUTDOWN_READ);
	char buffer[12];
	ASSERT(Read(cli, buffer, 12)==-1);
	ASSERT(Write(srv, "Hello world",12)==-1);

	for(uint i=0; i< 2000; i++) {
		ASSERT(Write(srv, "Hello world",12)==-1);
		check_transfer(cli, srv);
	}

	return 0;
}


BOOT_TEST(test_shudown_write,
	"Test that ShutDown with SHUTDOWN_WRITE first exhausts buffers and then causes Read to return 0"
	)
{
	Fid_t lsock;
	lsock = Socket(100);   ASSERT(lsock!=NOFILE);
	if(lsock!=0) { Dup2(lsock,0); Close(lsock); }
	ASSERT(Listen(lsock)==0);

	Fid_t cli = Socket(NOPORT); ASSERT(cli!=NOFILE);
	Fid_t srv;

	connect_sockets(cli, lsock, &srv, 100);

	for(uint i=0; i< 2000; i++) {
		check_transfer(cli, srv);
		check_transfer(srv, cli);
	}

	ASSERT(Write(srv, "Hello world",12)==12);
	ASSERT(Write(srv, "Hello world",12)==12);
	ASSERT(Write(srv, "Hello world",12)==12);

	ShutDown(srv, SHUTDOWN_WRITE);
	ASSERT(Write(srv, "Hello world",12)==-1);

	/* We should still read the three writes before the close. */
	char buffer[12];
	for(uint i=0;i<3;i++) {
		ASSERT(Read(cli, buffer, 12)==12);
		ASSERT(strcmp(buffer,"Hello world")==0);
	}

	for(uint i=0; i< 2000; i++) {
		ASSERT(Read(cli, buffer, 12)==0);
		check_transfer(cli, srv);
	}

	return 0;
}




TEST_SUITE(socket_tests,
	"A suite of tests for sockets."
	)
{
	&test_socket_constructor_many_per_port,
	&test_socket_constructor_out_of_fids,
	&test_socket_constructor_illegal_port,
	
	&test_listen_success,
	&test_listen_fails_on_bad_fid,
	&test_listen_fails_on_NOPORT,
	&test_listen_fails_on_occupied_port,
	&test_listen_fails_on_initialized_socket,

	&test_accept_succeds,
	&test_accept_fails_on_bad_fid,
	&test_accept_fails_on_unbound_socket,
	&test_accept_fails_on_connected_socket,
	&test_accept_reusable,
	&test_accept_fails_on_exhausted_fid,
	&test_accept_unblocks_on_close,

	&test_connect_fails_on_bad_fid,
	&test_connect_fails_on_bad_socket,
	&test_connect_fails_on_illegal_port,
	&test_connect_fails_on_non_listened_port,
	&test_connect_fails_on_timeout,

	&test_socket_small_transfer,
	&test_socket_single_producer,
	&test_socket_multi_producer,

	&test_shudown_read,
	&test_shudown_write,

	NULL
};




/*********************************************
 *
 *
 *
 *  Concurrency tests
 *
 *
 *
 *********************************************/





unsigned int timestamp=0;

unsigned int get_timestamp()
{
	return __atomic_fetch_add(&timestamp, 1, __ATOMIC_SEQ_CST);	
}


BOOT_TEST(test_multitask,
	"Test that Exec returns before execution of the child is finished."
	)
{
	unsigned int tschild;
	int child(int argl, void* args)
	{
		unsigned int f = fibo(38);
		tschild = get_timestamp();
		return f>10;
	}	

	Exec(child, 0, NULL);
	unsigned int ts = get_timestamp();
	WaitChild(NOPROC, NULL);

	ASSERT(ts < tschild);
	return 0;
}



BOOT_TEST(test_preemption,
	"Test that children are executed preemptively."
	)
{
	int child(int argl, void* args)
	{
		unsigned int* ts[2];
		assert(sizeof(ts)==argl);
		memcpy(ts, args, sizeof(ts));

		*(ts[0]) = get_timestamp();
		fibo(40);
		*(ts[1]) = get_timestamp();
		return 0;
	}

#define NCHILDREN 2
	unsigned int start[NCHILDREN], end[NCHILDREN];

	for(int i=0;i<NCHILDREN;i++) {
		unsigned int* args[2];
		args[0] = &start[i];
		args[1] = &end[i];

		Exec(child, sizeof(args), args);
	}


	for(int i=0; i<NCHILDREN; i++)
		WaitChild(NOPROC, NULL);

	for(int i=0;i<NCHILDREN;i++)
		for(int j=0; j<NCHILDREN; j++)
			if(i!=j)
				ASSERT(start[i] < end[j]);

	return 0;

#undef NCHILDREN
}





void mark_time(struct timeval* t)
{
	CHECK(gettimeofday(t, NULL));
}
double time_since(struct timeval* t0)
{
	struct timeval t1;
	mark_time(&t1);

	return ((double)(t1.tv_sec-t0->tv_sec)) + 1E-6* (t1.tv_usec - t0->tv_usec);
}


int compute_child(int argl, void* args)
{
	unsigned int fib = fibo(35);
	return fib %3 ==1;
}


BARE_TEST(test_parallelism,
	"This test tests whether multiple cores are used in parallel.",
	.timeout = 30, .minimum_cores=2,
	)
{

	struct timeval tstart;
	double Trun;

	int run_twice(int argl, void* args)
	{
		mark_time(&tstart);
		Exec(compute_child, 0, NULL);
		Exec(compute_child, 0, NULL);
		WaitChild(NOPROC, NULL);
		WaitChild(NOPROC, NULL);
		Trun = time_since(&tstart);
		return 0;
	}


	double run_times(uint ntimes, uint ncores)
	{
		double minTrun=0.0;
		for(int I=0;I<ntimes; I++) {
			boot(ncores, 0, run_twice, 0, NULL);
			if(I==0)
				minTrun = Trun;
			else 
				if(Trun < minTrun) minTrun = Trun;
		}
		return minTrun;
	}

	if(sysconf(_SC_NPROCESSORS_ONLN)<2) {
		MSG("Cannot run this test on this machine, there is only 1 core.\n");
		return;
	}

	double T1 = run_times(10, 1);
	double T2 = run_times(10, 2);

	MSG("1-core: %f   2-core: %f\n", T1, T2);
	ASSERT_MSG( fabs(T1/T2  - 2.0) < 0.4, 
		"Runtimes did not decrease enough. one core: %f sec    two cores: %f sec\n", 
		T1, T2);
}



TEST_SUITE(concurrency_tests,
	"A suite of tests which test the operational concurrency of the kernel."
	)
{
	&test_multitask,
	&test_preemption,
	&test_parallelism,
	NULL
};





/*********************************************
 *
 *
 *
 *  Terminal I/O tests
 *
 *
 *
 *********************************************/





BOOT_TEST(test_input_concurrency,
	"Test that input from one terminal does not obstruct input from other terminals.",
	.minimum_terminals = 2
	)
{
	void open_at_0(uint term)
	{
		Fid_t f = OpenTerminal(term);
		ASSERT(f!=NOFILE);
		if(f!=0) {
			ASSERT(Dup2(f,0)==0);
			ASSERT(Close(f)==0);
		}
	}

	int input_line(int argl, void* args)
	{
		FILE* fin = fidopen(0, "r");
		char* line=NULL;
		size_t llen;

		ASSERT(getline(&line, &llen, fin));
		fclose(fin);
		free(line);
		return 0;
	}


	open_at_0(0);
	Pid_t p0 = Exec(input_line, 0, NULL);
	open_at_0(1);
	Pid_t p1 = Exec(input_line, 0, NULL);

	sendme(0, "Hello ");
	sendme(1, "Hellow world\n");
	ASSERT(WaitChild(p1, NULL)==p1);
	sendme(0, " world\n");
	ASSERT(WaitChild(p0, NULL)==p0);
	return 0;
}



BOOT_TEST(test_term_input_driver_interrupt,
	"Test that terminal input is interrupt driven. This is done by\n"
	"opening a huge number of processes reading from the terminal and\n"
	"measuring the effect on a compute process.",
	.minimum_terminals = 1, .timeout = 100
	)
{
	int input_char(int argl, void* args)
	{
		char c;
		ASSERT(Read(0, &c, 1)==1);
		return 0;
	}

	struct timeval t0;
	double minTrun, maxTrun;

	void run_times(uint ntimes)
	{
		for(int I=0;I<ntimes; I++) {
			mark_time(&t0);
			Pid_t pid = Exec(compute_child,0,NULL);
			ASSERT(pid!=NOPROC);
			ASSERT(WaitChild(pid, NULL)==pid);
			double Trun = time_since(&t0);

			if(I==0)
				minTrun = maxTrun = Trun;
			else {
				if( Trun < minTrun) minTrun = Trun;
				if( Trun > maxTrun) maxTrun = Trun;
			}
		}
	}

	void run_with_io(int nioproc)
	{
		for(int i=0; i< nioproc; i++)
			ASSERT(Exec(input_char, 0, NULL)!=NOPROC);

		/* If the readers are polling as the compute task runs, 
		   this will take a long time! */
		run_times(10);

		char* dummy_input = malloc((nioproc+1)*sizeof(char));
		for(int i=0; i< nioproc; i++)
			dummy_input[i] = 'A';
		dummy_input[nioproc] = '\0';
		sendme(0, dummy_input);

		free(dummy_input);
		for(int i=0; i< nioproc; i++)
			ASSERT(WaitChild(NOPROC, NULL)!=NOPROC);
	}

	Fid_t fid = OpenTerminal(0);
	ASSERT(fid!=NOFILE);
	if(fid!=0) {
		ASSERT(Dup2(fid,0)==0);
		ASSERT(Close(fid)==0);		
	}

	run_with_io(1000);
	double T1000 = minTrun;

	run_with_io(0);
	double T0 = minTrun;

	MSG("Trun(0)= %f   Trun(1000)= %f\n", T0, T1000);
	ASSERT_MSG( (T1000-T0)/T0 < 0.1, "Failed: Trun(0)= %f   Trun(1000)= %f\n", T0, T1000 );

	return 0;
}




TEST_SUITE(io_tests,
	"A suite of tests which test the concurrency of terminal I/O."
	)
{
	&test_input_concurrency,
	&test_term_input_driver_interrupt,
	NULL
};




/*********************************************
 *
 *
 *
 *  Main program
 *
 *
 *
 *********************************************/




TEST_SUITE(all_tests,
	"A suite containing all tests.")
{
	&basic_tests,
	//&concurrency_tests,
	//&io_tests,
	&thread_tests,
	&pipe_tests,
	&socket_tests,
	NULL
};



/****************************************************************************
 *
 *       U S E R    T E S T S
 *
 * Feel free to add your own tests in the section below. For each test you
 * add, don't forget to add it to the 'user_tests' suite.
 * You can then run your tests by
 *   ./validate_api user_tests
 ****************************************************************************/


BARE_TEST(dummy_user_test,
	"A dummy test, feel free to edit it and copy it as needed."
	)
{
	ASSERT(1+1==2);
}


TEST_SUITE(user_tests, 
	"These are tests defined by the user."
	)
{
	&dummy_user_test,
	NULL
};





int main(int argc, char** argv)
{
	register_test(&all_tests);
	register_test(&user_tests);
	return run_program(argc, argv, &all_tests);
}



