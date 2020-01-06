
#include "kernel_pipe.h"
#include "kernel_proc.h"
#include "kernel_cc.h"
#include "kernel_streams.h"
#include "tinyos.h"



/*
	ring buffer implementation
 */

#define min(a,b) ({__auto_type x = (a);  __auto_type y = (b); \
	x < y ? x : y; })

size_t ring_buffer_put(ring_buffer* this, void* buf, size_t nelem)
{
	size_t txlen = min(nelem, this->capacity - this->size);
	ptrdiff_t tail = (this->head + this->size) % this->capacity;

	/* First segment copy */
	size_t txseg1 = min(txlen, this->capacity - tail);
	memcpy( this->data + tail, buf,  txseg1);

	/* Second segment copy */
	size_t txseg2 = txlen - txseg1;
	if(txseg2) memcpy(this->data, buf+txseg1, txseg2);

	this->size += txlen;
	return txlen;
}

size_t ring_buffer_get(ring_buffer* this, void* buf, size_t nsize)
{
	size_t txlen = min(nsize, this->size);

	/* First segment copy */
	size_t txseg1 = min(txlen, this->capacity - this->head);
	memcpy(buf, this->data + this->head, txseg1);

	size_t txseg2 = txlen - txseg1;
	if(txseg2) memcpy(buf + txseg1, this->data, txseg2);

	this->size -= txlen;
	this->head += txlen;
	this->head %= this->capacity;
	return txlen;
}



/*
	Pipe implementation
 */

static wait_channel wchan_pipe_read = { SCHED_PIPE, "pipe_read" };
static wait_channel wchan_pipe_write = { SCHED_PIPE, "pipe_write" };


pipecb* create_pipe(size_t capacity)
{
	pipecb* pipe = xmalloc(sizeof(pipecb));
	void* data = xmalloc(capacity);

	pipe->BUF = (ring_buffer){.capacity=capacity, .size=0, .head=0, .data=data};
	pipe->refcount = 1;
	wqueue_init(& pipe->read_ready, &wchan_pipe_read);
	wqueue_init(& pipe->write_ready, &wchan_pipe_write);
	pipe->end_of_stream = 0;
	return pipe;
}

void pipe_incref(pipecb* this) { this->refcount ++; }

void pipe_decref(pipecb* this) 
{
	if( (--this->refcount) == 0 ) {
		free(this->BUF.data);
		free(this);
	}
}


int pipe_read(pipecb* this, char* buf, unsigned int size)
{
	if(size==0) return 0;
	pipe_incref(this);

	/* Condition */
	while(this->BUF.size==0 && !this->end_of_stream && this->BUF.data!=NULL)
		kernel_wait(& this->read_ready);

	/* Action */

	/* Check that reading is not closed */
	if(this->BUF.data == NULL) {
		set_errcode(EINVAL);
		pipe_decref(this);
		return -1;
	}
	/* Do the read */
	int tx = ring_buffer_get(&this->BUF, buf, size);
	assert((tx==0) <= this->end_of_stream);

	/* broadcast */
	kernel_broadcast(& this->write_ready);

	pipe_decref(this);
	//fprintf(stderr, "Read %d of %d bytes\n", tx, size);
	return tx;
}


int pipe_write(pipecb* this, char* buf, unsigned int size)
{
	if(size==0) return 0;
	pipe_incref(this);

	/* Condition */
	while(ring_buffer_space(&this->BUF)==0 && !this->end_of_stream && this->BUF.data!=NULL)
		kernel_wait(& this->write_ready);

	/* Action */
	if(this->end_of_stream) {  /* Somebody closed us for writing */
		set_errcode(EINVAL);
		pipe_decref(this);
		return -1;
	}
	if(this->BUF.data==NULL) {
		set_errcode(EPIPE);
		pipe_decref(this);
		return -1;
	}

	int tx = ring_buffer_put(&this->BUF, buf, size);

	/* broadcast */
	kernel_broadcast(& this->read_ready);

	pipe_decref(this);
	return tx;
}


int pipe_read_close(pipecb* this)
{
	/* Make sure we close the read end */
	if(this->BUF.data) {
		free(this->BUF.data);
		this->BUF.data = 0;
		this->BUF.capacity = 0;

		kernel_broadcast(&this->read_ready);
		kernel_broadcast(&this->write_ready);
	}

	pipe_decref(this);
	return 0;
}


int pipe_write_close(pipecb* this)
{
	/* Make sure we close the read end */
	if(this->end_of_stream==0) {
		this->end_of_stream = 1;

		kernel_broadcast(&this->read_ready);
		kernel_broadcast(&this->write_ready);
	}

	pipe_decref(this);
	return 0;
}


file_ops pipe_read_stream_ops = 
{
	.Read = (void*) pipe_read,
	.Release = (void*) pipe_read_close
};

file_ops pipe_write_stream_ops =
{
	.Write = (void*) pipe_write,
	.Release = (void*) pipe_write_close
};



int sys_Pipe(pipe_t* pipe)
{
	Fid_t fid[2];
	FCB* fcb[2];

	if(! FCB_reserve(2, fid, fcb)) 
		return -1;

	pipecb* obj = create_pipe(PIPE_SIZE);

	pipe_incref(obj);
	fcb[0]->streamobj = fcb[1]->streamobj = obj;

	fcb[0]->streamfunc = & pipe_read_stream_ops;
	fcb[1]->streamfunc = & pipe_write_stream_ops;

	pipe->read = fid[0];
	pipe->write = fid[1];
	return 0;
}

