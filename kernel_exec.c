
#include "kernel_exec.h"
#include "kernel_proc.h"
#include "kernel_fs.h"
#include "kernel_sys.h"


/*===================================================

	Generic execution

 ==================================================== */

/* Helper functions */
static Fid_t exec_program_open(const char* pathname)
{
	/* Stat the path name to make sure it is a regular file */
	struct Stat st;
	if(sys_Stat(pathname, &st)==-1) return NOFILE;

	if(st.st_type!=FSE_FILE) { set_errcode(ENOEXEC); return NOFILE; }

	/* Open the executable file */
	return sys_Open(pathname, OPEN_RDONLY);
}

static int exec_program_get_magic(Fid_t txtfid, char magic[2])
{
	int rc = sys_Read(txtfid, magic, 2);
	if(rc==-1) { return -1; }
	else {  assert(rc>=0); set_errcode(ENOEXEC); return -1; }
	if(sys_Seek(txtfid, 0, 0)!=0) { return -1; }
	return 0;
}

static void exec_program_close(Fid_t* fp) { if(*fp!=NOFILE) sys_Close(*fp); }


/*
	The main dispatch routine to executable formats	
 */
int exec_program(struct exec_args* xargs, const char* pathname, 
	const char* const argv[], char* const envp[], int iter)
{
	/* Open this with a cleanup option, so that in case of error we do not leak fids */
	Fid_t fid  __attribute__((cleanup(exec_program_close))) 
		= exec_program_open(pathname);
	if(fid==NOFILE) { return -1; }

	/* Get the magic number of the executable file */
	char magic[2];
	if(exec_program_get_magic(fid, magic)) return -1;

	/* Find the magic number in the exec_formats */
	struct binfmt* xfmt = get_binfmt(magic);
	if(xfmt==NULL) { set_errcode(ENOEXEC); return -1; }

	/* Get the format recipe for executor. Dup is needed to ensure */
	int txtfid = sys_Dup(fid);
	return xfmt->exec_func(xargs, txtfid, pathname, argv, envp, iter+1);
}


/*===================================================

	The "Program/argc/argv" format

 ==================================================== */

typedef	int (*Program)(size_t, const char**);


struct bf_program
{
	char magic[2];
	Program program;
};


int bf_program_task(int argl, void* args)
{
	Program p;
	memcpy(&p, args, sizeof(p));
	size_t argc = argscount(argl, args);
	const char* argv[argc];
	argvunpack(argc, argv, argl, args);
	return p(argc, argv);
}


int bf_program_exec(struct exec_args* xargs, Fid_t f, const char* pathname, const char* const argv[], char* const envp[], int iter)
{
	struct bf_program prog;

	int rc = Read(f, (void*) &prog, sizeof(prog));
	Close(f);
	if(rc!=sizeof(prog)) return -1;

	/* Pack arguments */
	size_t argc = 0; { const char* const * v=argv; while(*v++) argc++; }
	size_t argl = argvlen(argc, argv);
	const size_t psize = sizeof(prog.program);
	void* args = malloc(psize+argl);
	memcpy(args, &prog.program, psize);
	argvpack(args+psize, argc, argv);

	xargs->task = bf_program_task;
	xargs->argl = argl;
	xargs->args = args;
	return 0;
}

struct binfmt bf_program = {
	.magic = {'#','P'},
	.exec_func = bf_program_exec
};


/*===================================================

	The binfmt table and binfs

 ==================================================== */

/*
	Search into binfmt_table and return an entry with a matching 
	magic.
 */
struct binfmt* get_binfmt(const char magic[2])
{
	for(unsigned int i=0; binfmt_table[i]; i++) {
		const char* fmagic = binfmt_table[i]->magic;
		if(memcmp(fmagic, magic, 2)==0) return binfmt_table[i];
	}
	/* Unknown magic */
	return NULL;
}


/* The binfmt table */
struct binfmt* binfmt_table[] = {
	&bf_program,
	NULL
};



/*===================================================

	The binary loader

 ==================================================== */



