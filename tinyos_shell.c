

#include "tinyoslib.h"
#include "bios.h"

/*
 * This is the initial task, which starts all the other tasks (except for the idle task). 
 */
int boot_shell(int argl, void* args)
{
	CHECK(Mount(0, "/", "tmpfs", 0, NULL));
	CHECK(MkDir("/dev"));
	CHECK(Mount(0, "/dev", "devfs", 0, NULL));
	CHECK(MkDir("/bin"));
	CHECK(Mount(0, "/bin", "binfs", 0, NULL));

	CHECK(dll_load("sysutils.so"));

	/* Find the shell */
	int nshells = 0;

	fprintf(stderr, "Switching standard streams\n");
	tinyos_replace_stdio();

	const char* argv[2] = { "sh", NULL };

	int fork_shell(int l, void* args)
	{
		if(args) {
			int fd = Open(args, OPEN_RDWR);
			if(fd==NOFILE) return 1;

			if(fd!=0) { Dup2(fd,0); Close(fd); }
			Dup2(0,1);
		}

		return Exec("/bin/sh", argv, NULL );
	}

	for(int i=1; i<=4; i++) {
		char termpath[32];
		snprintf(termpath,32,"/dev/serial%d",i);
		struct Stat st;
		if(Stat(termpath, &st)==-1) continue;
		Spawn(fork_shell, 32, termpath);
		nshells++;
	}

	if(nshells==0) {
		Close(0); Close(1);
		tinyos_pseudo_console();
		SpawnProgram("/bin/sh", 1, argv);
	}

	while( WaitChild(NOPROC, NULL)!=NOPROC ); /* Wait for all children */
	tinyos_restore_stdio();

	return 0;
}

/****************************************************/

void usage(const char* pname)
{
  printf("usage:\n  %s <ncores> <nterm>\n\n  \
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
  vm_config vmc;
  vm_configure(&vmc, NULL, ncores, nterm);

  boot(&vmc, boot_shell, 0, NULL);
  printf("*** TinyOS halted. Bye!\n");

  return 0;
}


