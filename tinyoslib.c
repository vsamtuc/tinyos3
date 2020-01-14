

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
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
	/* There is a bug in the custom stream code where it is assumed that all
	   the bytes given will be written. Therefore, we cannot simply pass the
	   arguments to Write(). Instead, we should write as much as we can, until 
	   error! 
	*/
	Fid_t fid = *(Fid_t*)cookie;
	int nbytes = 0;
	while(nbytes<size) {
		int ret = Write(fid, buf+nbytes, size-nbytes);
		if(ret<0) break;
		nbytes += ret;
	}
	return nbytes;
}


static int tinyos_fid_seek(void* cookie, off64_t* position, int whence)
{
	Fid_t fid = *(Fid_t*)cookie;
	intptr_t pos = Seek(fid, *position, whence);
	if(pos<0) return -1;
	*position = pos;
	return 0;
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
	tinyos_fid_seek,
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


void PError(const char* msg, ...)
{
	va_list ap;	
	va_start(ap, msg);
	vprintf(msg,ap);
	va_end(ap);

	char ebuf[164];
	printf(": %s\n", strerror_r(GetError(), ebuf, 164));
}



int GetTimestamp(timestamp_t* ts)
{
	Fid_t fclk = Open("/dev/clock", OPEN_RDONLY);
	if(fclk==NOFILE) return -1;

	int rc = Read(fclk, (void*)ts, sizeof(timestamp_t));
	if(rc!=sizeof(timestamp_t)) return -1;

	return 0;
}

void LocalTime(timestamp_t ts, struct tm* tm, unsigned long* usec)
{
	time_t tsec = ts/1000000;
	(void) localtime_r(&tsec, tm);
	if(usec) *usec = ts % 1000000;	
}


int GetTimeOfDay(struct tm* tm, unsigned long* usec)
{
	timestamp_t ts;
	if(GetTimestamp(&ts)) return -1;
	LocalTime(ts, tm, usec);
	return 0;
}



static int exec_wrapper(int argl, void* args)
{
	packer p = UNPACKER(argl, args);
	/* unpack the program pointer */
	char* program = strget(&p);
	size_t argc;  UNPACK(&p, argc);
	const char* argv[argc+1];
	unpackz(&p, argc, argv);

	return Exec(program, argv, NULL);
}


extern int bf_program_task(int argl, void* args);

int ParseProgArgs(Task task, int argl, void* args, Program* prog, int argc, const char** argv)
{
	if(task != bf_program_task)
		/* We do not recognize the format! */
		return -1;

	/* unpack the program pointer */
	assert(argl >= sizeof(prog));
	if(prog) memcpy(&prog, args, sizeof(prog));

	argl -= sizeof(prog);
	args += sizeof(prog);

	/* unpack the string vector */
	size_t N = argscount(argl, args);
	if(argv) {
		if(argc>N)
			argc = N;
		argvunpack(argc, argv, argl, args);
	}

	return N;		
}



int SpawnProgram(const char* path, size_t argc, const char* const * argv)
{
	packer p PACKER_CLEANUP = PACKER;
	strpack(&p, path);
	packv(&p, argc, argv);	

	/* Execute the process */
	return Spawn(exec_wrapper, p.pos, p.buffer);
}


void BarrierSync(barrier* bar, unsigned int n)
{
	assert(n>0);
	Mutex_Lock(& bar->mx);

	int epoch = bar->epoch;

	bar->count ++;
	if(bar->count >= n) {
		bar->epoch ++;
		bar->count = 0;
		Cond_Broadcast(&bar->cv);
	}

	while(epoch == bar->epoch)
		Cond_Wait(&bar->mx, &bar->cv);

	Mutex_Unlock(& bar->mx);
}


int ReadDir(int dirfd, char* buffer, unsigned int size)
{
	char nbuf[3];

	for(int i=0; i<2; ) {
		int rc = Read(dirfd, nbuf+i, 2-i);
		if(rc==0) return 0;
		if(rc==-1) return -1;
		i+=rc;
	}
	nbuf[2] = '\0';

	int len = strtol(nbuf, NULL, 16)+1;
	if(len>size) {
		Seek(dirfd, -2, SEEK_END);
		return -1;
	}

	for(int i=0; i<len; ) {
		int rc = Read(dirfd, buffer+i, len-i);
		if(rc==0) return 0;
		if(rc==-1) return -1;
		i+=rc;
	}
	return len;
}


int dll_load(const char* name)
{
	Fid_t fid = Open("/dev/.kernel_dl", OPEN_RDONLY);
	if(fid==NOFILE) return -1;
	if(Ioctl(fid, DLL_LOAD, (void*)name)) return -1;
	return 0;
}

int dll_unload(const char* name)
{
	char pathname[MAX_PATHNAME];
	snprintf(pathname, MAX_PATHNAME, "/dev/%s", name);

	Fid_t fid = Open(pathname, OPEN_RDONLY);
	if(fid==NOFILE) return -1;
	if(Ioctl(fid, DLL_UNLOAD, (void*)name)) return -1;
	return 0;
}

int install(size_t argc, const char** argv)
{
	if(argc<2) { printf("Insufficient arguments"); return 1; }
	if(dll_load(argv[1])==-1) { PError(argv[0]); return 1; }
	return 0;
}
REGISTER_PROGRAM(install)

int uninstall(size_t argc, const char** argv)
{
	if(argc<2) { printf("Insufficient arguments"); return 1; }
	if(dll_unload(argv[1])==-1) { PError(argv[0]); return 1; }
	return 0;
}
REGISTER_PROGRAM(uninstall)



TOS_REGISTRY
