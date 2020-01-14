
#include "sysutils.h"



int ls(size_t argc, const char** argv)
{
	check_help(argc,argv,
"Usage: ls [OPTION]... [FILE]...\n"
"List information about the FILEs (the current directory by default).\n"
"Options:\n"
"  -a          do not ignore entries starting with .\n"
"  -F          append indicator (one of */=>@|) to entries\n"
"  -l          use a long listing format \n"
"  -s          print the allocated size of each file, in blocks\n"
			);

	const char* list[argc];
	int nlist = 0;

	const char* files[argc];
	int nfiles = 0;

	int opt_long = 0;
	int opt_size = 0;
	int opt_all = 0;
	int opt_type = 0;

	struct Stat st;

	void set_options(const char* opt) {
		for(; *opt; ++opt) {
			switch(*opt) {
				case 'a':  opt_all=1; break;
				case 'l':  opt_long=1; break;
				case 's':  opt_size=1; break;
				case 'F':  opt_type=1; break;
				default:
					printf("Invalid option: '%c'. Type ls -h  for help\n", *opt);
			}
		}
	}

	int nonopt = 0;
	for(int i=1; i<argc;i++) {
		if(argv[i][0]=='-' && argv[i][1]!='\0') set_options(argv[i]+1);
		else { 
			nonopt++;
			int rc = Stat(argv[i], &st);
			if(rc==-1) { PError(argv[i]); continue; }
			if(st.st_type==FSE_DIR) { list[nlist++] = argv[i]; }
			else { files[nfiles++] = argv[i]; }
		}
	}
	if(nonopt==0) { list[0]="."; nlist=1; }

	struct tm cur_time;
	GetTimeOfDay(&cur_time, NULL);	

	/*
		Helper: print the entry for a single file
	 */
	void ls_print(const char* fullpath, const char* name) 
	{
		if(Stat(fullpath, &st)!=0) { PError("stat(%s): ", fullpath); return; }

		/* print size */
		if(opt_size) printf("%3d ", (st.st_size+1023)/1024);

		/* print long info */
		if(opt_long) {
			const char* T="ufso";
			switch(st.st_type) { 
				case FSE_DIR: T="dir"; break;
				case FSE_FILE: T="file"; break;
				case FSE_DEV: T="dev"; break;
			}
			//  generate short time field for mtime
			char mtime_buf[32];

			struct tm tmtime;
			LocalTime(st.st_modify, &tmtime, NULL);
			if(tmtime.tm_year == cur_time.tm_year)
				strftime(mtime_buf, 32, "%b %d %R", &tmtime);
			else
				strftime(mtime_buf, 32, "%b %d   %Y", &tmtime);

			printf("%4s  %2d %6zu  %s  ", T, st.st_nlink, st.st_size, mtime_buf);
		}

		/* print name */
		printf("%s", name);

		/* print file type */
		if(opt_type) {
			const char* T="?";
			switch(st.st_type) { 
				case FSE_DIR: T="/"; break;
				case FSE_FILE: T=""; break;
				case FSE_DEV: T=">"; break;
			}
			printf("%s", T);
		}

		/* done */
		printf("\n");
	}


	/* 
		Construct the pathname for a file in a directory and call ls_print 
	 */
	void ls_print_in_dir(const char* dirname, const size_t dirnamelen, char name[])
	{
		size_t nlen = strnlen(name, MAX_NAME_LENGTH);
		if(dirnamelen+1+nlen>MAX_PATHNAME) { 
			 /* Name is too long to stat! */
			printf("%.*s: path was too long\n", MAX_NAME_LENGTH, name);
			return;
		}
		char path[MAX_PATHNAME+1];
		strcpy(path, dirname);
		strcat(path+dirnamelen, "/");
		strncat(path+dirnamelen+1,name, MAX_NAME_LENGTH);

		ls_print(path,name);
	}


	/*
		Main printing loop

	*/

	for(int i=0; i<nfiles;i++) ls_print(files[i], files[i]);
	for(int i=0; i<nlist; i++) {
		if(nfiles > 0 || i>0) printf("\n");
		if(nfiles > 0 || nlist>1) printf("%s:\n", list[i]);

		char** dirlist;

		int hidden_filter(const char* name) { return opt_all || name[0]!='.';}

		if(ScanDir(list[i], &dirlist, hidden_filter)>=0) {
			const size_t dirnamelen = strlen(list[i]);
			for(char** p = dirlist; *p; p++) {
				ls_print_in_dir(list[i], dirnamelen, *p);
			}
			free(dirlist);
		} else {
			PError("Error scanning %s: ",list[i]);
		}
	}

	return 0;
}

REGISTER_PROGRAM(ls)
