
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <assert.h>

#include "tinyoslib.h"
#include "symposium.h"
#include "bios.h"

int Shell(size_t,const char**);
int RunTerm(size_t,const char**);
int ListPrograms(size_t,const char**);
int Fibonacci(size_t,const char**);
int Repeat(size_t,const char**);
int Hanoi(size_t,const char**);
int HelpMessage(size_t,const char**);
int SystemInfo(size_t,const char**);
int Symposium_wrap(size_t,const char**);


struct { const char * cmdname; Program prog; uint nargs; const char* help; } 
COMMANDS[]  = 
{
	{"help", HelpMessage, 0, "A help message."},
	{"ls", ListPrograms, 0, "List available programs programs."},
	{"sysinfo", SystemInfo, 0, "Print some basic info about the current system."},
	{"runterm", RunTerm, 2, "runterm <term> <prog>  <args...> : execute '<prog> <args...>' on terminal <term>."},
	{"sh", Shell, 0, "Run a shell."},
	{"repeat", Repeat, 2, "repeat <n> <prog> <args...>: execute '<prog> <args...>' <n> times."},
	{"fibo", Fibonacci, 1, "Compute a fibonacci number."},
	{"symposium", Symposium_wrap, 2, "Dining Philosophers: symposium  <philosophers> <bites>"},
	{"hanoi", Hanoi, 1, "The towers of Hanoi."},
	{NULL, NULL, 0, NULL}
};

int programs() {
	int c=0;
	while(COMMANDS[c++].cmdname);
	return c;
}

#define checkargs(argno) if(argc < (argno)) {\
  printf("Insufficient arguments. %d expected, %zd given.\n", (argno), argc);\
  return -1; }\

#define getint(n)  atoi(argv[n])

static inline int getprog_byname(const char* name) {
	for(int c=0; COMMANDS[c].cmdname ; c++)
		if(strcmp(COMMANDS[c].cmdname, name)==0)
			return c;
	return -1;
}

#define getprog(n) getprog_byname(argv[n])


int Symposium_wrap(size_t argc, const char** argv)
{
	checkargs(2);
	int args[4] = {0, 0, 0, 0};
	args[0] = getint(0);
	args[1] = getint(1);
	if(argc>=3) args[2] = getint(2);
	if(argc>=4) args[3] = getint(3);


	return Symposium_adjusted(sizeof(args), args);
}


int Repeat(size_t argc, const char** argv)
{
	checkargs(2);
	int times = getint(0);
	int prog = getprog(1);
	int ac = argc-2;
	const char** av = argv+2;

	/* Find program */
	if(prog<0) {
		printf("Program number out of range. See 'ls' for program names.\n");
		return prog;
	}
	if(times<0) {
		printf("Cannot execute a negative number of times!\n");
	}
	while(times--) {
		Execute(COMMANDS[prog].prog, ac, av);
		WaitChild(NOPROC,NULL);
	}
	return 0;
}


int RunTerm(size_t argc, const char** argv)
{
	checkargs(2);

	int term = getint(0);
	int prog = getprog(1);

	if(term<0 || term>=GetTerminalDevices()) {
		printf("The terminal provided is not valid: %d\n", term);
		return 1;
	}
	if(prog<0) {
		printf("The program provided is not valid: %d\n", term);
		return 2;		
	}

	/* Change our own terminal, so that the child inherits it. */
	int termfid = OpenTerminal(term);
	if(termfid!=0) {
		Dup2(termfid, 0);
		Close(termfid);
	}
	Dup2(0, 1);  /* use the same stream for stdout */

	return Execute(COMMANDS[prog].prog, argc-2, argv+2);
}


int SystemInfo(size_t argc, const char** argv)
{
	printf("Number of cores         = %d\n", cpu_cores());
	printf("Number of serial devices= %d\n", bios_serial_ports());
	printf("\n");

	return 0;
}


int HelpMessage(size_t argc, const char** argv)
{
	printf("This is a simple shell for tinyos.\n\
\n\
You can run some simple commands. Every command takes a\n\
only **integer** arguments. The list of commands and the\n\
number of arguments for each command is shown by\n\
typing 'ls'. \n\n\
When you are tired of playing, type 'exit' to quit.\n\
");
	return 0;
}


void hanoi(int n, int a, int b, int c)
{
	if(n==0) return;
	hanoi(n-1, a, c, b);
	printf("Move the top disk from tile %2d to tile %2d\n", a, b);
	hanoi(n-1, c, b, a);
}

int Hanoi(size_t argc, const char** argv)
{
	checkargs(1);
	int n = getint(0);
	int MAXN = 15;

	if(n<1 || n>MAXN) {
		printf("The argument must be between 1 and %d.\n",MAXN);
		return n;
	}

	printf("We shall move %d disks from tile 1 to tile 2 via tile 3.\n",n);
	hanoi(n, 1,2,3);
	return 0;
}

int Fibonacci(size_t argc, const char** argv)
{
	checkargs(1);
	int n = getint(0);

	printf("Fibonacci(%d)=%d\n", n, fibo(n));
	return 0;
}


int ListPrograms(size_t argc, const char** argv)
{
	printf("no.  %-15s no.of.args   help \n", "Command");
	printf("%s\n","---------------------------------------------------");
	for(int c=0; COMMANDS[c].cmdname; c++) {
		printf("%3d  %-20s %2d   %s\n", c, COMMANDS[c].cmdname, COMMANDS[c].nargs, COMMANDS[c].help);
	}
	return 0;
}



/*
	A very simple shell for tinyos 
*/
int Shell(size_t argc, const char** argv)
{
	char* cmdline = NULL;
	size_t cmdlinelen = 0;
	printf("Starting tinyos shell\nType 'help' for help, 'exit' to quit.\n");

	for(;;) {
		const char * argv[16];
		char* pos;
		int argc;

		/* Read the command line */
		printf("%% "); 
		ssize_t rc;

		again:
		rc = getline(&cmdline, &cmdlinelen, stdin);
		if(rc==-1 && ferror(stdin) && errno==EINTR) { 
			clearerr(stdin); 
			goto again; 
		}
		if(rc==-1 && feof(stdin)) {
			break;
		}


		/* Break it up */
		for(argc=0; argc<15; argc++) {
			argv[argc] = strtok_r((argc==0? cmdline : NULL), " \n\t", &pos);
			if(argv[argc]==NULL)
				break;
		}

		if(argc==0) continue;

		/* execute the command */
		if(strcmp(argv[0], "exit")==0) break;

		/* find command in the list of commands */
		int c;
		for(c=0; COMMANDS[c].cmdname; c++) {
			const char* cmdname = COMMANDS[c].cmdname;

			if(strcmp(argv[0], cmdname)==0) {
				Program prog = COMMANDS[c].prog;
				int nargs = COMMANDS[c].nargs;

				if(nargs>argc-1) {
					printf("%15s: not enough arguments, %d needed but %d given.\n", 
						cmdname, nargs, argc-1);
					printf("%15s: %s\n", "help", COMMANDS[c].help);
					break;
				}

				/* Ok, run the thing */
				Pid_t pid = Execute(prog, argc-1, argv+1);

				int exitval;
				WaitChild(pid, &exitval);
				if(exitval)
					printf("%s exited with status %d\n", cmdname, exitval);
				break;
			}
		}
		if(! COMMANDS[c].cmdname)
			printf("Command not found: '%s'\n", argv[0]);


	}
	printf("Exiting\n");
	free(cmdline);
	return 0;
}



/*
 * This is the initial task, which starts all the other tasks (except for the idle task). 
 */
int boot_shell(int argl, void* args)
{
  int nshells = (GetTerminalDevices()>0) ? GetTerminalDevices() : 1;

  if(GetTerminalDevices()) {
  	fprintf(stderr, "Switching standard streams\n");
    tinyos_replace_stdio();
  	for(int i=0; i<nshells; i++) {
  		int fdin = OpenTerminal(i);
  		if(fdin!=0) {  Dup2(fdin, 0 ); Close(fdin);  }
  		int fdout = OpenTerminal(i);
  		if(fdout!=1) {  Dup2(fdout, 1 ); Close(fdout);  }
  		Execute(Shell, 0, NULL);
  		Close(0);
  	}
	while( WaitChild(NOPROC, NULL)!=NOPROC ); /* Wait for all children */
    tinyos_restore_stdio();
  } else {
  		Execute(Shell, 0, NULL);  	
		while( WaitChild(NOPROC, NULL)!=NOPROC ); /* Wait for all children */
  }

  return 0;
}

/****************************************************/

void usage(const char* pname)
{
  printf("usage:\n  %s <ncores> <nterm> <philosophers> <bites>\n\n  \
    where:\n\
    <ncores> is the number of cpu cores to use,\n\
    <nterm> is the number of terminals to use,\n",
	 pname);
  exit(1);
}


int main(int argc, const char** argv) 
{
  unsigned int ncores, nterm;

  if(argc!=3) usage(argv[0]); 
  ncores = atoi(argv[1]);
  nterm = atoi(argv[2]);

  /* boot TinyOS */
  printf("*** Booting TinyOS with %d cores and %d terminals\n", ncores, nterm);
  boot(ncores, nterm, boot_shell, 0, NULL);
  printf("*** TinyOS halted. Bye!\n");

  return 0;
}


