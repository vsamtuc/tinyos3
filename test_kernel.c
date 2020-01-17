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

	mm_size = 1ul << 36;
	mm_handle = mmap(NULL, mm_size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE|MAP_NORESERVE, -1, 0);
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


/* 
	An ordered tree built using a 'less' function.

	We use rlnodes, with prev<->left and next<->right.
	We implement the red-black algorithm, as follows:

	we treat the tree as a 2-3 tree, i.e., each 't-node' is 
	either a single rlnode or a pair of rlnodes. Thus, each
	t-node holds 1 or 2 keys and 2 or 3 pointers. 

	A t-node is a list of rlnodes, linked by a marked pointer
	in the 'next' field.
*/


#if 0
typedef rlnode* tnode[2];
typedef int (*rtree_less)(rlnode*, rlnode_key key);


static inline void load_tnode(tnode N, rlnode* n) {
	N[0] = n;
	if(pointer_is_marked(n->next))
		N[1] = pointer_unmarked(n->next);
	else
		N[1] = NULL;
}


static inline int tnode_is_leaf(tnode N) { return N[0]->prev == NULL; }
static inline unsigned int tnode_size(tnode N) { return N[1] ? 2 : 1; }
static inline rlnode** tnode_subtree(tnode N, unsigned int i) {
	assert (i<= tnode_size(N));
	if(i==0) return & N[0]->prev;
	if(N[1]) return (i==1) ? & N[1]->prev : & N[1]->next;
	else return & N[0]->next;
} 
static inline unsigned int tnode_find_subtree(tnode N, rlnode_key key, rtree_less lessf)
{
	uint tns = tnode_size(N);
	for(uint i=0; i<tns; i++) if(!lessf(N[i],key)) return i;
	return tns; 
}

/*
	If node==NULL, return NULL (no insertion). Assume node != 0
	It is node->next == NULL. 

	Think of node as  (cp, n)  where cp==node->prev 

	Think of a tnode as  
	either  (c, k) c            child == 0/1
	or      (c,k) (c,k), c      child == 0/1/2

	Do insert (depending on child) one of 5 cases

	child==0:   (cp, n) (c,k) c   or  (cp,n) (c,k) (c,k) c  
	child==1:   (c, k)  (cp,n) c  or  (c, k) (cp,n) (c,k) c
	child==2:                         (c, k) (c, k) (cp, n) c

	On overflow (right column), generate bubble up as the leftmost pair
*/
static inline rlnode* tnode_insert(tnode N, rlnode* node, uint child)
{
	if(node==NULL) return NULL;
	assert(node->next == NULL);
	assert(child<=tnode_size(N));

	rlnode* bup = NULL;

	if(N[1]) {
		/* We split */
		rlnode* M; /* The left bloc of the split */
		if(child==0) {
			M=node; bup = N[0]; N[0]=N[1];
		} else if(child==1) {
			M=N[0]; bup = node; N[0]=N[1];
		} else {
			M = N[0]; bup = N[1]; N[0]=node;
			N[0]->next = bup->next;
		}
		N[1] = NULL;
		M->next = bup->prev; 
		bup->prev = M;
		bup->next = NULL;
	} else {
		if(child==0) {
			N[1] = N[0];
			N[0] = node; 
		} else {
			N[1] = node;
			node->next = N[0]->next;
		}
		N[0]->next = pointer_marked(N[1]);
	}
	return bup;
}

/*
	Precondition: node->key not equal to any node in the tree
 */
rlnode* rtree_insert(rlnode* tree, rlnode* node, rtree_less lessf)
{
	/* prepare node */
	node->prev = node->next = NULL; 

	/* base case */
	if(tree==NULL) return node;


	/* Insert into tree, return root of new tree */
	rlnode* insert(tnode N)
	{
		/* bubble-up */
		rlnode* bup;
		uint child = tnode_find_subtree(N, node->key, lessf);

		/* Reach bottom */
		if(! tnode_is_leaf(N)) {
			rlnode ** subtree = tnode_subtree(N,child);
			tnode Child;  load_tnode(Child, *subtree);

			/* Get bubble up from subtree */
			bup = insert(Child);

			/* child possibly changed, update it */
			*subtree = Child[0];
		} else 
			bup = node;
		
		/* We need to insert bubble-up into this tnode */
		rlnode* mybup = tnode_insert(N, bup, child);
		return mybup;
	}

	tnode T; load_tnode(T, tree);
	rlnode* bup = insert(T);
	if(bup) {
		bup->next = T[0];
		return bup;
	}
	return T[0];
}


rlnode* rtree_remove(rlnode* tree, rlnode* node)
{
	
}
#endif 

static inline int  _red(rlnode* n) { return n && pointer_is_marked(n->next); }
static inline void _set_red(rlnode* n) {n->next =  pointer_marked(n->next); }
static inline void _set_black(rlnode* n) {n->next =  pointer_unmarked(n->next); }
static inline void _color_flip(rlnode* n) { n->next =  pointer_mark_flipped(n->next); }

static inline void _set_color(rlnode* n, int red) { if(red) _set_red(n); else _set_black(n); }

static inline rlnode* _left(rlnode* n) { return n->prev; }
static inline rlnode* _right(rlnode* n) { return pointer_unmarked(n->next); }
static inline rlnode* _set_left(rlnode* n, rlnode* v) { n->prev = v; return v; }
static inline rlnode* _set_right(rlnode* n, rlnode* v) {
	n->next = pointer_is_marked(n->next) ? pointer_marked(v) : pointer_unmarked(v);
	return v;
}


static inline rlnode* _rotate_left(rlnode* h) 
{
	rlnode* x = _right(h);
	_set_right(h, _left(x));
	_set_left(x, h);
	_set_color(x, _red(h));
	_set_red(h);
	return x;
}
static inline rlnode* _rotate_right(rlnode* h)
{
	rlnode* x = _left(h);
	_set_left(h, _right(x));
	_set_right(x, h);
	_set_color(x, _red(h));
	_set_red(h);
	return x;	
}
static inline void _flip_colors(rlnode* h)
{
	_color_flip(h);
	_color_flip(_left(h));
	_color_flip(_right(h));
}

void rtree_init(rlnode* node, rlnode_key key)
{
	node->key = key;
	node->prev = node->next = NULL;
	_set_red(node);
}


/** 
	@brief Comparator function 

	Return -1, 0 or 1, depending on whether the two compared
	keys are <, ==  or > to each other
 */
typedef int (*rtree_cmp)(rlnode_key key1, rlnode_key key2);

rlnode* rtree_search(rlnode* tree, rlnode_key key, rtree_cmp cmpf)
{
	rlnode* x = tree;
	while(x) {
		int cmp = cmpf(key, x->key);
		if(cmp==0) return x;
		else x = (cmp<0)? _left(x) : _right(x);
	}
	return NULL;
}

rlnode* rtree_insert(rlnode* tree, rlnode* node, rtree_cmp cmpf)
{
	_set_red(node);

	rlnode* insert(rlnode* h) {
		if(h==NULL) return node;

		if(_red(_left(h)) && _red(_right(h)) ) _color_flip(h);

		int cmp = cmpf(node->key, h->key);
		assert(cmp); /* No duplicates allowed ! */
		if(cmp<0)  _set_left(h, insert(_left(h)));
		else       _set_right(h, insert(_right(h)));

		if(_red(_right(h)) && !_red(_left(h))) h = _rotate_left(h);
		if(_red(_left(h)) && _red(_left(_left(h)))) h = _rotate_right(h);

		return h;
	}

	tree = insert(tree);
	_set_black(tree);
	return tree;
}



void rtree_apply(rlnode* tree, void (*func)(rlnode* node))
{
	if(tree==NULL) return;
	rtree_apply(_left(tree), func);
	func(tree);
	rtree_apply(_right(tree), func);
}

rlnode_key rtree_reduce(rlnode* tree, rlnode_key (*reducer)(rlnode*, rlnode_key, rlnode_key), rlnode_key nullval)
{
	if(tree) {
		rlnode_key lval = rtree_reduce(_left(tree), reducer, nullval);
		rlnode_key rval = rtree_reduce(_right(tree), reducer, nullval);
		return reducer(tree, lval, rval);
	} else
		return nullval;
}


rlnode_key rtree_height(rlnode* tree)
{
	rlnode_key hred(rlnode* node, rlnode_key lval, rlnode_key rval) {
		uintptr_t lh = lval.unum;
		uintptr_t rh = rval.unum;

		return (rlnode_key) { .unum = (lh<rh ? rh : lh )+1 };
	}
	return rtree_reduce(tree, hred, 0);
}


static inline int uint_cmp(uintptr_t x, uintptr_t y) { return (x==y) ? 0 : (x<y) ? -1 : 1; }

int rtree_uint_cmp(rlnode_key a, rlnode_key b) { return uint_cmp(a.unum, b.unum); }
int qsort_cmp(const void* a, const void* b) { return uint_cmp(*(uintptr_t*)a, *(uintptr_t*)b); }


void rlnode_print_uint(rlnode* node)
{
	printf("%lu ", node->unum);
}

BARE_TEST(test_build_rtree1,"Test building and traversing rtree, random insertions")
{
	uintptr_t output[55];
	uintptr_t expected[55];
	size_t pos = 0;

	void rlnode_output_uint(rlnode* node)
	{
		output[pos++] = node->unum;
		printf("%lu ", node->unum);
	}


	rlnode* T = NULL;
	for(uint i=0; i<55; i++) {
		uintptr_t val = (i*34)%55; /* Unique value (Fibonacci numbers 34 and 55) */

		rlnode* node = alloca(sizeof(rlnode)); 
		rtree_init(node, val);		
		T = rtree_insert(T, node, rtree_uint_cmp);

		pos = 0;
		rtree_apply(T, rlnode_output_uint); printf("\n");
		ASSERT(i+1==pos);
		expected[i] = val;
		qsort(expected, pos, sizeof(uintptr_t), qsort_cmp);
		ASSERT(memcmp(output, expected, pos*sizeof(uintptr_t))==0);
	}
	ASSERT(T != NULL);
}

BARE_TEST(test_build_rtree2,"Test building and traversing rtree, increasing insertions")
{
	uintptr_t output[55];
	uintptr_t expected[55];
	size_t pos = 0;

	void rlnode_output_uint(rlnode* node)
	{
		output[pos++] = node->unum;
	}


	rlnode* T = NULL;
	for(uint i=0; i<55; i++) {
		uintptr_t val = i; 

		rlnode* node = alloca(sizeof(rlnode));
		rtree_init(node, val);		
		T = rtree_insert(T, node, rtree_uint_cmp);

		pos = 0;
		rtree_apply(T, rlnode_output_uint);
		ASSERT(i+1==pos);
		expected[i] = val;
		qsort(expected, pos, sizeof(uintptr_t), qsort_cmp);
		ASSERT(memcmp(output, expected, pos*sizeof(uintptr_t))==0);
	}
	ASSERT(T != NULL);
}

BARE_TEST(test_build_rtree3,"Test building and traversing rtree, decreasing insertions")
{
	uintptr_t output[55];
	uintptr_t expected[55];
	size_t pos = 0;

	void rlnode_output_uint(rlnode* node)
	{
		output[pos++] = node->unum;
	}


	rlnode* T = NULL;
	for(uint i=0; i<55; i++) {
		uintptr_t val = 55-i;

		rlnode* node = alloca(sizeof(rlnode));
		rtree_init(node, val);		
		T = rtree_insert(T, node, rtree_uint_cmp);

		pos = 0;
		rtree_apply(T, rlnode_output_uint);
		ASSERT(i+1==pos);
		expected[i] = val;
		qsort(expected, pos, sizeof(uintptr_t), qsort_cmp);
		ASSERT(memcmp(output, expected, pos*sizeof(uintptr_t))==0);
	}
	ASSERT(T != NULL);
}




TEST_SUITE(fsystem_tests, "Filesystem tests")
{
	&test_memfs_is_root,
	&test_pathcomp,
	&test_dlinker_binding,
	&test_dlink,
	&test_mm,
	&test_build_rtree1,
	&test_build_rtree2,
	&test_build_rtree3,
	NULL
};


int main(int argc, char** argv)
{
	return register_test(&fsystem_tests) || 
		run_program(argc, argv, &fsystem_tests);
}
