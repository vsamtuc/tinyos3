
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>

#include "tinyoslib.h"
#include "symposium.h"
#include "bios.h"
#include "util.h"

int Shell(size_t,const char**);
int RunTerm(size_t,const char**);
int ListPrograms(size_t,const char**);
int Fibonacci(size_t,const char**);
int Repeat(size_t,const char**);
int Hanoi(size_t,const char**);
int HelpMessage(size_t,const char**);
int SystemInfo(size_t,const char**);
int Capitalize(size_t,const char**);
int LowerCase(size_t,const char**);
int LineEnum(size_t,const char**);
int More(size_t,const char**);
int WordCount(size_t,const char**);
int Symposium_proc(size_t,const char**);
int Symposium_thr(size_t,const char**);
int RemoteServer(size_t,const char**);
int RemoteClient(size_t,const char**);
int Echo(size_t,const char**);


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
	{"cap", Capitalize, 0, "Copy stdin to stdout, capitalizing all letters"},
	{"lcase", LowerCase, 0, "Copy stdin to stdout, lower-casing all letters"},
	{"wc", WordCount, 0, "Count and print lines, words and chars of stdin"},
	{"lenum", LineEnum, 0, "Copy stdin to stdout, adding line numbers"},
	{"more", More, 0, "more [<n>] (default: <n>=20). Read the input <n> lines at a time."},	
	{"symposium", Symposium_proc, 2, "Dining Philosophers(processes): symposium  <philosophers> <bites>"},
	{"symp_thr", Symposium_thr, 2, "Dining Philosophers(threads): symp_thr  <philosophers> <bites>"},
	{"hanoi", Hanoi, 1, "The towers of Hanoi."},
	{"rserver", RemoteServer, 0, "A server for remote execution."},
	{"rcli", RemoteClient, 1, "Remote client: rcli <cmd> [<args...>]."},
	{"echo", Echo, 0, "echo [<args...>], send the <args...> to stdout"},

	{NULL, NULL, 0, NULL}
};

int programs() {
	int c=0;
	while(COMMANDS[c++].cmdname);
	return c;
}

#define checkargs(argno) if(argc <= (argno)) {\
  printf("Insufficient arguments. %d expected, %zd given.\n", (argno), argc-1);\
  return -1; }\

#define getint(n)  atoi(argv[n])

static inline int getprog_byname(const char* name) {
	for(int c=0; COMMANDS[c].cmdname ; c++)
		if(strcmp(COMMANDS[c].cmdname, name)==0)
			return c;
	return -1;
}

#define getprog(n) getprog_byname(argv[n])


static void __symp_argproc(size_t argc, const char** argv, symposium_t* symp)
{
	symp->N = getint(1);
	symp->bites = getint(2);

	int dBASE = 0;
	int dGAP = 0;

	if(argc>=4) dBASE = getint(3);
	if(argc>=5) dGAP = getint(4);

	adjust_symposium(symp, dBASE, dGAP);
}


int Symposium_thr(size_t argc, const char** argv)
{
	checkargs(2);
	symposium_t symp;
	__symp_argproc(argc, argv, &symp);
	return SymposiumOfThreads(sizeof(symp), &symp);
}

int Symposium_proc(size_t argc, const char** argv)
{
	checkargs(2);
	symposium_t symp;
	__symp_argproc(argc, argv, &symp);
	return SymposiumOfProcesses(sizeof(symp), &symp);
}


int Repeat(size_t argc, const char** argv)
{
	checkargs(2);
	int times = getint(1);
	int prog = getprog(2);
	int ac = argc-2;
	const char** av = argv+2;

	/* Find program */
	if(prog<0) {
		printf("Program not found. See 'ls' for program names.\n");
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

	int term = getint(1);
	int prog = getprog(2);

	if(term<0 || term>=GetTerminalDevices()) {
		printf("The terminal provided is not valid: %d\n", term);
		return 1;
	}
	if(prog<0) {
		printf("The program provided is not valid: %s\n", argv[2]);
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
	Fid_t finfo = OpenInfo();
	if(finfo!=NOFILE) {
		/* Print per-process info */
		procinfo info;
		printf("%5s %5s %6s %8s %20s\n",
			"PID", "PPID", "State", "Threads", "Main program"
			);
		/* Read in next piece of info */		
		while(Read(finfo, (char*) &info, sizeof(info)) > 0) {
			Program prog=NULL;
			const char* argv[10];
			int argc = ParseProcInfo(&info, &prog, 10, argv);

			const char* pname = "-";
			if(argc>=1)  {
				pname = argv[0];
			} else if(argc==-1) {
				/* Try to give some known names */
				if(info.pid==1) pname = "init";
			}

			printf("%5d %5d %6s %8u %20s\n",
				info.pid,
				info.ppid,
				(info.alive?"ALIVE":"ZOMBIE"),
				info.thread_count,
				pname
				);
		}
	}
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
	int n = getint(1);
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
	int n = getint(1);

	printf("Fibonacci(%d)=%d\n", n, fibo(n));
	return 0;
}


int Capitalize(size_t argc, const char** argv)
{
	char c;
	FILE* fin = fidopen(0, "r");
	FILE* fout = fidopen(1, "w");
	while((c=fgetc(fin))!=EOF) {
		fputc(toupper(c), fout);
	}
	fclose(fin);
	fclose(fout);
	return 0;
}


int Echo(size_t argc, const char** argv)
{
	FILE* fout = fidopen(1, "w");
	for(size_t i=1; i<argc; i++) {
		if(i>1) fputs(" ", fout);
		fprintf(fout,"%s", argv[i]);
	}
	fprintf(fout,"\n");
	fclose(fout);
	return 0;
}


int LowerCase(size_t argc, const char** argv)
{
	char c;
	FILE* fin = fidopen(0, "r");
	FILE* fout = fidopen(1, "w");
	while((c=fgetc(fin))!=EOF) {
		fputc(tolower(c), fout);
	}
	fclose(fin);
	fclose(fout);
	return 0;
}


int LineEnum(size_t argc, const char** argv)
{
	char c;
	FILE* fin = fidopen(0, "r");
	FILE* fout = fidopen(1, "w");
	int atend=1;
	size_t count=0;
	while((c=fgetc(fin))!=EOF) {
		if(atend) {
			count++;
			fprintf(fout, "%6zu: ", count);
			atend = 0;
		}

		fputc(c, fout);
		if(c=='\n') atend=1;
	}
	fclose(fin);
	fclose(fout);
	return 0;
}

int More(size_t argc, const char** argv)
{
	char* _line = NULL;
	size_t _lno = 0;

	int page = 25;
	if(argc>=2) {
		page = getint(1);
	}

	char c;
	FILE* fin = fidopen(0, "r");
	FILE* fout = fidopen(1, "w");
	FILE* fkbd = fidopen(1, "r");

	int atend=1;
	size_t count=0;
	while((c=fgetc(fin))!=EOF) {
		//printf("Read '%c'\n",c);
		if(atend) {
			count++;
			atend = 0;
			if(count % page == 0) {
				/* Here, we have to use getline, unless we change terminal */
				fprintf(fout, "press enter to continue:");
				(void)getline(&_line, &_lno, fkbd);
			}
		}

		fprintf(fout, "%c",c);
		if(c=='\n') atend=1;
	}
	fclose(fin);
	fclose(fout);
	fclose(fkbd);
	free(_line);
	return 0;
}



int WordCount(size_t argc, const char** argv)
{
	size_t nchar, nword, nline;
	nchar = nword = nline = 0;
	int wspace = 1;
	char c;
	FILE* fin = fidopen(0, "r");
	while((c=fgetc(fin))!=EOF) {
		nchar++;
		if(wspace && !isblank(c)) {
			wspace = 0;
			nword ++;
		}
		if(c=='\n') {
			nline++;
			wspace = 1;
		}
		if(isblank(c))
			wspace = 1;
	}
	fclose(fin);
	printf("%8zd %8zd %8zd\n", nline, nword, nchar);
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



/*************************************

	A remote server

***************************************/

#define REMOTE_SERVER_DEFAULT_PORT 20

/*
  The server's "global variables".
 */
struct __rs_globals
{
	/* a global flag */
	int quit;

	/* server related */
	port_t port;
	Tid_t listener;
	Fid_t listener_socket;

	/* Statistics */
	size_t active_conn;
	size_t total_conn;

	/* used so that each connection gets a unique id */
	size_t conn_id_counter;
	
	/* used to log connection messages */
	rlnode log;
	size_t logcount;
	
	/* Synchronize with active threads */
	Mutex mx;
	CondVar conn_done;
};

#define GS(name) (((struct __rs_globals*) __globals)->name)

/* forward decl */
static int rsrv_client(int sock, void* __globals);
static void log_message(void* __globals, const char* msg, ...)
	__attribute__((format(printf,2,3)));
static void log_init(void* __globals);
static void log_print(void* __globals);
static void log_truncate(void* __globals);

static int rsrv_listener_thread(int port, void* __globals);

/* the thread that accepts new connections */
static int rsrv_listener_thread(int port, void* __globals)
{
	Fid_t lsock = Socket(port);
	if(Listen(lsock) == -1) {
		printf("Cannot listen to the given port: %d\n", port);
		return -1;
	}
	GS(listener_socket) = lsock;

	/* Accept loop */
	while(1) {
		Fid_t sock = Accept(lsock);
		if(sock==NOFILE) {
			/* We failed! Check if we should quit */
			if(GS(quit)) return 0;
			log_message(__globals, "listener(port=%d): failed to accept!\n", port);
		} else {
			GS(active_conn)++;
			GS(total_conn)++;
			Tid_t t = CreateThread(rsrv_client, sock, __globals);
			ThreadDetach(t);
		}
	}
	return 0;
}


/*  The main server process */
int RemoteServer(size_t argc, const char** argv)
{
	/* Create the globals */
	struct __rs_globals __global_obj;
	struct __rs_globals *__globals = &__global_obj;

	GS(mx) = MUTEX_INIT;
	GS(conn_done) = COND_INIT;
	
	GS(quit) = 0;
	GS(port) = REMOTE_SERVER_DEFAULT_PORT;
	GS(active_conn) = 0;
	GS(total_conn) = 0;
	GS(conn_id_counter) = 0;

	log_init(__globals);

	/* Start a thread to listen on */
	GS(listener) = CreateThread(rsrv_listener_thread, GS(port), __globals);
	
	/* Enter the server console */
	char* linebuff = NULL;
	size_t lineblen = 0;
	FILE* fin = fidopen(0,"r");

	while(1) {
		printf("Type h for help, or a command: ");
		int rc;
		again:
		rc = getline(&linebuff, &lineblen, fin);
		if(rc==-1 && ferror(fin) && errno==EINTR) { 
			clearerr(fin); 
			goto again; 
		}
		if(rc==-1 && feof(fin)) {
			break;
		}

		assert(linebuff!=NULL);

		if(strcmp(linebuff, "q\n")==0) {
			/* Quit */
			GS(quit) = 1;
			printf("Quitting\n");
			Close(GS(listener_socket));
			ThreadJoin(GS(listener), NULL);

			Mutex_Lock(&GS(mx));
			while(GS(active_conn)>0) {
				printf("Waiting %zu connections ...\n", GS(active_conn));
				Cond_Wait(&GS(mx), &GS(conn_done));
			}
			Mutex_Unlock(&GS(mx));
			
			
			log_truncate(__globals);
			break;
		} else if(strcmp(linebuff, "s\n")==0) {
			/* Show statistics */
			printf("Connections: active=%4zd total=%4zd\n", 
				GS(active_conn), GS(total_conn));
		} else if(strcmp(linebuff, "h\n")==0) {
			printf("Commands: \n"
			       "q: quit the server\n"
			       "h: print this help\n"
			       "s: show statistics\n"
			       "l: show the log\n");
		} else if(strcmp(linebuff, "l\n")==0) {
			log_print(__globals);
		} else if(strcmp(linebuff, "\n")==0) {
		} else {
			printf("Unknown command: '%s'\n", linebuff);
		}
	}

	fclose(fin);
	free(linebuff);
	return 0;
}

typedef struct {
	rlnode node;
	char message[0];
} logrec;

/* log a message */
static void log_message(void* __globals, const char* msg, ...)
{
	/* put the log record in a memory buffer */
        char* buffer = NULL;
        size_t buffer_size;
        FILE* output = open_memstream(&buffer, &buffer_size);

	/* At the head of the buffer, reserve space for a logrec */
	fseek(output, sizeof(logrec), SEEK_SET);

	/* Add the message */
        va_list ap;
        va_start (ap, msg);
        vfprintf (output, msg, ap);
        va_end (ap);
        fclose(output);

	/* Append the record */
	logrec *rec = (logrec*) buffer;
	Mutex_Lock(& GS(mx));
	rlnode_new(& rec->node)->num = ++GS(logcount);
	rlist_push_back(& GS(log), & rec->node);
	Mutex_Unlock(& GS(mx));
}

/* init the log */
static void log_init(void* __globals)
{
	rlnode_init(& GS(log), NULL);
	GS(logcount)=0;
}

/* Print the log to the console */
static void log_print(void* __globals)
{
	Mutex_Lock(& GS(mx));
	for(rlnode* ptr = GS(log).next; ptr != &GS(log); ptr=ptr->next) {
		logrec *rec = (logrec*)ptr;
		printf("%6d: %s\n", rec->node.num, rec->message);
	}
	Mutex_Unlock(& GS(mx));
}

	
/* truncate the log */
static void log_truncate(void* __globals)
{
	rlnode list;
	rlnode_init(&list, NULL);
	
	Mutex_Lock(& GS(mx));
	rlist_append(& list, &GS(log));
	Mutex_Unlock(& GS(mx));

	/* Free the memory ! */
	while(list.next != &list) {
		rlnode *rec = rlist_pop_front(&list);
		free(rec);
	}
}



/* Helper to receive a message */
static int recv_message(Fid_t sock, void* buf, size_t len)
{
	size_t count = 0;
	while(count<len) {
		int rc = Read(sock, buf+count, len-count);
		if(rc<1) break;  /* Error or end of stream */
		count += rc;
	}
	return count==len;
}

/* Helper to execute a remote process */
static int rsrv_process(size_t argc, const char** argv)
{
	checkargs(2);
	Fid_t sock = atoi(argv[1]);

	/* Fix the streams */
	assert(sock!=0 && sock!=1);	
	Dup2(sock, 0);
	Dup2(sock, 1);
	Close(sock);

	/* (a) find the command */
	int c = getprog(2);
	if(c==-1) {
		/* This will appear in the rcli console */
		printf("Error in remote process: Command %s is not found\n", argv[2]);
		return -1;
	}
	Program proc = COMMANDS[c].prog;

	/* Execute */
	int exitstatus;
	WaitChild(Execute(proc, argc-2, argv+2), &exitstatus);
	return exitstatus;
}

/* this server thread serves a remote cliend */
static int rsrv_client(int sock, void* __globals)
{
	/* Get your client id */
	Mutex_Lock(&GS(mx));
	size_t ID = ++GS(conn_id_counter);
	Mutex_Unlock(&GS(mx));

	log_message(__globals, "Client[%6zu]: started", ID);
	
        /* Get the command from the client. The protocol is
	   [int argl, void* args] where argl is the length of
	   the subsequent message args.
	 */
	int argl;
	if(! recv_message(sock, &argl, sizeof(argl))) {
		log_message(__globals,
			    "Cliend[%6zu]: error in receiving request, aborting", ID);
		goto finish;
	}
		
	assert(argl>0 && argl <= 2048);
	{
		char args[argl];
		if(! recv_message(sock, args, argl)) {
			log_message(__globals,
				    "Cliend[%6zu]: error in receiving request, aborting", ID);
			goto finish;		
		}
		
		/* Prepare to execute subprocess */
		size_t argc = argscount(argl, args);	
		const char* argv[argc+2];
		argv[0] = "rsrv_process";
		char sock_value[32];
		sprintf(sock_value, "%d", sock);
		argv[1] = sock_value;
		argvunpack(argc, argv+2, argl, args);
	
		/* Now, execute the message in a new process */
		int exitstatus;
		Pid_t pid = Execute(rsrv_process, argc+2, argv);
		Close(sock);
		WaitChild(pid, &exitstatus);
	
		log_message(__globals, "Client[%6zu]: finished with status %d",
			    ID, exitstatus);
	}

finish:
	Mutex_Lock(&GS(mx));
	GS(active_conn)--;
	Cond_Broadcast(& GS(conn_done));
	Mutex_Unlock(&GS(mx));
	return 0;
}

/*********************
   the client program
************************/

/* helper for RemoteClient */
static void send_message(Fid_t sock, void* buf, size_t len)
{
	size_t count = 0;
	while(count<len) {
		int rc = Write(sock, buf+count, len-count);
		if(rc<1) break;  /* Error or End of stream */
		count += rc;
	}
	if(count!=len) {
		printf("In client: I/O error writing %zu bytes (%zu written)\n", len, count);
		Exit(1);
	}
}

/* the remote client program */
int RemoteClient(size_t argc, const char** argv)
{
	checkargs(1);
	
	/* Create a socket to the server */
	Fid_t sock = Socket(NOPORT);
	if(Connect(sock, REMOTE_SERVER_DEFAULT_PORT, 1000)==-1) {
		printf("Could not connect to the server\n");
		return -1;
	}

	assert(sock!=NOFILE);

	/* Make up the message */
	int argl = argvlen(argc-1, argv+1);
	char args[argl];
	argvpack(args, argc-1, argv+1);

	/* Send message */
	send_message(sock, &argl, sizeof(argl));
	send_message(sock, args, argl);
	ShutDown(sock, SHUTDOWN_WRITE);

	/* Read the server data and display */
	char c;
	FILE* fin = fidopen(sock, "r");
	FILE* fout = fidopen(1, "w");
	while((c=fgetc(fin))!=EOF) {
		fputc(c, fout);
	}
	fclose(fin);
	fclose(fout);
	return 0;
}



/*************************************

	A very simple shell for tinyos 

***************************************/


int process_builtin(int argc, const char** argv)
{
	if(strcmp(argv[0], "?")==0) {
		printf("Type 'help' for help, 'exit' to quit.\n");
		return 1;
	}
	return 0;
}


static inline Fid_t savefid(Fid_t fsaved)
{
	Fid_t savior = OpenNull();
	assert(savior!=NOFILE);
	Dup2(fsaved, savior);
	return savior;
}


int process_line(int argc, const char** argv)
{
	/* Split up into pipeline fragments */
	int Vargc[argc];
	Vargc[0]=0;
	const char** Vargv[argc];

	int frag=0;

	for(int i=0; i<argc; i++)
		if(strcmp(argv[i],"|")!=0) {
			Vargc[frag] ++;
			if(Vargc[frag]==1) 
				Vargv[frag] = argv+i;
		}
		else 
			Vargc[++frag] = 0;
	frag++;

	/* Check that no fragment is empty and each has a program */
	int comd[frag];
	for(int i=0; i<frag; i++) {
		if(Vargc[i]==0) {
			printf("Error: a pipeline fragment was empty.\n");
			return 0;
		}
		int c;
		for(c=0; COMMANDS[c].cmdname; c++)
			if(strcmp(COMMANDS[c].cmdname, Vargv[i][0])==0)
				break;
		if(! COMMANDS[c].cmdname) {
			printf("Command not found: '%s'\n", Vargv[i][0]);
			return 0;
		}
		comd[i] = c;
	}

	/* Construct pipeline */
	int child[frag];
	int savein, saveout;

	savein = savefid(0);
	saveout = savefid(1);

	pipe_t pipe;
	for(int i=0; i<frag; i++) {
		if(i<frag-1) {
			/* Not the last fragment, make a pipe */
			Pipe(& pipe);
			Dup2(pipe.write,1);
			Close(pipe.write);
		} else {
			/* Last fragment, restore saved 1 */
			Dup2(saveout, 1);
			Close(saveout);				
		}

		child[i] = Execute(COMMANDS[comd[i]].prog, Vargc[i], Vargv[i]);

		if(i<frag-1) {
			/* Not the last fragment, make a pipe */
			Dup2(pipe.read,0);
			Close(pipe.read);
		} else {
			/* Last fragment, restore saved 1 */
			Dup2(savein, 0);
			Close(savein);
		}
	}

	/* Wait for the children */
	for(int i=0; i<frag; i++) {
		int exitval;
		WaitChild(child[i], &exitval);
		if(exitval) 
			printf("%s exited with status %d\n", Vargv[i][0], exitval);						
	}

	return 1;
}



int Shell(size_t argc, const char** argv)
{
	int exitval = 0;
	char* cmdline = NULL;
	size_t cmdlinelen = 0;

	FILE *fin, *fout;
	fin = fidopen(0, "r");
	fout = fidopen(1, "w");		

	fprintf(fout,"Starting tinyos shell\nType 'help' for help, 'exit' to quit.\n");

	const int ARGN = 128;

	for(;;) {
		const char * argv[ARGN];
		char* pos;
		int argc;

		/* Read the command line */
		fprintf(fout, "%% "); 
		ssize_t rc;

		again:
		rc = getline(&cmdline, &cmdlinelen, fin);
		if(rc==-1 && ferror(fin) && errno==EINTR) { 
			clearerr(fin); 
			goto again; 
		}
		if(rc==-1 && feof(fin)) {
			break;
		}

		/* Break it up */
		for(argc=0; argc<ARGN-1; argc++) {
			argv[argc] = strtok_r((argc==0? cmdline : NULL), " \n\t", &pos);
			if(argv[argc]==NULL)
				break;
		}

		if(argc==0) continue;

		/* Check exit */
		if(strcmp(argv[0], "exit")==0) {
			if(argc>=2) 
				exitval = atoi(argv[1]);
			goto finished;
		}

		/* First check if command is builtin */
		if(process_builtin(argc,argv)) continue;

		/* Process the command */
		process_line(argc, argv);

	}
	fprintf(fout,"Exiting\n");
finished:
	free(cmdline);
	fclose(fin);
	fclose(fout);
	return exitval;
}



/*
 * This is the initial task, which starts all the other tasks (except for the idle task). 
 */
int boot_shell(int argl, void* args)
{
	int nshells = (GetTerminalDevices()>0) ? GetTerminalDevices() : 1;

	/* Find the shell */
	int shprog = getprog_byname("sh");

	if(GetTerminalDevices()) {
		fprintf(stderr, "Switching standard streams\n");
		tinyos_replace_stdio();
		for(int i=0; i<nshells; i++) {
			int fdin = OpenTerminal(i);
			if(fdin!=0) {  Dup2(fdin, 0 ); Close(fdin);  }
			int fdout = OpenTerminal(i);
			if(fdout!=1) {  Dup2(fdout, 1 ); Close(fdout);  }
			Execute(COMMANDS[shprog].prog, 1, & COMMANDS[shprog].cmdname );
			Close(0);
		}
		while( WaitChild(NOPROC, NULL)!=NOPROC ); /* Wait for all children */
		tinyos_restore_stdio();
	} else {
		fprintf(stderr, "Switching standard streams to pseudo console\n");
		tinyos_replace_stdio();

		for(int i=0; i<nshells; i++) {
			Close(0); Close(1);
			tinyos_pseudo_console();
			Execute(COMMANDS[shprog].prog, 1, & COMMANDS[shprog].cmdname );
		}
		while( WaitChild(NOPROC, NULL)!=NOPROC ); /* Wait for all children */

		tinyos_restore_stdio();

		//Execute(COMMANDS[shprog].prog, 1, & COMMANDS[shprog].cmdname );
		//while( WaitChild(NOPROC, NULL)!=NOPROC ); /* Wait for all children */
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


