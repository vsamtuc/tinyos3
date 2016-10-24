
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <setjmp.h>
#include "util.h"

#include "unit_testing.h"

#undef NDEBUG
#include <assert.h>


/* Unit tests for the resource lists */


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
	MSG("Performance: %f nsec/loop\n", (T2-T1)/n);
}

void compute_func(int m)
{
	RAISE_ERROR;
}
BARE_TEST(test_exc_inloop,
	"Test the performance of exceptions in a loop"
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
	report_time(&tbegin, &tend, n);
	MSG("sum=%lld,  sum2=%lld, expected %lld\n",sum,sum2, n*(n+1)/2ll);
	ASSERT(sum2== n*(n+1)/2ll );
}

int compute_func2(int m)
{
	return(m*3);
}

BARE_TEST(test_exc_inloop2,
	"Test the performance of exceptions in a loop, non throwing"
	)
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
	&test_exc_inloop,
	&test_exc_inloop2,
	NULL
};




TEST_SUITE(all_tests,
	"All tests")
{
	&rlist_tests,
	&test_pack_unpack,
	&exception_tests,	
	NULL
};


int main(int argc, char** argv)
{
	return register_test(&all_tests) || 
		run_program(argc, argv, &all_tests);
}