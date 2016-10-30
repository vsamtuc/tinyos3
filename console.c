
#include "kernel_streams.h"
#include "tinyoslib.h"

/*
	Here, we implement two pseudo-streams
	that tie to stdin and stdout.

	They can be used to run without terminals.
*/

extern FILE *saved_in, *saved_out;

static int stdio_read(void* this, char *buf, unsigned int size)
{
	//assert(this == saved_in);
	size_t ret;

	while(1) {
		ret = fread_unlocked(buf, 1, size, (FILE*)this);

		if(ferror(this)) {
			assert(errno==EINTR);
			clearerr(this);
		} else {
			break;
		}
	}
	return ret;
}


static int stdio_write(void* this, const char* buf, unsigned int size)
{
	assert(this == saved_out);
	return fwrite_unlocked(buf, 1, size, (FILE*)this);
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

	fcb[0]->streamobj = fdopen(0, "w+");
	fcb[1]->streamobj = saved_out;

	fcb[0]->streamfunc = &__stdio_ops;
	fcb[1]->streamfunc = &__stdio_ops;

}
