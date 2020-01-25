
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <setjmp.h>
#include "util.h"

#include "unit_testing.h"

#undef NDEBUG
#include <assert.h>


/*****************************************************
 *
 *  Tests for rlist
 *
 ******************************************************/


/* Make a list with dynamically allocated nodes, each node containing
	one character from the string @c data.
 */
static rlnode* make_list(rlnode* L, const char* data)
{
	rlnode_init(L,NULL);
	for(const char* d =data; *d; d++) {
		rlnode* node = (rlnode*) malloc(sizeof(rlnode));
		rlnode_new(node)->num = *d;
		rlist_push_back(L, node);
	}
	ASSERT(rlist_len(L)==strlen(data));
	return L;
}


/* Some data */

static const int NData = 5;
static const char* Data[5] = { "", "H", "Ha", "a", "asdas" };


BARE_TEST(test_list_init,
	"Test list creation, initialization and simple splicing"
	)
{
	rlnode L; rlnode_init(&L,NULL);
	ASSERT(L.obj==NULL);
	ASSERT(L.prev == L.next);
	ASSERT(L.prev == &L);


	rlnode n1, n2; 
	rlnode_init(&n1, &L);
	rlnode_new(&n2)->num = 5;
	ASSERT(n2.num==5);
	ASSERT(n2.key.num == 5);

	rlnode R = { .obj= NULL, .prev=&R, .next=&R };
	ASSERT(is_rlist_empty(&R));
	ASSERT(is_rlist_empty(&L));
	ASSERT(is_rlist_empty(&n1));
	ASSERT(is_rlist_empty(&n2));

	rl_splice(&n1, &n2);
	ASSERT(! is_rlist_empty(&n1));
	ASSERT(! is_rlist_empty(&n2));
	ASSERT(n1.next == &n2);
	ASSERT(n2.next == &n1);
	ASSERT(n1.prev == &n2);
	ASSERT(n2.prev == &n1);
	ASSERT(rlist_len(&n1)==1);

	rl_splice(&n1, &n2);
	ASSERT(is_rlist_empty(&n1));
	ASSERT(is_rlist_empty(&n2));
	ASSERT(rlist_len(&n1)==0);
}

BARE_TEST(test_list_len,
	"Test the list length and removal"
	)
{
	rlnode L;  rlnode_new(&L);
	rlnode n[10];
	for(int i=0;i<10;i++) {
		ASSERT(rlist_len(&L)==i);
		rlnode_new(n+i)->num = i;
		rlist_push_back(&L, n+i);
		ASSERT(rlist_len(&L)==i+1);
	}

	ASSERT(rlist_len(&L)==10);	

	for(int i=0;i<10;i+=2) {
		int l = rlist_len(&L);
		rlist_remove(n+i);
		ASSERT(rlist_len(&L)==l-1);
	}
	for(int i=1;i<10;i+=2) {
		int l = rlist_len(&L);
		rlist_remove(n+i);
		ASSERT(rlist_len(&L)==l-1);
	}

	for(int i=0; i<NData;i++) {
		make_list(&L, Data[i]);
		ASSERT(rlist_len(&L)==strlen(Data[i]));
	}

}

BARE_TEST(test_list_queue,
	"Test the list as a queue"
	)
{
	rlnode L; rlnode_new(&L);
	rlnode n[10];

	for(int i=0;i<10;i++) {
		rlnode_new(n+i);
		rlist_push_front(&L,n+i);
	}

	rlnode* I = L.prev;
	for(int i=0;i<10;i++) {
		ASSERT(I==n+i);
		I = I->prev;
	}

	I = n;
	while(! is_rlist_empty(&L)) {
		rlnode* p = rlist_pop_back(&L);
		ASSERT(I==p);
		I++;
	}

	ASSERT(is_rlist_empty(&L));

	I = rlist_pop_back(&L);   /* The list is empty, but the pop_back method does not mind! */
	ASSERT(I==&L);	
	ASSERT(is_rlist_empty(&L));
}



void build_list(rlnode* L, rlnode* from, rlnode* to, void* key, size_t esize) 
{
	for(; from!=to; from++) {
		rlnode_init(from, key);
		rlist_push_back(L,from);
		key += esize;
	}
}


BARE_TEST(test_list_select,
	"Test list selection"
	)
{
	short int a[] = { 4,2,5,7,8,3,2,9,3 };
	rlnode L; rlnode_new(&L);
	rlnode nodes[9];

	ASSERT(rlist_len(&L)==0);
	build_list(&L, nodes, nodes+9, a, sizeof(short int));
	ASSERT(rlist_len(&L)==9);

	int i=0;
	for(rlnode* p=L.next; p!=&L; p=p->next) {
		ASSERT(p->obj==(a+i));
		i++;
	}

	// test select
	rlnode Q; rlnode_new(&Q);
	int gt5(rlnode* p) { 
		return *((short int*)(p->obj)) >= 5; 
	}
	rlist_select(&L, &Q, gt5);
	ASSERT(rlist_len(&Q)==4);
	ASSERT(rlist_len(&L)==5);
}


BARE_TEST(test_list_equal,
	"Test list equality"
	)
{
	rlnode L1, L2;

	const int N = 5;
	const char* D[6] = {
		"",
		"H",
		"Ha",
		"a", 
		"asdas"
	};

	for(int i=0;i<N;i++)
		for(int j=0;j<N;j++) 
		{
			make_list(&L1, D[i]);
			make_list(&L2, D[j]);
			ASSERT( (rlist_equal(&L1, &L2)? 1 : 0) == ((i==j) ? 1 : 0) );
		}
}


BARE_TEST(test_list_prepend,
	"Test list prepending"
	)
{
	rlnode Ld, Ls, Le, La;

	rlnode_init(&Ld, NULL);
	rlnode_init(&Ls, NULL);
	rlnode_init(&Le, NULL);
	rlnode_init(&La, NULL);

	short int a[9] = { 4,2,5,7,8,3,2,9,3 };
	rlnode nodes[18];

	build_list(&Ld, nodes, nodes+5, a, sizeof(short int));
	build_list(&Ls, nodes+5, nodes+9, a+5, sizeof(short int));	
	build_list(&La, nodes+9, nodes+18, a, sizeof(short int));

	rlist_prepend(&Ls, &Le);
	ASSERT(is_rlist_empty(&Le));
	rlist_prepend(&Le, &Ls);
	ASSERT(is_rlist_empty(&Ls));
	rlist_prepend(&Le, &Ld);
	ASSERT(is_rlist_empty(&Ld));
	ASSERT(rlist_len(&Le)==9);
	ASSERT(rlist_equal(&Le, &La));


}

BARE_TEST(test_list_append,
	"Test list appending"
	)
{
	rlnode L1, L2, L3;
	make_list(&L1, "Hello ");
	make_list(&L2, "world");
	rlnode_init(&L3,NULL);

	rlist_append(&L3, &L1);
	rlist_append(&L3, &L2);

	ASSERT(rlist_equal(&L3, make_list(&L1, "Hello world")));

}


TEST_SUITE(rlist_tests,
	"Tests for the resource list")
{
	&test_list_init,
	&test_list_len,
	&test_list_queue,
	&test_list_select,
	&test_list_equal,
	&test_list_prepend,
	&test_list_append,
	NULL
};



/*****************************************************
 *
 *  Tests for rheap
 *
 ******************************************************/


static inline int num_less(rlnode* a, rlnode* b) 
{
	return a->num < b->num; 
}


void check_legal_heap(rlnode* heap, rlnode_less_func lessf)
{
	if(heap==NULL) return;
	for(rlnode* p=heap->prev; !pointer_is_marked(p); p=p->next) {
		check_legal_heap(p, lessf);
		ASSERT( ! lessf(p,heap));
	}
}


rlnode* build_int_ring(rlnode* from, size_t n, int* key) 
{
	rlnode L; rlnode_init(&L,NULL);
	for(size_t i=0; i<n; i++) {
		rlnode_new(from)->num = *(key++);
		rlist_push_back(&L,from++);
	}
	return rl_splice(&L, L.prev);
}


void print_int_heap(rlnode* heap)
{
	if(heap==NULL) return;
	printf("%lu:[ ",heap->num);
	for(rlnode* p = heap->prev; !pointer_is_marked(p); p=p->next) {
		if(p!=heap->prev)
			printf(" | ");
		print_int_heap(p);
	}
	printf(" ]");
}

void rheap_unlink(rlnode* node);


BARE_TEST(test_rheap_init, "Heap initialization")
{
	int a[] = { 4,2,5,7,8,3,2,9,3 };
	rlnode nodes[9];
	rlnode* ring = build_int_ring(nodes, 9, a);

	rlnode* heap = rheap_from_ring(ring, num_less);
	ASSERT(rheap_size(heap)==9);
	check_legal_heap(heap, num_less);
	rheap_unlink(nodes+3);
	check_legal_heap(nodes+3, num_less);
	ASSERT(rheap_size(heap)+rheap_size(nodes+3) == 9);

	ASSERT(heap != NULL);
	int hsize = rheap_size(heap);
	while(heap != NULL) {
		hsize--;
		heap = rheap_delmin(heap, num_less);
		check_legal_heap(heap, num_less);
	}
	ASSERT(hsize==0);

	check_legal_heap(NULL, num_less);
}


BARE_TEST(test_rheap_delmin, "Test delmin via heapsort with pairing heaps")
{
	int a[] = { 4,2,5,7,8,3,2,9,3 };
	rlnode nodes[9];
	rlnode* ring = build_int_ring(nodes, 9, a);

	rlnode* heap = rheap_from_ring(ring, num_less);
	ASSERT(rheap_size(heap)==9);	

	int p = heap->num;  
	while( (heap = rheap_delmin(heap, num_less)) != NULL ) {
		int q = heap->num;
		ASSERT( p <= q );
		p = q;
	}
}


BARE_TEST(test_rheap_delete, "Heap deletion")
{
	int a[] = { 4,2,5,7,8,3,2,9,3 };
	rlnode nodes[9];
	rlnode* ring = build_int_ring(nodes, 9, a);

	rlnode* heap = rheap_from_ring(ring, num_less);
	check_legal_heap(heap, num_less);
	ASSERT(rheap_size(heap)==9);	

	int i=8;
	while( (heap = rheap_delete(heap, &nodes[i], num_less)) != NULL ) {
		check_legal_heap(heap, num_less);

		ASSERT( rheap_size(heap) == i );
		ASSERT( rheap_size(&nodes[i]) == 1 );		
		i--;
	}

}

BARE_TEST(test_rheap_meld, "Heap melding")
{
	ASSERT(rheap_meld(NULL, NULL, num_less)==NULL);
	rlnode node;
	rheap_init(&node)->num=22;

	rlnode* heap = &node;
	check_legal_heap(heap, num_less);
	ASSERT(rheap_meld(NULL, NULL, num_less)==NULL);

	ASSERT(rheap_meld(heap, NULL, num_less)==heap);
	ASSERT(rheap_meld(NULL, heap, num_less)==heap);

}


TEST_SUITE(rheap_tests,
	"Test for the resource priority queue")
{
	&test_rheap_init,
	&test_rheap_delmin,
	&test_rheap_delete,
	&test_rheap_meld,
	NULL
};


/*****************************************************
 *
 *  Tests for rtree
 *
 ******************************************************/


void rtree_apply(rlnode* tree, void (*func)(rlnode* node))
{
	if(tree==NULL) return;
	rtree_apply(rtree_left(tree), func);
	func(tree);
	rtree_apply(rtree_right(tree), func);
}

rlnode_key rtree_reduce(rlnode* tree, rlnode_key (*reducer)(rlnode*, rlnode_key, rlnode_key), rlnode_key nullval)
{
	if(tree) {
		rlnode_key lval = rtree_reduce(rtree_left(tree), reducer, nullval);
		rlnode_key rval = rtree_reduce(rtree_right(tree), reducer, nullval);
		return reducer(tree, lval, rval);
	} else
		return nullval;
}


void rtree_check_invariants(rlnode* tree)
{
	/* Invariants:
		(1) if a node is red, both children are black
		(2) number of black nodes to every leaf is equal
	 */
	inline int is_red(rlnode* h) { return h && pointer_is_marked(h->next); }

	size_t check(rlnode* h)
	{
		size_t lbh=0, rbh=0;
		if(h) {
			lbh = check(rtree_left(h));
			rbh = check(rtree_right(h));
		}
		ASSERT(!  (is_red(h) && (is_red(rtree_left(h)) || is_red(rtree_right(h))) ));
		ASSERT_RELATION(lbh, rbh, "%zu", _1==_2);
		return lbh + (is_red(h)? 0: 1);
	}
	check(tree);
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
	}


	rlnode* T = NULL;
	rtree_check_invariants(T);

	rlnode* NODES[55];

	for(uint i=0; i<55; i++) {
		uintptr_t val = (i*34)%55; /* Unique value (Fibonacci numbers 34 and 55) */

		rlnode* node = alloca(sizeof(rlnode)); 
		NODES[i] = node;
		rtree_init(node, val);		
		T = rtree_insert(T, node, rtree_uint_cmp);
		rtree_check_invariants(T);

		pos = 0;
		rtree_apply(T, rlnode_output_uint);
		ASSERT(i+1==pos);
		expected[i] = val;
		qsort(expected, pos, sizeof(uintptr_t), qsort_cmp);
		ASSERT(memcmp(output, expected, pos*sizeof(uintptr_t))==0);
	}
	ASSERT(T != NULL);

	for(uint i=0; i<55; i++) {
		rlnode* node = NODES[i];
		T = rtree_delete(T, node, rtree_uint_cmp);
		rtree_check_invariants(T);		
	}	

	ASSERT(T == NULL);
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
	rtree_check_invariants(T);
	for(uint i=0; i<55; i++) {
		uintptr_t val = i; 

		rlnode* node = alloca(sizeof(rlnode));
		rtree_init(node, val);		
		T = rtree_insert(T, node, rtree_uint_cmp);
		rtree_check_invariants(T);

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
	rtree_check_invariants(T);
	for(uint i=0; i<55; i++) {
		uintptr_t val = 55-i;

		rlnode* node = alloca(sizeof(rlnode));
		rtree_init(node, val);		
		T = rtree_insert(T, node, rtree_uint_cmp);
		rtree_check_invariants(T);

		pos = 0;
		rtree_apply(T, rlnode_output_uint);
		ASSERT(i+1==pos);
		expected[i] = val;
		qsort(expected, pos, sizeof(uintptr_t), qsort_cmp);
		ASSERT(memcmp(output, expected, pos*sizeof(uintptr_t))==0);
	}
	ASSERT(T != NULL);
}


BARE_TEST(test_rtree_ops,"Test inserting and deleting keys in random order")
{


	rlnode* NODES[55];
	for(uint i=0; i<55; i++) {
		uintptr_t val = (i*34)%55;
		NODES[i] = alloca(sizeof(rlnode));
		rtree_init(NODES[i], val);
	}

	uint intree = 0;

	rlnode* T = NULL;
	rtree_check_invariants(T);
	for(uint i=0; i<100000; i++) {
		/* choose a random number in [0:55) */
		uint j = lrand48() % (i%55 +1);

		if(j>=intree) {
			/* swap NODES[j] and NODES[intree] */
			rlnode* tmp = NODES[intree];
			NODES[intree] = NODES[j];
			NODES[j] = tmp;

			T = rtree_insert(T, NODES[intree], rtree_uint_cmp);
			rtree_check_invariants(T);

			intree++;
		} else {
			intree--;

			/* swap NODES[j] and NODES[intree] */
			rlnode* tmp = NODES[intree];
			NODES[intree] = NODES[j];
			NODES[j] = tmp;

			T = rtree_delete(T, NODES[intree], rtree_uint_cmp);
			rtree_check_invariants(T);			
		}
	}
}





TEST_SUITE(rtree_tests, "Tests for resource tree")
{
	&test_build_rtree1,
	&test_build_rtree2,
	&test_build_rtree3,
	&test_rtree_ops,
	NULL
};




/*****************************************************
 *
 *  Tests for exceptions
 *
 ******************************************************/



BARE_TEST(test_dict_init, "Initialization of rdict")
{
	rdict dict;
	unsigned long sizes[7] = { 0, 1, 2, 5, 6, 20, 1000 };

	unsigned long expected[7] = { 5, 5, 5, 11, 11, 23, 1741 };
	for(int i=0; i<7; i++) {
		rdict_init(&dict, sizes[i]); 
		ASSERT_RELATION(dict.bucketno, expected[i], "%lu", _1==_2);
		rdict_destroy(&dict);
	}
}


BARE_TEST(test_dict_resize, "Resizing")
{
	rdict dict;
	unsigned long sizes[6] =    { 1, 2,  5,  6, 20, 1000 };
	unsigned long expected[6] = { 5, 5, 11, 11, 23, 1741 };

	rlnode node; rdict_node_init(&node, 0, 0);

	ASSERT(rdict_next_greater_prime_size((size_t)0, 0)==5lu);

	for(int i=0; i<6; i++) {
		rdict_init(&dict, 0); 
		ASSERT_RELATION(dict.bucketno, 5lu, "%lu", _1==_2);

		/* Fudge the size !!! */
		dict.size = sizes[i];
		rdict_insert(&dict, &node);

		ASSERT_RELATION(dict.bucketno, expected[i], "%lu", _1==_2);
		rdict_destroy(&dict);
	}
}


int equalty(rlnode* n, rlnode_key key) { return n->key.num == key.num; }

BARE_TEST(test_dict_insert_lookup_remove, "Test the dictionary inserting many elements, looking them up and removing them")
{
	const int N = 1000000;
 
	rdict dict;

	rdict_init(&dict, 0);

	/* Build N elements */
	for(intptr_t i=0;i<N;i++) {
		rlnode* node = malloc(sizeof(rlnode));
		rdict_node_init(node, i, i);
		rdict_insert(&dict, node);

		/* The starndard policy has a threshold of 1 to the load factor */
		ASSERT(dict.bucketno >= dict.size);
	}

	ASSERT(dict.size == N);
	ASSERT(dict.bucketno >= dict.size);

	/* Look them up */
	for(intptr_t i=0;i<N;i++) {
		rdict_iterator iter = rdict_find(&dict, i, i, equalty);
		ASSERT(iter != NULL);
		ASSERT(*iter != NULL);
		ASSERT((*iter)->num == i);
		ASSERT(rdict_lookup(&dict, i,i,equalty)->num == i);

		ASSERT(rdict_find_node(&dict, *iter) == iter);
		ASSERT(rdict_find_next(&dict, iter, i, i, equalty) == rdict_end(&dict));

	}

	/* Scan the elements sequentially and check that we get them all */
	unsigned long count = 0;
	for(rdict_iterator I=rdict_begin(&dict); I!=rdict_end(&dict); I = rdict_next(I)) {
		count ++;
	}
	ASSERT_RELATION(count, dict.size, "%lu", _1==_2);

	/* Remove them out of order */
	for(intptr_t i=0;i<N;i++) {
		rdict_iterator iter = rdict_find(&dict, i, i, equalty);
		ASSERT(iter != NULL);
		ASSERT((*iter)->num == i);
		rlnode* node = *iter;
		ASSERT(rdict_remove(&dict, *iter));
		free(node);
	}

	rdict_destroy(&dict);
}




TEST_SUITE(rdict_tests, "Tests for the hashing data structure rdict")
{
	&test_dict_init,
	&test_dict_resize,
	&test_dict_insert_lookup_remove,
	NULL
};


/*****************************************************
 *
 *  Tests for packing and unpacking
 *
 ******************************************************/




void test_argv(size_t argc, const char* argv[])
{
	int l = argvlen(argc, argv);
	char args[l];
	int argl = argvpack(args, argc, argv);
	ASSERT(argl == l);
	ASSERT(argscount(argl,args)==argc);

	const char* uargv[argc];
	argvunpack(argc, uargv, argl, args);

	for(int i=0;i<argc;i++) {
		ASSERT_MSG(strcmp(argv[i], uargv[i])==0, "In %s(%d): '%s'=='%s' failed for i=%d\n",
			__FILE__, __LINE__, argv[i], uargv[i],i);
	}
}


BARE_TEST(test_pack_unpack,"Test packing and unpacking a argvuence of strings into a (int,void*) pair.")
{
	{
		const char* Aargv[] = { "Hello", "Goodbye", NULL };
		ASSERT(argvlen(2, Aargv)==14);
		test_argv(2,Aargv);		
	}
	{
		const char* Aargv[] = { NULL };
		ASSERT(argvlen(0, Aargv)==0);
		test_argv(0,Aargv);		
	}
	{
		const char* Aargv[] = { "Goodbye", NULL };
		ASSERT(argvlen(1,Aargv)==8);
		test_argv(1,Aargv);		
	}
	{
		const char* Aargv[] = { "", "", NULL };
		ASSERT(argvlen(2,Aargv)==2);
		test_argv(2,Aargv);		
	}
	{
		const char* Aargv[] = { "", NULL };
		ASSERT(argvlen(1,Aargv)==1);
		test_argv(1,Aargv);		
	}
}


/*****************************************************
 *
 *  Tests for exceptions
 *
 ******************************************************/


struct exception_stack_frame* unwind_context;

#define TRY  TRY_WITH(&unwind_context)
#define RAISE_ERROR raise_exception(&unwind_context)


BARE_TEST(test_exc_empty_body,
	"Test exceptions whose body is empty."
	)
{
	volatile int count = 0;

	TRY {		
	};

	TRY {
		FINALLY(e) {
			ASSERT(e==0);
			ASSERT(count==0);
			count++;			
		}
		ON_ERROR{
			ASSERT(0);
		}

	}
	ASSERT(count==1);

	count = 0;
	TRY {
		FINALLY(e) {
			count++;
			ASSERT(count==2);
		}
	
		FINALLY(e) {
			count++;
			ASSERT(count==1);
		}
	
	}
	ASSERT(count==2);

	count = 10;
	TRY {
		FINALLY(e) {
			TRY {
				FINALLY(e) {
					count++;
				}
			}
			count++;
		}
		ASSERT(count==10);
	}
	ASSERT(count==12);
}


BARE_TEST(test_exc_catcher_match,
	"Test the matching of catchers"
	)
{
	int count;

	count=0;
	TRY {
		ON_ERROR{
			count++;
		};
		ON_ERROR{
			count++;
		}
		RAISE_ERROR;
	}
	ASSERT(count==2);

	count=0;
	TRY {
		RAISE_ERROR;
		ON_ERROR{
			ASSERT(0);
		};
		ON_ERROR{
			ASSERT(0);
		}
	}
	ASSERT(count==0);


	count=0;
	TRY {
		ON_ERROR{
			count++;			
		};
		RAISE_ERROR;
		ON_ERROR{
			ASSERT(0);
		}
	}
	ASSERT(count==1);

}


static void __foo2() {
	RAISE_ERROR;
}
static void __foo(int* c) {
	TRY {
		FINALLY(e) {
			(*c)++;
		}
		__foo2();
		ASSERT(0);
	}
}
static int __bar() {
	int count=0;
	ASSERT(unwind_context!=NULL);
	TRY {
		ON_ERROR{
			ASSERT(count==1);
			count++;
		}
		__foo(&count);
	}
	ASSERT(count==2);
	return count;
}

BARE_TEST(test_exc_unwind,
	"Test that exceptions will unwind the stack"
	)
{
	TRY {
		ON_ERROR{
			ASSERT(0);
		}		
		ASSERT(__bar()==2);
	}
	ASSERT(unwind_context==NULL);
}


void report_time(struct timespec* t1, struct timespec* t2, int n)
{
	double T1 = t1->tv_nsec + t1->tv_sec*1E9;
	double T2 = t2->tv_nsec + t2->tv_sec*1E9;
	MSG("Performance: %f nsec\n", (T2-T1)/n);
}

void compute_func(int m)
{
	RAISE_ERROR;
}
BARE_TEST(test_exc_inloop1, 
	"Test the performance of exceptions in a loop.\n" 
	)
{
	long long int sum = 0l, sum2=0l;
	const long long int n = 10000000;
	struct timespec tbegin, tend;
	clock_gettime(CLOCK_REALTIME, &tbegin);
	for(int i=1;i<=n;i++) {
		TRY {
			FINALLY(e) {
				sum += i;
			}

			__atomic_signal_fence(__ATOMIC_ACQUIRE);
			sum2+=i;

			compute_func(i);
		}
	}
	clock_gettime(CLOCK_REALTIME, &tend);
	MSG("Timing the cost of setting up a TRY block and raising an exception\n");
	report_time(&tbegin, &tend, n);
#if 0
	MSG("sum=%lld,  sum2=%lld, expected %lld\n",sum,sum2, n*(n+1)/2ll);
#endif
	ASSERT(sum== n*(n+1)/2ll );
	ASSERT(sum2== n*(n+1)/2ll );
}

int compute_func2(int m)
{
	return(m*3);
}

BARE_TEST(test_exc_inloop2, "Test the performance of exceptions in a loop, non throwing")
{
	long long int sum = 0l, sum2=0l;
	const int n = 10000000;
	struct timespec tbegin, tend;
	clock_gettime(CLOCK_REALTIME, &tbegin);
	for(int i=1;i<=n;i++) {
		TRY {
			sum2+=3*i;
			sum += compute_func2(i);
		}
	}
	clock_gettime(CLOCK_REALTIME, &tend);
	MSG("Timing the cost of setting up a TRY block, without raising an exception\n");
	report_time(&tbegin, &tend, n);
	ASSERT(sum==sum2);
	ASSERT(sum== 3ll*n*(n+1)/2ll );
}


BARE_TEST(test_exc_inloop3, "Test the performance a loop without exceptions, for comparison with previous tests")
{ 
	/* Note: these are volatile to disable some very aggressive optimizations */
	volatile long long int sum = 0l, sum2=0l; 
	const int n = 10000000; 
	struct timespec tbegin, tend;
	clock_gettime(CLOCK_REALTIME, &tbegin);
	for(int i=1;i<=n;i++) {
			sum2+=3*i;
			sum += compute_func2(i);
	}
	clock_gettime(CLOCK_REALTIME, &tend);
	MSG("Timing the cost of the loop without exceptions\n");
	report_time(&tbegin, &tend, n);
	ASSERT(sum==sum2);
	ASSERT(sum== 3ll*n*(n+1)/2ll );
}



TEST_SUITE(exception_tests,
	"Tests for exceptions.")
{
	&test_exc_empty_body,
	&test_exc_catcher_match,
	&test_exc_unwind,
	&test_exc_inloop1,
	&test_exc_inloop2,
	&test_exc_inloop3,
	NULL
};




TEST_SUITE(all_tests,
	"All tests")
{
	&rlist_tests,
	&rheap_tests,	
	&rtree_tests,	
	&rdict_tests,
	&test_pack_unpack,
	&exception_tests,	
	NULL
};


int main(int argc, char** argv)
{
	return register_test(&all_tests) || 
		run_program(argc, argv, &all_tests);
}