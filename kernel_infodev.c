
#include "kernel_dev.h"
#include "kernel_proc.h"
#include "kernel_fs.h"

/*============================================

  The info device driver

  This is a category of virtual devices whose
  purpose is to return information to the user
  from kernel data.

 ============================================*/


typedef struct info_stream_control_block
{
	char* data;
	size_t size;
	intptr_t pos;
} info_stream;


typedef struct info_device_control_block
{
	/* The name of the device */
	const char* pubname;

	/* The function that creates the info data */
	void (*write_output)(FILE*);
} info_dcb_t;


/*
	Print one "line" for each ACTIVE or ZOMBIE PCB in the process table.
 */
void write_procinfo(FILE* f)
{
	for(Pid_t pid=1; pid<MAX_PROC; pid++) {
		PCB* pcb = get_pcb(pid);
		if(pcb==NULL) continue;

		char status;
		const char* wchan = "-";

		if(pcb->pstate==ALIVE) {
			thread_info mtinfo;
			get_thread_info(pcb->main_thread, &mtinfo);
			if(mtinfo.state==STOPPED) {
				status = 'S';
				wchan = mtinfo.wchan->name;
			} else
				status = 'R';
		} else 
			status = 'Z'; 

		fprintf(f, "%u:%u:%c:%s:%p:%u:",
			pid,
			get_pid(pcb->parent),
			status,
			wchan,
			pcb->main_task,
			pcb->argl
			);
		fwrite(pcb->args, 1, pcb->argl, f);
		fprintf(f,"\n");
	}
}

/*
	Return a sequence of strings where each line is a path with a mounted file system.
 */
void write_mnttab(FILE* f)
{
	char mpath[MAX_PATHNAME];
	for(uint i=1; i< MOUNT_MAX; i++) {
		FsMount* mnt = & mount_table[i];
		if(mnt->fsys == NULL) continue;
		if(get_pathname(mnt->mount_point, mpath, MAX_PATHNAME)==0) {
			fprintf(f, "%s %u %s\n", mpath, mnt->device, mnt->fsys->name);
		}
	}
}

/* Return a sequence of strings where each line is a file system type */
void write_filesystems(FILE* f)
{
	for(uint i=0;i<FSYS_MAX;i++) {
		FSystem* fsys = file_system_table[i];
		if(fsys)
			fprintf(f, "%s\n", fsys->name);
	}
}



#define MAX_INFO 3

static info_dcb_t info_dev[MAX_INFO] = 
{
	{
		.pubname = "procinfo",
		.write_output = write_procinfo
	},
	{
		.pubname = "mnttab",
		.write_output = write_mnttab
	},
	{
		.pubname = "filesystems",
		.write_output = write_filesystems
	}
};


/*
	The device methods
 */

static void info_devinit();

static void* info_open(uint minor)
{
	assert(minor < MAX_INFO);
	info_stream* s = xmalloc(sizeof(info_stream));
	s->data = NULL;
	s->size = 0;
	s->pos = 0;

	FILE* mf = open_memstream(&s->data, &s->size);
	info_dev[minor].write_output(mf);
	fclose(mf);
	return s;
}


static int info_read(void* dev, char *buf, unsigned int size)
{
	info_stream* s = dev;
	if(size==0) return 0;
	if(s->pos >= s->size) return 0;

	size_t remaining_bytes = s->size - s->pos;
	size_t txbytes = (remaining_bytes < size) ? remaining_bytes : size ;

	memcpy(buf, s->data+s->pos, txbytes);
	s->pos += txbytes;

	return txbytes;
}

static intptr_t info_seek(void* dev, intptr_t offset, int whence)
{
	info_stream* s = dev;
	intptr_t newpos;
	switch(whence) {
	        case SEEK_SET:
	                newpos = 0; break;
	        case SEEK_CUR: 
	                newpos = s->pos; break;
	        case SEEK_END:
	                newpos = s->size; break;
	        default:
	                set_errcode(EINVAL);
	                return -1;
	}

	newpos += offset;
	if(newpos <0 || newpos>= s->size) {
	                set_errcode(EINVAL);
	                return -1;              
	}
	s->pos = newpos;
	return newpos;	
}

static int info_close(void* dev) 
{
	info_stream* s = dev;
	free(s->data);
	free(s);
	return 0;
}

static DCB info_dcb = 
{
	.type = DEV_INFO,
	.Init = info_devinit,
	.Open = info_open,
	.devnum = MAX_INFO,
	.dev_fops = {
		.Read = info_read,
		.Seek = info_seek,
		.Release = info_close
	}	
};



static void info_devinit()
{
	for(uint i=0; i<MAX_INFO; i++)
		device_publish(info_dev[i].pubname, DEV_INFO, i);
}


REGISTER_DRIVER(info_dcb)
