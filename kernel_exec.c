#include <dlfcn.h>
#include <link.h>
#include <elf.h>

#include "kernel_exec.h"
#include "kernel_proc.h"
#include "kernel_fs.h"
#include "kernel_dev.h"
#include "kernel_sys.h"
#include "tinyoslib.h"

/*===================================================

	The exec_program() function and its helpers

 ==================================================== */

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
	const char* argv[], const char* envp[], int iter)
{
	/* If we are too deep in translation, return error */
	if(iter >= BINFMT_MAX_DEPTH) { set_errcode(ELOOP); return -1; }

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

	/* Fix argv and envp NULL case. Here we follow the linux tradition... */
	const char* __empty[1] = { NULL };
	if(! argv) argv = __empty;
	if(! envp) envp = __empty;

	/* Get the format recipe for executor. Dup is needed to ensure */
	int txtfid = sys_Dup(fid);
	return xfmt->exec_func(xargs, txtfid, pathname, argv, envp, iter+1);
}


/*===================================================
	The "Program/argc/argv" format
 ==================================================== */


/*
	Wrapper task: This task unwraps (argl, args) into [prog, argc, argv]
	and executes the latter
 */
int bf_program_task(int argl, void* args)
{
	Program p;
	assert(argl>=sizeof(p));
	memcpy(&p, args, sizeof(p));
	size_t argc = argscount(argl-sizeof(p), args+sizeof(p));
	const char* argv[argc+1];
	argvunpack(argc, argv, argl-sizeof(p), args+sizeof(p));
	argv[argc] = NULL;
	return p(argc, argv);
}


/*
	Packer: 
	1.  collects a pointer 'prog' from f
	2.  packs  [prog, argc, argv]  into (argl, args)
	3.  sets up xargs
 */
int bf_program_exec(struct exec_args* xargs, Fid_t f, const char* pathname, const char* argv[], const char* envp[], int iter)
{
	struct bf_program prog;

	/* Read the program pointer */
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

	/* Get lease on DSO of program */
	struct dlobj* lease = dlobj_addr(prog.program);
	if(lease) dlobj_incref(lease);

	/* Set up xargs */
	xargs->task = bf_program_task;
	xargs->argl = argl+psize;
	xargs->args = args;
	xargs->lease = lease;

	return 0;
}


struct binfmt bf_program = {
	.magic = {'#','P'},
	.exec_func = bf_program_exec
};



/*===================================================
	The 'shebang' format

	The first line up to the newline or end of file must be
	(a) up to 255 chars
	(b) start with '#!'
	(c) look like
			#!  <progpath> [arg1] ... [arg30]
		where <progpath> must be a white-space-free path name
		[arg1] to [arg9] are optional.
		These must be separated by spaces/tabs 
 ==================================================== */

#define SHEBANG_MAX_ARGNO 32  /* max. args + 2 */

int bf_shebang_exec(struct exec_args* xargs, Fid_t f, const char* pathname, const char* argv[], const char* envp[], int iter)
{
	/* If we are too deep in translation, return error now */
	if(iter+1 >= BINFMT_MAX_DEPTH) { set_errcode(ELOOP); return -1; }

	/* Read up to 256 bytes from the file */
	char first_line[255+1];

	int rc = sys_Read(f, first_line, 255);
	sys_Close(f);  if(rc==-1) return -1;
	assert(rc>=0 && rc<256);

	/* 
		Parse first line.

		We must not assume anything, because the file may have changed since we read the shebang 
	*/
	first_line[rc] = '\0';

	/* Mark the newline with a \0, if found, and skip #! */
	char* p;
	for(p=first_line; *p; p++) 
		if(*p=='\n') break;
	if(p==first_line+rc) { set_errcode(ENOEXEC); return -1; } /* Line too long! */
	*p = '\0';
	if( (p-first_line) < 2 ) { set_errcode(ENOEXEC); return -1; } /* Line too short! */
	if( first_line[0]!='#' || first_line[1]!='!') { set_errcode(ENOEXEC); return -1; } /* Shebang missing! */

	/* 
		Tokenize cmd to get the arguments int (pargc, pargv) 
	*/
	char* cmd = first_line+2;
	char* pargv[SHEBANG_MAX_ARGNO];  
	memset(pargv, 0, SHEBANG_MAX_ARGNO*sizeof(char*));  /* Clear array */
	size_t pargc=0;
	for(uint i=0; i<SHEBANG_MAX_ARGNO; i++) {
		pargv[i] = strtok_r((i==0)?cmd:NULL, " \t", &p);
		if(pargv[i]) pargc++; else break;
	}

	if(pargc==0) { set_errcode(ENOEXEC); return -1; } /* Too few args! */
	if(pargv[SHEBANG_MAX_ARGNO-1]) { set_errcode(ENOEXEC); return -1; } /* Too many args! */

	/* 
		Construct the new argument list 
	*/
	size_t argc=0; 
	/* Count entries in argv. argv==NULL means 0 entries */
	for(const char*const* p=argv; *p; p++) argc++;
	/* Increase this since we will substitute the possibly non-existent argv[0] by the pathname */
	if(argc==0) argc=1;

	/* Make new "nargv" =  [pargv 0...pargc] pathname [argv 1...argc] */
	const char* nargv[pargc+argc+1];
	uint nargc = 0;
	for(uint i=0; i<pargc; i++) 
		nargv[nargc++] = pargv[i];
	nargv[nargc++] = pathname;
	for(uint i=1; i<argc; i++)
		nargv[nargc++] = argv[i];
	nargv[nargc] = NULL;

	/* 
		Recurse
	*/
	return exec_program(xargs, pargv[0], nargv, envp, iter);
}


struct binfmt bf_shebang = {
	.magic = {'#', '!'},
	.exec_func = bf_shebang_exec
};


/*===================================================
	binfmt table
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
	&bf_shebang,
	NULL
};





/*===================================================

	The dynamic linker device. 

	This device manages the shared objects loaded
	into the kernel, to provide executable routines
	to the system.

	A shared object (dlobj) contains routines marked with
	REGISTER_PROGRAM.

	To load a dlobj, use dlobj_load(...)
	To unload a dlobj, use dlobj_unload(...)

	Problem: If any thread is executing code in a dlobj
	that is unloaded, the thread will crash! Therefore,
	we need to implement protection to disallow 
	unloading while a dlobj is in use.

	Executable file: an executable file is just a file
	looking like [magic number, pointers to routines].

	Loading a dlobj: When a dlobj is loaded, its Programs
	must be identified and made available. To do this,
	each Program is pointed at by an object of type
	`struct tos_entity`

 ==================================================== */


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

static void __tos_add(struct dlobj* dl, struct tos_entity* ent)
{
	hash_value hv = hash_nstring(ent->name,MAX_NAME_LENGTH);
	if(rdict_lookup(&bin_directory, hv, ent->name, __tos_entity_eq)) {
		/* The name already exists! Skip it for now */
		return;
	}
	rdict_node_init(&ent->dnode, ent, hv);
	rdict_insert(&bin_directory, &ent->dnode);
	dl->pub_count ++;
}

static void __tos_del(struct dlobj* dl, struct tos_entity* ent)
{
	rdict_remove(&bin_directory, &ent->dnode);
	dl->pub_count --;
}



/* 
	Finds the dlobj containing this address
 */
struct dlobj* dlobj_addr(void* addr)
{
	Dl_info dli;
	struct link_map* lmap = NULL;

	if( (! dladdr1(addr, &dli, (void**) &lmap, RTLD_DL_LINKMAP))
		|| lmap==NULL) 
	{
		fprintf(stderr, "%s: dladdr() ould not resolve %p\n", __func__, addr);
		return NULL;
	}

	for(uint i=0; i<MAX_DLOBJ; i++) {
		if(dl_table[i].used && lmap == dl_table[i].lmap) return dl_table+i;
	}
	return NULL;
}


/* 
	Resolves a symbol in a dlobj
 */
void* dlobj_sym(struct dlobj* dl, const char* symbol)
{
	if(! dl->used) return NULL;
	dlerror();
	char* dlerr;
	void* resolved = dlsym(dl->handle, symbol);
	if( (dlerr = dlerror()) != NULL ) {
		fprintf(stderr, "dlobj_sym(%p, %s): %s", dl, symbol, dlerr);
	}
	return resolved;
}


/* 
	Calls a function on each tos_element of the DL object
 */
void dlobj_apply(struct dlobj* dlobj, void (*func)(struct dlobj*, struct tos_entity*))
{
	/* Take the 'tinyos' section of the shared object */
	struct tos_entity* begin = dlobj_sym(dlobj,"__start_tinyos");
	struct tos_entity* end = dlobj_sym(dlobj,"__stop_tinyos");
	assert(begin && end);

	/* Iterate over tos entities */
	for(struct tos_entity* p = begin; p < end; p++) 	func(dlobj, p);
}


/* Active entries in the dlobj table */
unsigned int dl_table_no = 0;

/* The dlobj table */
struct dlobj dl_table[MAX_DLOBJ];



int dlobj_load(const char* filename, struct dlobj** dlp)
{
	void* handle = NULL;

	/* Get the handle */
	if(filename==NULL) {
		handle = RTLD_DEFAULT;
		filename = ".kernel_dl";
	} else {
		/* Use dlopen to load a shared file */
		if(strlen(filename)==0) return EINVAL;
		if(strlen(filename)>MAX_NAME_LENGTH) return ENAMETOOLONG;

		char pathname[MAX_NAME_LENGTH+2];
		snprintf(pathname, MAX_NAME_LENGTH+2, "./%s", filename);

		dlerror();
		handle = dlopen(pathname, RTLD_NOW | RTLD_LOCAL);
		if(handle==NULL) {
			fprintf(stderr, "%s: %s\n", __func__, dlerror());
			return ENOENT;
		}
	}

	/* Proceed with dlobj allocation for the handle */
	int retval = 0;
	struct dlobj* dl = NULL;  /* Will hold allocated dlobj */
	int pos = -1; /* Will hold empty entry */

	/* Scan all entries to see if handle is already open */
	for(int i=0; i<MAX_DLOBJ; i++) {

		if(! dl_table[i].used) {
			/* Mark this location for insertion. */
			if(pos==-1) pos = i; 
			continue;
		}

		if(dl_table[i].handle == handle) {
			/* 	Oops! reopened. */
			if(handle!=RTLD_DEFAULT) {
				/* 
					Close handle with error checking. We do this because 
					dlopen() is reference-counting itself, and we have re-opened a DSO
				*/
				dlerror();
				if(dlclose(handle)!=0) {
					fprintf(stderr,"dlclose: %s\n", dlerror());
					return EIO;
				}
			}
			retval = EEXIST;
			assert(strncmp(filename, dl_table[i].name, MAX_NAME_LENGTH)==0);
			dl = dl_table+i;
			break;
		}
	}

	/* Check case where table is overflown */
	if(dl==NULL && pos==-1) {  assert(dl_table_no== MAX_DLOBJ);  return ENOMEM; }
	assert(dl==NULL);
	/* Initialize new location */
	if(dl==NULL) {
		dl_table_no ++;
		dl_table[pos].used = 1;
		dl_table[pos].published = 0;
		dl_table[pos].pub_count = 0;
		strncpy(dl_table[pos].name, filename, MAX_NAME_LENGTH);

		dl_table[pos].handle = handle;
		if(dlinfo(handle, RTLD_DL_LINKMAP, &dl_table[pos].lmap)==-1) {
			fprintf(stderr, "dlinfo(%p): %s\n", handle, dlerror());
			abort();
		}

		dl = &dl_table[pos];
		dl_table_no ++;
		assert(dl->handle==handle);
	}
	assert(dl!=NULL);


	/* Publish */
	if(! dl->published) {
		/* Add definitions to bin_directory */
		dlobj_apply(dl, __tos_add);

		/* publish this device */
		device_publish(dl->name, DEV_DLL, pos);
		dl->published = 1;
	}

	/* return the object */
	if(dlp) *dlp = dl;

	return retval;  /* This is either 0 or EEXIST */
}


int dlobj_unload(struct dlobj* dl)
{
	/* Unpublish */
	if(dl->published) {
		/* Remove definitions from bin_directory */
		dlobj_apply(dl, __tos_del);
		device_retract(dl->name);
		dl->published = 0;
	}

	if(dl->use_count != 0) return EBUSY;

	/* Unpublish */
	dl_table_no --;
	dl->used = 0;

	int retval = 0;

	if(dl->handle != RTLD_DEFAULT) {
		dlerror(); /* clear */
		if(dlclose(dl->handle)) {
			fprintf(stderr, "%s: %s\n", __func__, dlerror());
			retval = EIO;  /* What? */
		}
	}

	dl->handle = NULL;
	return retval;
}


int dlobj_decref(struct dlobj* dl) 
{
	if(! dl) return ENOENT;
	if(dl->used == 0) return ENODEV;

	assert(dl->use_count > 0);
	dl->use_count --;
	if(dl->use_count==0 && ! dl->published)
		return dlobj_unload(dl);
	else
		return 0;
}

void dlobj_incref(struct dlobj* dl) 
{
	assert(dl->used);
	dl->use_count ++;
}

struct dlobj* dlobj_byname(const char* name)
{
	for(int i=0; i < MAX_DLOBJ; i++)
		if(strncmp(name, dl_table[i].name, MAX_NAME_LENGTH)==0)
			return dl_table+i;
	return NULL;
}

void dlobj_initialize()
{
	rdict_init(&bin_directory, 0);
	memset(dl_table, 0, MAX_DLOBJ*sizeof(struct dlobj));

	if(dlobj_load(NULL, NULL)!=0) abort();
}

void dlobj_finalize()
{
	for(uint i=0; i<MAX_DLOBJ; i++) {
		struct dlobj* dl = dl_table+i;
		if(! dl->used) continue;
		assert(dl->use_count == 0);
		dlobj_unload(dl);
	}
	assert(bin_directory.size == 0);
	rdict_destroy(&bin_directory);
}

void* dlobj_open(devnum_t minor) 
{
	assert(minor<MAX_DLOBJ);
	struct dlobj* dl = dl_table+minor;
	dlobj_incref(dl);
	return dl;
}

int dlobj_close(void* _dl)
{
	struct dlobj* dl = _dl;
	return dlobj_decref(dl);
}

int dlobj_ioctl(void* _dl, unsigned long request, void* _name)
{
	struct dlobj* dl = _dl;
	const char* name = _name;

	switch(request) {
		case DLL_LOAD:
			return dlobj_load(name, NULL);
		case DLL_UNLOAD:
			return dlobj_unload(dl);
		default:
			return ENOTTY;
	}
}

static struct ioctl_func __dll_ioctls[] = {
	{ dlobj_ioctl, DLL_LOAD },
	{ dlobj_ioctl, DLL_UNLOAD },
	{ NULL, 0 }
};


static DCB dll_dcb = {
	.type = DEV_DLL,
	.Init = dlobj_initialize,
	.Fini = dlobj_finalize,
	.Open = dlobj_open,
	.devnum = MAX_DLOBJ,
	.dev_fops = {
		.Release = dlobj_close,
		.ioctls = __dll_ioctls
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


void initialize_binfmt()
{

}


void finalize_binfmt()
{
}

