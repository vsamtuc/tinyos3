

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio_ext.h>

#include "util.h"
#include "tinyos.h"
#include "tinyoslib.h"



static ssize_t tinyos_fid_read(void *cookie, char *buf, size_t size)
{
	return Read(*(Fid_t*)cookie, buf, size); 
}

static ssize_t tinyos_fid_write(void *cookie, const char *buf, size_t size)
{
	int ret = Write(*(Fid_t*)cookie, buf, size); 
	return (ret<0) ? 0 : ret;
}

static int tinyos_fid_close(void* cookie)
{
	free(cookie);
	return 0;
}

static cookie_io_functions_t  tinyos_fid_functions =
{
	tinyos_fid_read,
	tinyos_fid_write,
	NULL,
	tinyos_fid_close
};

static FILE* get_std_stream(int fid, const char* mode)
{
	FILE* term = fidopen(fid, mode);
	assert(term);
	/* This is glibc-specific and tunrs off fstream locking */
	__fsetlocking(term, FSETLOCKING_BYCALLER);	
	return term;
}


FILE* fidopen(Fid_t fid, const char* mode)
{

	Fid_t* fidloc = (Fid_t *) malloc(sizeof(Fid_t));
	* fidloc = fid;
	FILE* f = fopencookie(fidloc, mode, tinyos_fid_functions);

	CHECKRC(setvbuf(f, NULL, _IONBF, 0));
	return f;
}

FILE *saved_in = NULL, *saved_out = NULL;


void tinyos_replace_stdio()
{
	assert(saved_in == NULL);
	assert(saved_out == NULL);
	//if(GetTerminalDevices()==0) return;
	setbuf(stdout, NULL);

	FILE* termin = get_std_stream(0, "r");
	FILE* termout = get_std_stream(1, "w");

	saved_in = stdin;
	saved_out = stdout;

	stdin = termin;
	stdout = termout;
}

void tinyos_restore_stdio()
{
	if(saved_out == NULL)  return;	

	fclose(stdin);
	fclose(stdout);

	stdin = saved_in; 
	stdout = saved_out;

	saved_in = saved_out = NULL;
}



static int exec_wrapper(int argl, void* args)
{
	/* unpack the program pointer */
	Program prog;

	/* unpack the prog pointer */
	memcpy(&prog, args, sizeof(prog));

	argl -= sizeof(prog);
	args += sizeof(prog);

	/* unpack the string vector */
	size_t argc = argscount(argl, args);
	const char* argv[argc];
	argvunpack(argc, argv, argl, args);

	/* Make the call */
	return prog(argc, argv);
}


int ParseProcInfo(procinfo* pinfo, Program* prog, int argc, const char** argv )
{
	if(pinfo->main_task != exec_wrapper)
		/* We do not recognize the format! */
		return -1;

	if(pinfo->argl > PROCINFO_MAX_ARGS_SIZE) 
		/* The full argument is not available */
		return -1;

	int argl = pinfo->argl;
	void* args = pinfo->args;

	/* unpack the program pointer */
	if(prog) memcpy(&prog, args, sizeof(prog));

	argl -= sizeof(Program*);
	args += sizeof(Program*);

	/* unpack the string vector */
	size_t N = argscount(argl, args);
	if(argv) {
		if(argc>N)
			argc = N;
		argvunpack(argc, argv, argl, args);
	}

	return N;		
}



int Execute(Program prog, size_t argc, const char** argv)
{
	/* We will pack the prog pointer and the arguments to 
	  an argument buffer.
	  */

	/* compute the argument buffer size */
	size_t argl = argvlen(argc, argv) + sizeof(prog);

	/* allocate the buffer */
	char args[argl];

	/* put the pointer at the start */
	memcpy(args, &prog, sizeof(prog));

	/* add the string vector */
	argvpack(args+sizeof(prog), argc, argv);

	/* Execute the process */
	return Exec(exec_wrapper, argl, args);
}



void BarrierSync(barrier* bar, unsigned int n)
{
	assert(n>0);
	Mutex_Lock(& bar->mx);

	int epoch = bar->epoch;
	assert(bar->count < n);

	bar->count ++;
	if(bar->count == n) {
		bar->epoch ++;
		bar->count = 0;
		Cond_Broadcast(&bar->cv);
	}

	while(epoch == bar->epoch)
		Cond_Wait(&bar->mx, &bar->cv);

	Mutex_Unlock(& bar->mx);
}


