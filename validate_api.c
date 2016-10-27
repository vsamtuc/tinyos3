
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



BOOT_TEST(test_waitchild_error_on_invalid_pid,
	"Test that WaitChild returns an error when the pid is invalid."
	)
{
	void waitchild_error()
	{
		/* Cannot wait on myself */
		ASSERT(WaitChild(GetPid(),NULL)==NOPROC);
		ASSERT(WaitChild(MAX_PROC, NULL)==NOPROC);
		ASSERT(WaitChild(GetPid()+1, NULL)==NOPROC);
	}
	int subprocess(int argl, void* args) 
	{
		ASSERT(GetPid()!=1);
		waitchild_error();
		return 0;
	}
	waitchild_error();
	Pid_t cpid = Exec(subprocess, 0, NULL);
	ASSERT(WaitChild(NOPROC, NULL)==cpid);
	return 0;
}


BOOT_TEST(test_waitchild_error_on_nonchild,
	"Test that WaitChild returns an error when the process is not\n"
	"its child."
	)
{
	int void_child(int argl, void* args) { return 0; }
	Pid_t cpid = Exec(void_child, 0, NULL);
	int bad_child(int argl, void* args)
	{
		ASSERT(WaitChild(cpid, NULL)==NOPROC);
		return 0;
	}
	Pid_t badpid = Exec(bad_child, 0, NULL);
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
	"returns the correct status."
	)
{
	struct test_pid_rec  myrec;  /* only used by init task */
	struct test_pid_rec* prec;

	if(argl==0) {
		ASSERT(GetPid()==1);
		prec = &myrec;
		prec->level = 9;      /* 4^9 = 2^18 ~= 260000 children will be run */
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



BOOT_TEST(test_exec_copies_arguments,
	"Test that Exec creates of copy of the arguments of the new process."
	)
{
	int child(int argl, void* args)
	{
		*(int*)args = 1;
		return 0;
	}

	Pid_t cpid;
	int value = 0;
	ASSERT((cpid = Exec(child, sizeof(value), &value))!=NOPROC);
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


BOOT_TEST(test_exit_returns_status,
	"Test that the exit status is returned by Exit"
	)
{
 	int child(int arg, void* args) {
		Exit(GetPid());
		ASSERT(0);
		return 1;
	}
	Pid_t children[100];
	for(int i=0;i<100;i++)
		children[i] = Exec(child, 0, NULL);

	for(int i=0;i<100;i++) {
		int status;
		WaitChild(children[i], &status);
		ASSERT(status==children[i]);
	}
	return 0;
}

BOOT_TEST(test_main_return_returns_status,
	"Test that the exit status is returned by return from main task"
	)
{
 	int child(int arg, void* args) {
		return GetPid();
	}
	const int N=10;
	Pid_t children[N];
	for(int i=0;i<N;i++)
		children[i] = Exec(child, 0, NULL);
	for(int i=0;i<N;i++) {
		int status;
		WaitChild(children[i], &status);
		ASSERT(status==children[i]);
	}
	return 0;
}


BOOT_TEST(test_orphans_adopted_by_init,
	"Test that when a process exits leaving orphans, init becomes the new parent."
	)
{
	int grandchild(int argl, void* args)
	{
		return 1;
	}
	int child(int arg, void* args)
	{
		for(int i=0;i<5;i++)
			ASSERT(Exec(grandchild, 0, NULL)!=NOPROC);
		return 100;
	}

	for(int i=0;i<3; i++)
		ASSERT(Exec(child,0,NULL)!=NOPROC);


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



BOOT_TEST(test_child_inherits_files,
	"Test that a child process inherits files.",
	.minimum_terminals = 1
	)
{
	Fid_t fterm = OpenTerminal(0);
	ASSERT(fterm!=NOFILE);
	if(fterm!=0)
		ASSERT(Dup2(fterm, 0)==0);

	int greeted_child(int argl, void* args)
	{
		checked_read(0, "Hello child");
		checked_read(0, "Hello again");
		return 0;
	}

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
	char buffer[10];
	ASSERT(Write(0, buffer, 10)==-1);
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




BOOT_TEST(test_create_join_thread,
	"Test that a process thread can be created and joined. Also, that "
	"the argument of the thread is passed correctly."
	)
{
	int flag = 0;

	int task(int argl, void* args) {
		ASSERT(args == &flag);
		*(int*)args = 1;
		return 2;
	}

	Tid_t t = CreateThread(task, sizeof(flag), &flag);
	ASSERT(t!=NOTHREAD);
	int exitval;
	ASSERT(ThreadJoin(t, &exitval)==0);
	ASSERT(flag==1);
	return 0;
}


BOOT_TEST(test_exit_many_threads,
	"Test that a process thread calling Exit will clean up correctly."
	)
{

	int task(int argl, void* args) {
		fibo(45);
		return 2;
	}

	int mthread(int argl, void* args){
		for(int i=0;i<5;i++)
			ASSERT(CreateThread(task, 0, NULL) != NOTHREAD);

		fibo(35);
		return 0;
	}

	Exec(mthread, 0, NULL);
	ASSERT(WaitChild(NOPROC, NULL)!=NOPROC);

	return 0;
}






TEST_SUITE(thread_tests, 
	"A suite of tests for threads."
	)
{
	&test_create_join_thread,
	&test_exit_many_threads,
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




void connect_sockets(Fid_t sock1, Fid_t lsock, Fid_t* sock2, port_t port)
{
	int accept_thread(int argl, void* args) {
		*sock2 = Accept(lsock);
		ASSERT(*sock2 != NOFILE);
		return 0;
	}
	int connect_thread(int argl, void* args) {
		ASSERT(Connect(sock1, port, 1000)==0);
		return 0;
	}

	Tid_t t1,t2;
	t1 = CreateThread(accept_thread, 0, NULL);
	t2 = CreateThread(connect_thread, 0, NULL);
	ASSERT(t1!=NOTHREAD);
	ASSERT(t2!=NOTHREAD);
	ASSERT(ThreadJoin(t1, NULL)==0);
	ASSERT(ThreadJoin(t2, NULL)==0);

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

	/* Now, if we try a connection we should fail! */
	int accept_connection(int argl, void* args) {
		ASSERT(Accept(lsock)==NOFILE);
		return 0;
	}

	Tid_t t = CreateThread(accept_connection, 0, NULL);
	ASSERT(Connect(cli, 100, 1000)==-1);

	ThreadJoin(t,NULL);
	return 0;
}

BOOT_TEST(test_accept_unblocks_on_close,
	"Test that Accept will unblock if the listening socket is closed."
	)
{
	Fid_t lsock = Socket(100);
	ASSERT(lsock!=NOFILE);
	ASSERT(Listen(lsock)==0);

	int accept_connection(int argl, void* args) {
		ASSERT(Accept(lsock)==NOFILE);
		return 0;
	}
	Tid_t t = CreateThread(accept_connection, 0, NULL);

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



