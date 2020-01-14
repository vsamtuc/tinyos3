#undef NDEBUG
#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <setjmp.h>

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


struct _pair { const char* name; Program prog; };

void print_table(const char* tname, struct _pair* p)
{
	MSG("Table: %s\n", tname);
	if(p==NULL) MSG("    NULL table\n");
	while(p->prog) {
		MSG("  Name: %s \n", p->name);
		p++;
	}
}


void execute_program(void* handle)
{
	Program test_program = dlsym(handle,"test_program");
	FATAL_ASSERT_MSG(test_program, "dlerror: %s", dlerror());
	FATAL("We cannot run yet, fix needed")
#if 0
	ASSERT(Execute(test_program,0,NULL)!=NOPROC);
#endif
	WaitChild(NOPROC,NULL);
}


void print_section(struct tos_entity* start, struct tos_entity* stop)
{
	MSG("REGISTERED:\n");
	for(struct tos_entity* p = start; p!= stop; p++) {
		MSG("Name: %s\n", p->name);

		Dl_info dli;
		int rc = dladdr(p->data, &dli);
		if(rc==0)
			MSG(" Info error: %s\n", dlerror());
		else
			MSG(" Info: fname=%s fbase=%p  sname=%s saddr=%p\n", 
				dli.dli_fname, dli.dli_fbase, dli.dli_sname, dli.dli_saddr);


		WaitChild(NOPROC, NULL);
	}
	MSG("END REGISTERED:\n");
}

void print_dlsection(void* handle2)
{
	ASSERT(dlsym(handle2,"__start_tinyos"));
	struct tos_entity** s2 = dlsym(handle2,"__begin_tinyos");
	struct tos_entity** e2 = dlsym(handle2,"__end_tinyos");
	FATAL_ASSERT(s2);
	FATAL_ASSERT(e2);
	print_section(*s2, *e2);	
}

BOOT_TEST(test_load_shared,"Test that loading a shared object and executing a program from it works")
{
	FATAL_ASSERT(Mount(NO_DEVICE, "/", "tmpfs", 0, NULL)==0);
	FATAL_ASSERT(MkDir("/dev")==0);
	FATAL_ASSERT(Mount(NO_DEVICE, "/dev", "devfs", 0, NULL)==0);

	void* handle = dlopen("./testprog.so", RTLD_NOW|RTLD_LOCAL);
	FATAL_ASSERT_MSG(handle, "dlerror: %s", dlerror());

	void* h1 = dlopen("./testprog.so", RTLD_NOW|RTLD_LOCAL);
	FATAL_ASSERT_MSG(h1, "dlerror: %s", dlerror());
	ASSERT(h1==handle);
	dlclose(h1);


	void* handle2 = dlopen("./testprog2.so", RTLD_NOW|RTLD_LOCAL);
	FATAL_ASSERT_MSG(handle, "dlerror: %s", dlerror());

	execute_program(handle);
	execute_program(handle2);
	
	print_dlsection(handle);
	print_dlsection(handle2);

	ASSERT(dlclose(handle)==0);
	ASSERT(dlclose(handle2)==0);

	return 0;
}




TEST_SUITE(fsystem_tests, "Filesystem tests")
{
	&test_memfs_is_root,
	&test_pathcomp,
	&test_dlinker_binding,
	&test_dlink,
	&test_load_shared,
	NULL
};


int main(int argc, char** argv)
{
	return register_test(&fsystem_tests) || 
		run_program(argc, argv, &fsystem_tests);
}
