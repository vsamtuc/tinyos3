
#include "kernel_streams.h"
#include "tinyoslib.h"

/*
	Here, we implement two pseudo-streams
	that tie to stdin and stdout.

	They can be used to run without terminals.
*/

extern FILE *saved_in, *saved_out;

static int stdio_read(void* __this, char *buf, unsigned int size)
{
	size_t ret;

	while(1) {
		ret = fread_unlocked(buf, 1, size, saved_in);

		if(ferror_unlocked(saved_in)) {			
			//assert(errno==EINTR);
			if(errno!=EINTR) {
				char buf[101];
				fprintf(stderr, "error: %s\n",strerror_r(errno, buf, 100));				
			}
			clearerr_unlocked(saved_in);
		} else {
			break;
		}
	}
	return ret;
}


static int stdio_write(void* __this, const char* buf, unsigned int size)
{
	return fwrite_unlocked(buf, 1, size, saved_out);
}

static int stdio_close(void* this) { return 0; }

file_ops __stdio_ops = {
	.Read = stdio_read,
	.Write = stdio_write,
	.Close = stdio_close
};

void tinyos_pseudo_console()
{
	Fid_t fid[2];
	FCB* fcb[2];

	/* Since FCB_reserve allocates fids in increasing order,
	   we expect pair[0]==0 and pair[1]==1 */
	if(FCB_reserve(2, fid, fcb)==0 || fid[0]!=0 || fid[1]!=1)
	{
		printf("Failed to allocate console Fids\n");
		abort();
	}

	fcb[0]->streamobj = NULL;
	fcb[1]->streamobj = NULL;

	fcb[0]->streamfunc = &__stdio_ops;
	fcb[1]->streamfunc = &__stdio_ops;

}
