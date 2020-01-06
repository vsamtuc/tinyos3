#ifndef __KERNEL_PIPE_H
#define __KERNEL_PIPE_H

#include <stddef.h>
#include "kernel_sched.h"

typedef struct ring_buffer {
	size_t capacity;
	size_t size;
	ptrdiff_t head;

	void*  data;
} ring_buffer;



static inline size_t ring_buffer_space(ring_buffer* this)
{
	return this->capacity - this->size;
}


size_t ring_buffer_put(ring_buffer* this, void* buf, size_t nelem);

size_t ring_buffer_get(ring_buffer* this, void* buf, size_t nsize);


#define PIPE_SIZE 8192

typedef struct pipe_control_block
{
	ring_buffer BUF;
	unsigned int refcount;
	wait_queue read_ready, write_ready;
	int end_of_stream;
} pipecb;


pipecb* create_pipe(size_t capacity);

void pipe_incref(pipecb* this);
void pipe_decref(pipecb* this);

int pipe_read(pipecb* this, char* buf, unsigned int size);
int pipe_write(pipecb* this, char* buf, unsigned int size);
int pipe_read_close(pipecb* this);
int pipe_write_close(pipecb* this);



#endif
