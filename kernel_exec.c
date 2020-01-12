
#include <dlfcn.h>

#include "kernel_exec.h"
#include "kernel_proc.h"
#include "kernel_fs.h"
#include "kernel_dev.h"
#include "kernel_sys.h"
#include "tinyoslib.h"

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
	else if(rc!=2) {  assert(rc>=0); set_errcode(ENOEXEC); return -1; }
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

	int rc = sys_Read(f, (void*) &prog, sizeof(prog));
	sys_Close(f);
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

	The dynamic linker device. 

	This device manages the shared objects loaded
	into the kernel, to provide executable routines
	to the system.

 ==================================================== */


#define MAX_DLOBJ 64

/*
	A directory that contains all the exposed executable
	entities
 */
rdict bin_directory;


static int __tos_entity_eq(rlnode* node, rlnode_key key)
{
        struct tos_entity* ent = node->obj;
        return strncmp(ent->name, key.str, MAX_NAME_LENGTH)==0;
}

static void __tos_add(struct tos_entity* ent)
{
	hash_value hv = hash_nstring(ent->name,MAX_NAME_LENGTH);
	if(rdict_lookup(&bin_directory, hv, ent->name, __tos_entity_eq)) {
		/* The name already exists! Skip it for now */
		return;
	}
	rdict_node_init(&ent->dnode, ent, hv);
	rdict_insert(&bin_directory, &ent->dnode);
}

static void __tos_del(struct tos_entity* ent)
{
	rdict_remove(&bin_directory, &ent->dnode);
}


/*
	Dynamically loaded object control block

	Each control block is a device instance
 */
struct dlobj {
	uint16_t minor;
	unsigned int use_count;
	void* handle;
	char name[MAX_NAME_LENGTH];
};


/* 
	Helper that calls a function on each tos_element of the DL object
 */
void dlobj_apply(struct dlobj* dlobj, void (*func)(struct tos_entity*))
{
	struct tos_entity** begin = dlsym(dlobj->handle,"__begin_tinyos");
	struct tos_entity** end = dlsym(dlobj->handle,"__end_tinyos");
	for(struct tos_entity* p = *begin; p!= *end; p++) 
		func(p);
}


/* The table of DL objects */
unsigned int dl_table_no = 0;
struct dlobj dl_table[MAX_DLOBJ];


int dlobj_alloc(void* handle, const char* name, struct dlobj** dl)
{
	int pos = -1; /* Will hold empty entry */

	/* Scan all entries to see if handle is already open */
	for(int i=0; i<MAX_DLOBJ; i++) {

		if(dl_table[i].use_count==0) {
			/* Mark this location for insertion. */
			if(pos==-1) pos = i; 
			continue;
		}

		if(dl_table[i].handle == handle) {
			/* Oops! reopened */
			int rc = dlclose(handle);
			assert(rc==0);
			return EEXIST;
		}

	}

	if(pos==-1) {
		return ENOMEM; /* Out of memory (no available location) */
	}

	/* Initialize new location */
	dl_table[pos].use_count = 1;
	dl_table[pos].handle = handle;
	strncpy(dl_table[pos].name, name, MAX_NAME_LENGTH);

	dlobj_apply(&dl_table[pos], __tos_add);
	if(dl) *dl = &dl_table[pos];
	dl_table_no ++;

	return 0;
}


int dlobj_open(const char* filename, struct dlobj** dl)
{
	if(strlen(filename)==0) return EINVAL;
	if(strlen(filename)>MAX_NAME_LENGTH) return ENAMETOOLONG;

	char pathname[MAX_NAME_LENGTH+2];
	snprintf(pathname, MAX_NAME_LENGTH+2, "./%s", filename);

	dlerror();
	void* handle = dlopen(pathname, RTLD_NOW | RTLD_LOCAL);
	if(handle==NULL) {
		fprintf(stderr, "dlobj_open: %s\n", dlerror());
		return ENOENT;
	}
	return dlobj_alloc(handle, filename, dl);
}


int dlobj_close(struct dlobj* dl)
{
	assert(dl->use_count==0);
	dlobj_apply(dl, __tos_del);
	dl_table_no --;
	if(dl->handle == RTLD_DEFAULT) {
		dl->handle = NULL; /* This is superfluous... */
		return 0;
	}

	dlerror(); /* clear */
	if(dlclose(dl->handle)) {
		fprintf(stderr, "dlobj_close: %s\n", dlerror());
		return EIO;  /* What? */
	} else {
		dl->handle = NULL;
		return 0;
	}
}


int dlobj_decref(struct dlobj* dl) 
{
	assert(dl->use_count);
	dl->use_count --;
	if(dl->use_count==0)
		return dlobj_close(dl);
	else
		return 0;
}

void dlobj_incref(struct dlobj* dl) 
{
	dl->use_count ++;
}


void dlobj_initialize()
{
	rdict_init(&bin_directory, 0);
	memset(dl_table, 0, MAX_DLOBJ*sizeof(struct dlobj));

	if(dlobj_alloc(RTLD_DEFAULT, ".kernel", NULL)!=0) abort();

	assert(dlobj_open("testprog.so", NULL)==0);
	assert(dlobj_open("testprog2.so", NULL)==0);
}


void dlobj_finalize()
{
	assert(dlobj_decref(dl_table+1)==0);
	assert(dlobj_decref(dl_table+2)==0);
	assert(dlobj_decref(dl_table)==0);

	rdict_destroy(&bin_directory);
}



static DCB dll_dcb = {
	.type = DEV_DLL,
	.Init = dlobj_initialize,
	.Fini = dlobj_finalize,
	.Open = NULL,
	.devnum = MAX_DLOBJ,
	.dev_fops = {

	}
};


REGISTER_DRIVER(dll_dcb);


/****************************************************

  The binary file system. This is a minimal file system 
  that exposes binary executable routines. It is typically 
  mounted under /bin.

  It is possible to load and unload dynamically a Linux 
  shared object containing executable routines. This is done
  by creating a **directory** with the same name as a host 
  (i.e., Linux) file in the current (Linux) directory. 

  Removing this directory (if possible) unloads the shared object.

 ****************************************************/

extern FSystem BINFS;


static inline int __is_dir(inode_t ino) { return ino <= MAX_DLOBJ; }
static inline struct dlobj* __to_dl(inode_t ino) { return dl_table+ino; }
static inline struct tos_entity* __to_ent(inode_t ino) { return (void*) ino; }


/* Mount a file system of this type. */
static int binfs_mount(MOUNT* mnt, Dev_t dev, unsigned int param_no, mount_param* param_vec)
{
    if(dev!=NO_DEVICE && dev!=0) return ENXIO;
    mnt->ptr = NULL;
    return 0;
}

/* Unmount a particular mount always succeeds */
static int binfs_umount(MOUNT mnt) { return 0; }


static int binfs_statfs(MOUNT mnt, struct StatFs* statfs)
{ 
	statfs->fs_dev = 0;
	statfs->fs_root = 0;

	statfs->fs_fsys = BINFS.name;
	statfs->fs_blocks = 0;
	statfs->fs_bused = 0;
	statfs->fs_files = bin_directory.size + 1;
	statfs->fs_fused = statfs->fs_files;

	return 0;
}


/* This is a read-only file system */
static int binfs_create(MOUNT _mnt, inode_t _dir, const pathcomp_t name, Fse_type type, inode_t* newino, void* data)
{ return EROFS; }


static int binfs_link(MOUNT _mnt, inode_t _dir, const pathcomp_t name, inode_t id)
{  return EROFS; }
static int binfs_unlink(MOUNT _mnt, inode_t _dir, const pathcomp_t name)
{ return EROFS; }


/* These always succeed */
static int binfs_pin(MOUNT _mnt, inode_t _ino) { return 0; }
static int binfs_unpin(MOUNT _mnt, inode_t _ino) { return 0; }
static int binfs_flush(MOUNT _mnt, inode_t _ino) { return 0; }

static int binfs_listdir(MOUNT _mnt, inode_t _ino, struct dir_list* dlist)
{
	if(_ino != 0) return ENOTDIR;

	void dir_list_add_tos(rlnode* n) {
		struct tos_entity* e = n->obj;
		if(e->type == TOS_PROGRAM)
			dir_list_add(dlist, e->name);
	}

	rdict_apply(& bin_directory, dir_list_add_tos);
	return 0;
}



struct binfs_stream {
	struct tos_entity* ent;
	intptr_t pos;
};


int binfs_read(void* _s, char* buf, unsigned int size)
{
	struct binfs_stream* s = _s;
    if(size==0) return 0;
    if(s->pos >= s->ent->size) return 0;

    size_t remaining_bytes = s->ent->size - s->pos;
    size_t txbytes = (remaining_bytes < size) ? remaining_bytes : size ;

    memcpy(buf, s->ent->data+s->pos, txbytes);
    s->pos += txbytes;
    return txbytes;
}

intptr_t binfs_seek(void* _s, intptr_t offset, int whence)
{
	struct binfs_stream* s = _s;
	intptr_t newpos;
	switch(whence) {
	        case SEEK_SET:
	                newpos = 0; break;
	        case SEEK_CUR: 
	                newpos = s->pos; break;
	        case SEEK_END:
	                newpos = s->ent->size; break;
	        default:
	                set_errcode(EINVAL);
	                return -1;
	}

	newpos += offset;
	if(newpos <0 || newpos>= s->ent->size) {
	                set_errcode(EINVAL);
	                return -1;              
	}
	s->pos = newpos;
	return newpos;
}

int binfs_close(void* _s)
{
	free(_s);
	return 0;		
}


static file_ops binfs_ops = {
	.Read = binfs_read,
	.Seek = binfs_seek,
	.Release = binfs_close
};


static int binfs_open(MOUNT _mnt, inode_t _ino, int flags, void** obj, file_ops** ops)
{
	if(_ino == 0) return EISDIR;
	struct tos_entity* e = (void*) _ino;
	struct binfs_stream* s;

	switch(e->type) {
	case TOS_PROGRAM:
		s = xmalloc(sizeof(struct binfs_stream));
		s->ent = e;
		s->pos = 0;
		*obj = s;
		*ops = &binfs_ops;
		return 0;
	default:
		return EINVAL;
	}
}

static int binfs_truncate(MOUNT _mnt, inode_t _ino, intptr_t length)
{
	if(_ino) return EROFS;
	else return EISDIR;
}

static int binfs_fetch(MOUNT _mnt, inode_t _dir, const pathcomp_t name, inode_t* id, int creat)
{
	if(_dir) return ENOTDIR;

	/* First treat the standard directory entries */
	if(strcmp(name,".")==0 || strcmp(name,"..")==0) { *id = _dir; return 0; }

	/* Look up into dev_directory */
	rlnode* n = rdict_lookup(& bin_directory, hash_nstring(name,MAX_NAME_LENGTH), name, __tos_entity_eq);
	if(n==NULL) return creat ? EROFS : ENOENT;
	/* Found device */
	*id = (inode_t) n->obj;
	
	return 0;
}

extern timestamp_t boot_timestamp;

static int binfs_status(MOUNT _mnt, inode_t _ino, struct Stat* st, pathcomp_t name)
{
	if(_ino == 0) {
		if(name != NULL) name[0]='\0';
		if(st == NULL) return 0;

		st->st_type = FSE_DIR;
		st->st_nlink = 2;
		st->st_rdev = NO_DEVICE;
		st->st_size = bin_directory.size;
	} else {
		if(name != NULL) return ENOTDIR;
		if(st == NULL) return 0;

		struct tos_entity* ent = (void*) _ino;

		st->st_type = FSE_FILE;
		st->st_nlink = 1;
		st->st_rdev = NO_DEVICE;
		st->st_size = ent->size;
	}

	st->st_dev = 0;
	st->st_ino = _ino;
	st->st_blksize = 1024;
	st->st_blocks = 0;
	st->st_change = st->st_modify = st->st_access = boot_timestamp;
	return 0;
}


FSystem BINFS = {
	.name = "binfs",
	.Mount = binfs_mount,
	.Unmount = binfs_umount,
	.Create = binfs_create,
	.Pin = binfs_pin,
	.Unpin = binfs_unpin,
	.Flush = binfs_flush,
	.Open = binfs_open,
	.ListDir = binfs_listdir,
	.Truncate = binfs_truncate,
	.Fetch = binfs_fetch,
	.Link = binfs_link,
	.Unlink = binfs_unlink,
	.Status = binfs_status,
	.StatFs = binfs_statfs
};

REGISTER_FSYS(BINFS);





/*===================================================

	Init

 ==================================================== */


int hello(size_t argc, const char** argv)
{
	printf("Hello world\n");
	return 0;
}

REGISTER_PROGRAM(hello, "A simple test")
TOS_REGISTRY

void initialize_binfmt()
{
#if 0
	rdict_init(&bin_directory, 0);
	memset(dl_table, 0, MAX_DLOBJ*sizeof(struct dlobj));

	if(dlobj_alloc(RTLD_DEFAULT, ".kernel", NULL)!=0) abort();

	assert(dlobj_open("testprog.so", NULL)==0);
	assert(dlobj_open("testprog2.so", NULL)==0);
#endif
}


void finalize_binfmt()
{
#if 0
	assert(dlobj_decref(dl_table+1)==0);
	assert(dlobj_decref(dl_table+2)==0);
	assert(dlobj_decref(dl_table)==0);

	rdict_destroy(&bin_directory);
#endif
}

