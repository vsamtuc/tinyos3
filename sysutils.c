
#include <ctype.h>

#include "sysutils.h"



void check_help(size_t argc, const char** argv, const char* help)
{
	for(size_t i=1; i<argc; i++) 
		if(strcmp(argv[i],"-h")==0) { printf(help); Exit(0); }
}



int mkdir(size_t argc, const char** argv)
{
	checkargs(1);
	check_help(argc, argv,
"Usage: mkdir DIRECTORY...\n"
"Create the DIRECTORY(ies), if they do not already exist.\n"
		);
	for(int i=1; i<argc; i++) 
		if(MkDir(argv[i])!=0) {
			PError(argv[0]);
			return 1;
		}
	return 0;
}

int rmdir(size_t argc, const char** argv)
{
	checkargs(1);
	check_help(argc, argv,
"Usage: rmdir DIRECTORY...\n"
"Remove the DIRECTORY(ies), if they are empty.\n"
		);
	for(int i=1; i<argc; i++) 
		if(RmDir(argv[i])!=0) {
			PError(argv[0]);
			return 1;
		}
	return 0;
}


int stat(size_t argc, const char** argv)
{
	checkargs(1);
	check_help(argc, argv,
"Usage: stat FILE...\n"
"Display file status.\n"
 	);

	const char* file_type(Fse_type type) {
		switch(type) {
			case FSE_DIR: return "directory";
			case FSE_FILE: return "regular file";
			case FSE_DEV: return "device";
			default: return "unknown type";
		}
	}

	void print_time(const char* label, timestamp_t ts) {
		time_t T = ts / 1000000;
		long usec = ts % 1000000;
		struct tm tm;
		char tbuf[64];
		strftime(tbuf, 64, "%F %T", localtime_r(&T, &tm));
		printf("%s: %s.%06lu\n", label, tbuf, usec);
	}

	for(int i=1; i<argc; i++) {
		struct Stat st;
		if(Stat(argv[i], &st)!=0) {
			PError(argv[0]);
			continue;
		}
		printf("File: %s\n", argv[i]);
		printf("Size: %-8lu\tBlocks: %-8lu\t\tIO Block: %-5lu\t%s\n",
			st.st_size, st.st_blocks, st.st_blksize, file_type(st.st_type));
		printf("Device: %3u/%-3u\tInode: %-21lu\tLinks: %-8u", 
			DEV_MAJOR(st.st_dev), DEV_MINOR(st.st_dev),
			st.st_ino, st.st_nlink);

		if(st.st_type==FSE_DEV)
			printf("\tDevice type: %3u/%-3u", DEV_MAJOR(st.st_rdev), DEV_MINOR(st.st_rdev));
		printf("\n");

		print_time("Access", st.st_access);
		print_time("Modify", st.st_modify);
		print_time("Change", st.st_change);
	}
	return 0;	
}


int statfs(size_t argc, const char** argv)
{
	checkargs(1);
	check_help(argc, argv,
"Usage: statfs FILE...\n"
"Display file system status for file.\n"
 	);

	for(int i=1; i<argc; i++) {
		struct StatFs st;
		if(StatFs(argv[i], &st)!=0) {
			PError(argv[0]);
			continue;
		}

		printf("File: %s\n", argv[i]);

		printf("Device: %3u/%-3u\tType: %12s\tRoot: %lx\n",
			DEV_MAJOR(st.fs_dev), DEV_MINOR(st.fs_dev),
			st.fs_fsys,
			st.fs_root);
		printf("Blocks Total: %6lu\tUsed: %6lu\n", st.fs_blocks, st.fs_bused);
		printf("Inodes Total: %6lu\tUsed: %6lu\n", st.fs_files, st.fs_fused);
	}
	return 0;	
}




int ln(size_t argc, const char** argv)
{
	checkargs(2);
	check_help(argc, argv,
"Usage: ln TARGET LINK_NAME\n"
"Create a link to TARGET with the name LINK_NAME.\n"
 	);
	if(Link(argv[1], argv[2])!=0) { PError(argv[0]); return 1; }
	return 0;
}

int rm(size_t argc, const char** argv)
{
	checkargs(1);
	check_help(argc, argv,
"Usage: rm [FILE]...\n"
"Remove (unlink) the FILE(s).\n"
 	);
	for(int i=1; i<argc; i++) 
		if(Unlink(argv[i])!=0) { PError(argv[0]); return 1; }
	return 0;
}

int kill(size_t argc, const char** argv)
{
	checkargs(1);
	check_help(argc, argv,
"Usage: kill pid\n"
"Send a kill signal to the process.\n"
 	);
	Pid_t pid = atoi(argv[1]);
	int rc = Kill(pid);
	if(rc==-1) { PError(argv[0]); return 1; }
	return 0;
}

int cat(size_t argc, const char** argv)
{
	check_help(argc, argv,
"Usage: cat [FILE]...\n"
"Concatenate FILE(s) to standard output.\n"
 	);

	void output(Fid_t fid) {
		char buffer[8192];
		while(1) {
			int bsize = Read(fid, buffer, 8192);
			if(bsize==0) break;
			if(bsize==-1) { PError(argv[0]); Exit(1); }

			for(int pos=0; pos<bsize; ) {
				int wb = Write(1, buffer+pos, bsize-pos);
				if(wb==-1) { PError(argv[0]); Exit(1); }
				pos += wb;
			}
		}
	}

	if(argc == 1) {
		/* just redirect stdin to stdout */
		output(0); return 0;
	}

	for(int a = 1; a < argc; a++) {
		Fid_t fid = Open(argv[a], OPEN_RDONLY);
		if(fid==NOFILE) { PError(argv[0]); Exit(1); }
		output(fid);
		Close(fid);
	}
	return 0;
}


int df(size_t argc, const char** argv)
{
	check_help(argc, argv,
"Usage: df\n"
"Show a list of the current mounted file systems.\n"
 	);

	Fid_t mtab = Open("/dev/mnttab", OPEN_RDONLY);
	if(mtab==NOFILE) { PError(argv[0]); return 1; }

	FILE* fmtab = fidopen(mtab,"r");
	printf("%-7s %-8s %10s %10s %-4s %s\n", 
		"Device", "Filesys", "Blocks", "Used", "Use%", "Mounted on");
	while(! feof(fmtab)) {
		char mount_point[MAX_PATHNAME];
		Dev_t device;
		char SYSFS[MAX_NAME_LENGTH];
		int rc = fscanf(fmtab, "%s %u %s", mount_point, &device, SYSFS);
		if(rc==EOF) break;
		if(rc!=3) { PError("Unparseable mnttab"); return 1; }

		struct StatFs st;
		if(StatFs(mount_point, &st)!=0) { PError(mount_point); continue; }

		char usage[20];
		if(st.fs_blocks)
			snprintf(usage,20, "%3.0f%%", 
				(100.0*st.fs_bused)/(st.fs_blocks));
		else
			snprintf(usage,20,"-");

		printf("%3u/%-3u %-8s %10u %10u %4s %s\n", 
			DEV_MAJOR(st.fs_dev), DEV_MINOR(st.fs_dev),
			st.fs_fsys, 
			st.fs_blocks, st.fs_bused, usage,
			mount_point);
	}
	return 0;
}


int date(size_t argc, const char** argv)
{
	check_help(argc, argv,
"Usage: date\n"
"Show the current date and time.\n"
 	);

	/* Break down the time */
	struct tm tm;
	unsigned long usec;
	int rc = GetTimeOfDay(&tm, &usec);
	if(rc) { PError("%s: reading clock: ",argv[0]); return 1; }

	/* Print it */
	char buf[64];
	strftime(buf, 64, "%c", &tm);

	printf("%s %7lu usec\n", buf, usec);
	return 0;
}




int ps(size_t argc, const char** argv)
{
	check_help(argc, argv,
"Usage: ps\n"
"Show a list of the current processes.\n"
 	);

	Fid_t pi = Open("/dev/procinfo", OPEN_RDONLY);
	if(pi==NOFILE) { PError(argv[0]); return 1; }
	FILE* fpi = fidopen(pi,"r");
	printf("%-5s %-5s %-18s %s\n", "PID", "PPID", "Status(wchan)", "Cmdline");
	while(! feof(fpi)) {
		Pid_t pid=0, ppid=0;
		char status, wchan[16]="none";
		Task task = NULL;
		unsigned int argl = 0;

		int rc = fscanf(fpi,"%u:%u:%c:%16[0-9A-Za-z_-]:%p:%u:", &pid, &ppid, &status, wchan, &task, &argl);
		if(rc==EOF) break;
		assert(rc == 6);

		printf("%5d %5d %c %-16s ", pid, ppid, status, wchan);

		char args[argl];
		if(argl != 0) {	
			char format[12];
			sprintf(format,"%%%uc\\n", argl);
			int rc2 = fscanf(fpi, format, args);
			assert(rc2==1);
		}

		int argc = ParseProgArgs(task, argl,args, NULL, 0, NULL);
		if(argc>=0) {
			Program prog = NULL;
			const char* argv[argc];
			ParseProgArgs(task,argl,args, &prog, argc, argv);
			for(unsigned int j=0;j<argc; j++) 
				printf("%s ",argv[j]);
		} else {
			printf("-");
		}

		printf("\n");
	}

	fclose(fpi);
	return 0;
}

int mount(size_t argc, const char** argv)
{
	checkargs(2);
	check_help(argc, argv,
"Usage: mount <moutpoint> <fstype>\n"
"Mount a file system.\n"
 	);

	int rc = Mount(NO_DEVICE, argv[1], argv[2], 0, NULL);
	if(rc!=0)
		PError(argv[0]);
	return rc;
}

int umount(size_t argc, const char** argv)
{
	checkargs(1);
	check_help(argc, argv,
"Usage: umount mpoint\n"
"Unount a file system.\n"
 	);

	int rc = Umount(argv[1]);
	if(rc!=0)
		PError(argv[0]);
	return rc;
}



int repeat(size_t argc, const char** argv)
{
	checkargs(2);
	int times = atoi(argv[1]);


	/* Find program */
	if(times<0) {
		printf("Cannot execute a negative number of times!\n");
	}
	while(times--) {
		SpawnProgram(argv[2], argc-2, argv+2);
		WaitChild(NOPROC,NULL);
	}
	return 0;
}


int runterm(size_t argc, const char** argv)
{
	checkargs(2);

	/* Change our own terminal, so that the child inherits it. */
	const char* term = argv[1];
	Fid_t termfid = Open(term, OPEN_RDWR);
	if(termfid==NOFILE) {
		PError("Error opening '%s': ", term);
		return 1;
	}
	if(termfid!=0) {
		Dup2(termfid, 0);
		Close(termfid);
	}
	Dup2(0, 1);  /* use the same stream for stdout */

	return Exec(argv[2], argv+2, NULL)==NOPROC;
}




int wc(size_t argc, const char** argv)
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


int cap(size_t argc, const char** argv)
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


int echo(size_t argc, const char** argv)
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


int lcase(size_t argc, const char** argv)
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


int lenum(size_t argc, const char** argv)
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

int more(size_t argc, const char** argv)
{
	char* _line = NULL;
	size_t _lno = 0;

	int page = 25;
	if(argc>=2) {
		page = atoi(argv[1]);
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


int help(size_t argc, const char** argv)
{
	printf("This is a simple shell for tinyos.\n\
\n\
You can run some simple commands. Every command takes \n\
a number of arguments separated by spaces. The list of commands and the\n\
number of arguments for each command is shown by\n\
typing 'list'. Each command executes in a new process.\n\n\
You can also construct pipelines of commands, using '|' to\n\
separate the pieces of a pipeline.\n\n\
In addition, the shell supports some builtin commands. These\n\
can be shown if you type '?' on the command line. Note that\n\
builtin commands cannot form pipelines.\n\n\
When you are tired of playing, type 'exit' to quit.\n\
");
	return 0;
}



REGISTER_PROGRAM(mkdir)
REGISTER_PROGRAM(rmdir)
REGISTER_PROGRAM(stat)
REGISTER_PROGRAM(statfs)
REGISTER_PROGRAM(ln)
REGISTER_PROGRAM(rm)
REGISTER_PROGRAM(kill)
REGISTER_PROGRAM(cat)
REGISTER_PROGRAM(df)
REGISTER_PROGRAM(date)
REGISTER_PROGRAM(ps)
REGISTER_PROGRAM(umount)
REGISTER_PROGRAM(mount)
REGISTER_PROGRAM(repeat)
REGISTER_PROGRAM(wc)

REGISTER_PROGRAM(cap)
REGISTER_PROGRAM(echo)
REGISTER_PROGRAM(lcase)
REGISTER_PROGRAM(lenum)
REGISTER_PROGRAM(more)

REGISTER_PROGRAM(help)

