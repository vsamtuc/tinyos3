
#include "tinyoslib.h"



/*************************************

	A very simple shell for tinyos 

***************************************/

/* Builtin commands */

void builtin_help(size_t argc, const char** argv);



void builtin_cd(size_t argc, const char** argv)
{
	if(ChDir(argv[1])!=0) { PError(argv[0]); }
}

/*
	Note: currently this has memory leaks, since we do not 
	have malloced memory clean up on Exec!!
 */
void builtin_exec(size_t argc, const char** argv)
{
	assert(argc>=2);

	/* Execute */
	int rc = Exec(argv[1], argv+2, NULL);
	if(rc) { PError(argv[0]);  }
}


struct {const char* name; void (*util)(size_t, const char**); uint nargs; const char* help; }
BUILTIN[] = {
	{"?", builtin_help, 0, "Print help for the builtin shell commands"},
	{"cd", builtin_cd, 1, "Change current directory"},
	{"exec", builtin_exec, 1, "Execute a program instead of this shell"},
	{NULL,NULL,0,NULL}
};

void builtin_help(size_t argc, const char** argv)
{
	const char* sep = "---------------------------------------------------";
	printf("The shell built-in commands\n%s\n", sep);
	printf("no.  %-15s no.of.args   help \n", "Command");
	printf("%s\n",sep);
	for(int c=0; BUILTIN[c].name; c++) {
		printf("%3d  %-20s %2d   %s\n", c, BUILTIN[c].name, BUILTIN[c].nargs, BUILTIN[c].help);
	}
}


int process_builtin(int argc, const char** argv)
{
	int c;
	for(c=0; BUILTIN[c].name!=NULL; c++) {
		if(strcmp(argv[0], BUILTIN[c].name)==0) break;
	}
	void (*util)(size_t, const char**) = BUILTIN[c].util;
	if(util==NULL) return 0;
	if(argc < BUILTIN[c].nargs+1)
		printf("Insufficient arguments for %s:  %d expected, %zd given.\n", argv[0], BUILTIN[c].nargs, argc-1);
	else
		util(argc,argv); 
	return 1; 
}


int p_file_exists(const char* pathname)
{
	struct Stat st;
	if(Stat(pathname, &st)) return 0;
	return st.st_type==FSE_FILE;
}


const char* PATH[] = { ".", "/bin", NULL };

char* alloc_progpath(const char* file, const char* PATH[])
{
	if(strlen(file)==0) return NULL;

	/* Absolute file, ignore PATH */
	if(file[0]=='/') {
		if(p_file_exists(file)) return strdup(file);
		return NULL;
	}

	/* Relative file, try PATH */
	int flen = strlen(file);
	for(const char** p=PATH; *p; p++) {
		const char* path = *p;
		int plen = strlen(path);
		if(flen+plen+2 >= MAX_PATHNAME) continue;
		char fpath[flen+plen+2];
		strcpy(fpath, path);
		fpath[plen] = '/';
		strcpy(fpath+plen+1, file);
		if(p_file_exists(fpath)) return strdup(fpath);
	}
	return NULL;
}


void cleanup_Close(Fid_t* pfid) { Close(*pfid); }
#define CLOSE __attribute__((cleanup(cleanup_Close)))


struct redir_descriptor {  const char* pathname;  int flags; };


Fid_t open_redir(struct redir_descriptor* redir, Fid_t stdfd)
{
	Fid_t fid = (redir->pathname==NULL)? 
		Dup(stdfd) : 
		Open(redir->pathname, redir->flags);

	if(fid==NOFILE) {
		char errbuf[80];
		if(redir->pathname) {
			printf("Could not open '%s' for %s: %s\n", 
				redir->pathname,
				(stdfd) ? "output" : "input",
				strerror_r(GetError(), errbuf,80));		
		} else {
			printf("Could not Dup(%d): %s\n", 
				stdfd,
				strerror_r(GetError(), errbuf,80));					
		}
	}
	return fid;
}



int launch_fragment(int argl, void* args)
{
	packer p = UNPACKER(argl, args);
	Fid_t sin, sout; 
	UNPACK(&p, sin);
	UNPACK(&p, sout);

	char* progname = strget(&p);

	size_t argc; UNPACK(&p, argc);

	const char* argv[argc+1];
	unpackz(&p, argc, argv);

	if(sin!=0) { Dup2(sin, 0); Close(sin); }
	if(sout!=1) { Dup2(sout,1); Close(sout); }

	return Exec(progname, argv, NULL);
}


Pid_t exec_fragment(char* progname, int argc, const char** argv, Fid_t sin, Fid_t sout)
{
	packer p PACKER_CLEANUP = PACKER;
	PACK(&p, sin);
	PACK(&p, sout);
	strpack(&p, progname);
	packv(&p, argc, argv);

	Pid_t ret = Spawn(launch_fragment, p.pos, p.buffer);
	Close(sin); Close(sout);
	return ret;
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

	void cleanup_alloc(char* (*arr)[]) {
		for(int i=0;i<frag;i++) {
			free((*arr)[i]);
		}
	}


	/* Check that no fragment is empty, each has a program and any
	   redirections are successful */
	char* comd[frag]  __attribute__((cleanup(cleanup_alloc)));
	for(int i=0;i<frag;i++) comd[i]=NULL;

	/* These records will hold the redirections if any */
	struct redir_descriptor input_redir = {NULL, 0};
	struct redir_descriptor output_redir = {NULL, 0};

	for(int i=0; i<frag; i++) {
		/* 
			Parse redirections
			Note that a redirection is a pair of arguments like
			> somepath  or < somepath or >> somepath. 

			When we recognize such a pair, we remove them from the 
			argument list.

			Also, redirection of input is only allowed at the 1st 
			component, and of output at the last.

			We pass information about these redirections through
			struct redir_description.
		 */
		for(int j=0; j<Vargc[i]; j++) {

			int allowed_i;
			const char* direction;
			struct redir_descriptor* redir;

			if(strcmp(Vargv[i][j], "<")==0) {
				allowed_i = 0;
				direction = "Input";
				input_redir.flags = OPEN_RDONLY;
				redir = &input_redir;
			} else if(strcmp(Vargv[i][j], ">")==0) {
				allowed_i = frag-1;
				direction = "Output";
				output_redir.flags = OPEN_WRONLY|OPEN_CREAT|OPEN_TRUNC;
				redir = &output_redir;
			} else if(strcmp(Vargv[i][j], ">>")==0) {
				allowed_i = frag-1;
				direction = "Output";
				output_redir.flags = OPEN_WRONLY|OPEN_APPEND|OPEN_CREAT;
				redir = &output_redir;
			} else 
				continue;

			if(i!=allowed_i) { 
				printf("%s redirection not allowed in fragment %d of the pipeline\n", direction, i+1); 
				return 0; 
			}

			if(j+1>=Vargc[i]) {
				printf("%s redirection missing file\n");
				return 0; 					
			} else 
				redir->pathname = Vargv[i][j+1];
			

			/* ok, eat the two arguments */
			for(int k=j+2; k<Vargc[i]; k++) {
				Vargv[i][k-2] = Vargv[i][k];
			}
			Vargc[i] -= 2;

			/* Loop again for the same j ! */
			j -= 1;
		}

		/* Check that fragment is not empty (after redirections have been removed) */
		if(Vargc[i]==0) {
			printf("Error: a pipeline fragment was empty.\n");
			return 0;
		}

		/* Check fragment's program (argv[0]) */
		char* progpath = alloc_progpath(Vargv[i][0], PATH);
		if(! progpath) {
			printf("%s: command not found:\n", Vargv[i][0]);
			return 0;
		}
		comd[i] = progpath;

	}


	/* Try to open the redirections, fail early if not possible */
	Fid_t plin CLOSE = open_redir(&input_redir, 0);
	if(plin==NOFILE) return 0;

	Fid_t plout CLOSE = open_redir(&output_redir, 1);
	if(plout==NOFILE) return 0;

	/* Construct pipeline */
	Pid_t child[frag];
	for(int i=0; i<frag; i++) {
		Fid_t sin = plin;
		Fid_t sout;

		/* If not at the last fragment, make a pipe */
		if(i<frag-1) {
			pipe_t pipe;
			if(Pipe(& pipe)==-1) return 0;
			sout = pipe.write;
			plin = pipe.read;
		} else {
			sout = plout;
			plin = plout = NOFILE;
		}

		child[i] = exec_fragment(comd[i], Vargc[i], Vargv[i], sin, sout);
		if(child[i]==NOPROC) return 0;
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



int sh(size_t argc, const char** argv)
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
		char CWD[100];
		if(GetCwd(CWD,100)!=0) { sprintf(CWD, "(error=%d)", GetError() ); }
		fprintf(fout, "%s%% ", CWD); 
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

REGISTER_PROGRAM(sh)

TOS_REGISTRY