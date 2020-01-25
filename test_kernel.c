#undef NDEBUG
#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include <sys/mman.h>

#include <dlfcn.h>

#include "util.h"
#include "bios.h"

#include "unit_testing.h"
#include "kernel_fs.h"
#include "tinyoslib.h"


BOOT_TEST(test_memfs_is_root, "Test that the devfs filesystem is mounted")
{
	struct StatFs st;

	ASSERT(StatFs("/dev", &st)==0);

	return 0;
}

BOOT_TEST(test_pathcomp, "Test operations on pathcomp_t")
{
	pathcomp_t pcomp;

	strcpy(pcomp, "hello");

	void change(pathcomp_t p) {
		p[0] = 'H';
	}
	change(pcomp);

	ASSERT(pcomp[0]=='H');
	return 0;
}



/* A symbol is a reference in a module. */
struct dl_symbol
{
	const char* name;
	void* addr;
	int providers;	/* to detect collisions */
	void** unresolved; /* pointer to chain of bindings */
	rlnode symtab_node;
};

typedef struct dlinker
{
	rdict symtab;	/* symbol table */
	rdict modules;  /* set of modules */

	struct dl_module* load_list; /* list of still unloaded modules */
	fcontext_t __retcontext; /* Temporary used during loading */
} dlinker;


/* 
	A function preparing the data segment. 
	It exports symbols to the dynamic linker and
	requests bindings from other modules.
*/
typedef void (*dl_module_func)(dlinker*);

struct dl_module
{
	dl_module_func func;
	void* saved_sp;
	void* alloc_sp;
	size_t stack_size;

	/* used by the dloader */
	rlnode modules_node;
	struct dl_module* load_next;
};


void dl_module_init(struct dl_module* dlmod, dl_module_func func)
{
	dlmod->func = func;
	dlmod->saved_sp = NULL;
	dlmod->alloc_sp = NULL;
	dlmod->stack_size = 0;

	rdict_node_init(& dlmod->modules_node, dlmod, hash_buffer(&func, sizeof(func)));
	dlmod->load_next = NULL;
}

static inline int symtab_equalf(rlnode* node, rlnode_key key)
{
	const char* modname = ((struct dl_symbol*) node->obj)->name;
	const char* keyname = key.str;
	return strcmp(modname, keyname)==0;	
}
static inline int modules_equalf(rlnode* node, rlnode_key key)
{
	dl_module_func modfunc = ((struct dl_module*) node->obj)->func;
	dl_module_func keyfunc = key.obj;
	return modfunc==keyfunc;
}


void dl_init(dlinker* dl)
{
	rdict_init(&dl->symtab, 0);
	rdict_init(&dl->modules,0);
	dl->load_list = NULL;
}


static void dl_free_obj(rlnode* node) { free(node->obj); }

void dl_destroy(dlinker* dl)
{
	void dict_free(rdict* dict) {
		rdict_apply_removed(dict, dl_free_obj);
		rdict_destroy(dict);
	}
	dict_free(&dl->symtab);
	dict_free(&dl->modules);
}


/** a module asks for another module */
static inline struct dl_module*  dl_mod_lookup(rdict* dict, dl_module_func func)
{
	hash_value hash = hash_buffer(&func, sizeof(func));
	rlnode* node = rdict_lookup(dict, hash, func, modules_equalf);
	if(node) return node->obj; 
	return NULL;
}


struct dl_module* dl_require(dlinker* dl, dl_module_func func)
{
	struct dl_module* dlmod = dl_mod_lookup(& dl->modules, func);
	if(! dlmod)
	{
		/* Allocate new module */
		dlmod = malloc(sizeof(struct dl_module));
		dl_module_init(dlmod, func);

		/* add to modules */
		rdict_insert(&dl->modules, &dlmod->modules_node);
		
		/* push it to load_list so that it will be loaded */
		dlmod->load_next = dl->load_list;
		dl->load_list = dlmod;
	}
	return dlmod;
}

/* Return a symbol, initializing one if necessary */
struct dl_symbol* dl_get_symbol(dlinker* dl, const char* name)
{
	hash_value hash = hash_string(name);
	struct dl_symbol* sym = NULL;
	rlnode* node = rdict_lookup(&dl->symtab, hash, name, symtab_equalf);
	if(node) 
		sym = node->obj;
	else {
		/* Initialize a symbol */
		sym = malloc(sizeof(struct dl_symbol));
		sym->name = name;
		sym->addr = NULL;
		sym->providers = 0;
		sym->unresolved = NULL;

		/* Add to symtab */
		rdict_node_init(&sym->symtab_node, sym, hash);
		rdict_insert(&dl->symtab, &sym->symtab_node);
	}
	return sym;
} 


/** a symbol is provided by a module */
void dl_export_symbol(dlinker* dl, const char* name, void* addr)
{
	struct dl_symbol* sym = dl_get_symbol(dl, name);

	/* Check for collision */
	sym->providers ++;
	if(sym->addr) {
		/* We have a collision */
		fprintf(stderr, "Collision detected for symbol %s\n",name);
	} else 
		sym->addr = addr;

	/* Resolve any unresolved bindings */
	while(sym->unresolved) {
		void** binding = sym->unresolved;
		sym->unresolved = *binding;
		*binding = sym->addr;
	}
}

/** a symbol is needed by a module */
void dl_bind_symbol(dlinker* dl, const char* name, void** addr)
{
	struct dl_symbol* sym = dl_get_symbol(dl, name);
	if(sym->providers) 
		/* resolve now */
		*addr = sym->addr;
	else {
		/* chain to the unresolved links */
		*addr = sym->unresolved;
		sym->unresolved = addr;
	}
}



/* Called to load a module */
void dl_load(dlinker* dl, struct dl_module* mod)
{
	/* Execute the module in a fast context */
	mod->stack_size = 1<<16;
	mod->alloc_sp = malloc(mod->stack_size);

	/* stack grows towards 0 */
	void* stack = mod->alloc_sp + mod->stack_size;

	void trampoline(struct transfer_t trans)
	{
		dl->__retcontext = trans.fctx;
		mod->func(dl);
	}

	fcontext_t module_ctx = make_fcontext(stack, mod->stack_size, trampoline);
	struct transfer_t t = jump_fcontext(module_ctx, NULL);

	mod->saved_sp = t.data;
	fprintf(stderr, "Used stack = %lu\n", mod->stack_size - (mod->saved_sp - mod->alloc_sp));
}

/* Called at the end of a module to provide it to the dynamic linker in a prepared state */
void dl_provide(dlinker* dl)
{
	/* Get the context to resume */
	void* stack_top = __builtin_frame_address(0);
	jump_fcontext(dl->__retcontext, stack_top);
}


int dl_load_next(dlinker* dl)
{
	/* Check if load list is empty */
	if(dl->load_list == NULL) return 0;

	/* Pop module from the load list */
	struct dl_module* dlmod = dl->load_list;
	dl->load_list = dlmod->load_next;

	/* load it */
	dl_load(dl, dlmod);
	return 1;	
}



/* Some macros for convenience */

#define EXTERN(name) (*__extern_## name)
#define BIND(name)	 dl_bind_symbol(__dl, #name, (void**) &__extern_##name)
#define EXPORT(name) dl_export_symbol(__dl, #name, &name)


void Lib(dlinker* __dl)
{

	int exported_x=0;

	int print_message(void) {
		printf("Hello from lib, x=%d\n", exported_x);
		return 10;
	}

	EXPORT(print_message);
	EXPORT(exported_x);
	dl_provide(__dl);
}


void Prog(dlinker* __dl)
{
	int EXTERN(exported_x);
	BIND(exported_x);
#define exported_x  EXTERN(exported_x)

	int EXTERN(print_message)(void);
	BIND(print_message);
#define print_message EXTERN(print_message)


	int Main(int argl, void* args)
	{
		exported_x = 10;
		print_message();
		printf("The value of x is %d\n", exported_x);
		return 42;
	}

	EXPORT(Main);

#undef exported_x
#undef print_message
	dl_provide(__dl);
};



BARE_TEST(test_dlinker_binding, "Test dlinker binding")
{
	int a;
	int *pa1, *pa2, *pa3;
	dlinker dl;
	dl_init(&dl);

	dl_bind_symbol(&dl, "a", (void**) &pa1);
	dl_bind_symbol(&dl, "a", (void**) &pa2);

	dl_export_symbol(&dl, "a", &a);
	dl_bind_symbol(&dl, "a", (void**) &pa3);

	ASSERT(&a == pa1);
	ASSERT(&a == pa2);
	ASSERT(&a == pa3);

	dl_destroy(&dl);
}

BOOT_TEST(test_dlink,"Test dynamic linking")
{
	/* A dynamic linker */
	dlinker DL;
	dl_init(&DL);

	struct dl_module *prog, *lib;
	prog = dl_require(&DL, Prog);
	lib = dl_require(&DL, Lib);

	ASSERT(DL.load_list == lib);
	dl_load_next(&DL);

	ASSERT(DL.load_list == prog);
	dl_load_next(&DL);	

	ASSERT(DL.load_list == NULL);

	/* Get Main */
	struct dl_symbol* sym_Main = dl_get_symbol(&DL, "Main");
	ASSERT(sym_Main != NULL);
	ASSERT(sym_Main->addr != NULL);

	int (*Main)() = sym_Main->addr;

	Pid_t pid = Spawn(Main, 0, NULL);

	int status;
	ASSERT(WaitChild(pid, &status)==pid);
	ASSERT(status==42);

	return 0;
}

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

size_t get_file_size(const char* filename)
{
	struct stat st;
	stat(filename, &st);
	return st.st_size;
}

int set_up_memory(const char* filename, void* loc, size_t size)
{
	int fd = open(filename, O_RDONLY);
	FATAL_ASSERT(fd>=0);
	int rc = read(fd, loc, size);
	close(fd);
	return rc;
}

#define PAGESIZE (1<<12)

void * mm_handle;
size_t mm_size;
size_t pgfaults;

int addr_in_range(void* addr, void* start, size_t size) 
{
	return (addr>=start && (addr-start) < size);
}

void* pageof(void* addr) 
{
	uintptr_t ia = (uintptr_t)addr;
	ia &= ~(PAGESIZE-1);
	return (void*)ia;
}


void sigsegv_handler(int signum, siginfo_t* si, void* ctx)
{
	fprintf(stderr, "Segfault\n");
	if(signum == SIGSEGV && 
		si->si_code == SEGV_ACCERR && 
		addr_in_range(si->si_addr, mm_handle, mm_size)) 
	{
		CHECK(mprotect(pageof(si->si_addr), PAGESIZE, PROT_READ|PROT_WRITE));
		pgfaults++;
		atomic_signal_fence(memory_order_seq_cst);
	} else {
		psiginfo(si, "Unrecognized segfault");
		abort();
	}
}


BOOT_TEST(test_mm,"mm test")
{

	/* 
		Reserve a huge amount of address space, make all of it segfault on any access
	 */

	mm_size = 1ul << 40;
	mm_handle = mmap(NULL, mm_size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE|MAP_NORESERVE, -1, 0);
	//mm_handle = mmap(NULL, mm_size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
	if(! mm_handle) { perror("test_mm"); }
	FATAL_ASSERT(mm_handle);
	CHECK(mprotect(mm_handle, mm_size, PROT_NONE));

	/*
		Set up handler for SIGSEGV
	 */

	stack_t ss = { .ss_sp = xmalloc(1<<14), .ss_size=1<<14, .ss_flags=0 };
	stack_t old_ss;
	CHECK(sigaltstack(&ss, &old_ss));

	struct sigaction sa = { .sa_sigaction=sigsegv_handler, .sa_flags=SA_SIGINFO|SA_ONSTACK|SA_NODEFER };
	struct sigaction saved_sa;
	CHECK(pthread_sigmask(0, NULL, &sa.sa_mask));
	CHECK(sigaction(SIGSEGV, &sa, &saved_sa));

	pgfaults=0;  /* Will count page faults */
	atomic_signal_fence(memory_order_seq_cst);

	/*
		Read some data
	 */
	const char* fname = "Makefile";
	size_t fsize = get_file_size(fname);
	char fdata[fsize];
	int rc = set_up_memory(fname, fdata, fsize+ (1<<12));
	ASSERT(fsize==rc);

	size_t psize = (fsize + PAGESIZE-1) / PAGESIZE;

	/* Cause a segfault */
	memcpy(mm_handle, fdata, fsize);
	ASSERT(memcmp(mm_handle, fdata, fsize)==0);
	CHECK(madvise(mm_handle, psize, MADV_DONTNEED));
	ASSERT(memcmp(mm_handle, fdata, fsize)!=0);

	/* Cause another segfault */
	int* xp = (int*)(mm_handle+2*PAGESIZE);
	* xp= 10;
	ASSERT(*xp == 10);

	/* check number of segfaults */

	/* Cleanup */
	CHECK(munmap(mm_handle, mm_size));
	CHECK(sigaction(SIGSEGV, &saved_sa, NULL));
	CHECK(sigaltstack(&old_ss, NULL));

	return 0;
}




TEST_SUITE(fsystem_tests, "Filesystem tests")
{
	&test_memfs_is_root,
	&test_pathcomp,
	&test_dlinker_binding,
	&test_dlink,
	&test_mm,
	NULL
};


int main(int argc, char** argv)
{
	return register_test(&fsystem_tests) || 
		run_program(argc, argv, &fsystem_tests);
}
