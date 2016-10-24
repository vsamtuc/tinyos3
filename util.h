
#ifndef UTIL_H
#define UTIL_H

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>
#include <assert.h>

/**
	@file util.h

	@brief Tinyos utility code.

	This file defines the following:
	- macros for error checking and message reporting
	- a _resource list_ data structure

	Resource list
	--------------

*/


/**
	@defgroup check_macros Macros for checking system calls.

	@brief Macros for checking system calls and reporting errors.

	With regard to reporting an error, there are, generally speaking, two kinds of functions in
	the Unix API:
	-# Functions that return >=0 on success and -1 on error, storing the error code
	  in the global variable @c errno, and
	-# Functions which return an error code directly.

	The macros in this section help with checking the error status of such calls,
	when an error should abort the program. Their use can be seen in bios.c

	@{
 */

/**
	@brief Print a message to stderr and abort.
 */
#define FATAL(msg) { fprintf(stderr, "FATAL %s(%d):%s: %s\n", \
__FILE__, __LINE__, __FUNCTION__, (msg)); abort(); }

/**
	@brief Print an error message to stderr and abort.

	This macro will print a readable message (using @c FATAL) 
	corresponding to a unix error code, usually stored in 
	@c errno, or returned from a function.

	@param errcode a unix error code
	@see FATAL
 */
#define FATALERR(errcode) FATAL(strerror(errcode))

/**
	@brief Wrap a unix call to check for errors.

	This macro can wrap a call whose (integer) return value 
	is 0 for success or an error code for error.

	For example:
	@code
	CHECKRC(pthread_mutex_init(&mutex, NULL));
	@endcode
	@see CHECK
*/
#define CHECKRC(cmd) { int rc = (cmd); if(rc) FATALERR(rc); }

/**
	@brief Wrap a unix call to check for errors.

	This macro can wrap a call whose (integer) return value 
	is 0 for success or -1 for failure, with an error code stored
	in errno.

	For example:
	@code
	CHECK(gettimeofday(t, NULL));
	@endcode
	@see CHECK
*/	
#define CHECK(cmd)  { if((cmd)==-1) FATALERR(errno); }

/**
	@brief Check a condition and abort if it fails.

	This macro will check a boolean condition and 
	abort with a message if it does not hold. It is used to
	check parameters of functions.

	This is similar to @c assert(...), but will not be turned off.
*/
#define CHECK_CONDITION(expr) { if(!(expr)) FATAL("Failed constraint: " # expr) }


/**
	@brief A wrapper for malloc checking for out-of-memory.

	If there is no memory to fulfill a request, FATAL is used to
	print an error message and abort.

	@param size the number of bytes allocated via malloc
	@returns the new memory block
  */
static inline void * xmalloc (size_t size)
{
  void *value = malloc (size);
  if (value == 0)
    FATAL("virtual memory exhausted");
  return value;
}


/** @}   check_macros  */


/*******************************************************
 *
 *
 *******************************************************/

/**
	@defgroup rlists  Resource lists
	@brief  A simple and fast list implementation.


	Overview
	--------

	This data structure is a doubly-linked circular list, whose implementation
	is based on the splicing operation. 

	In a circular list, the nodes form a ring. For example if a, b, and c
	are nodes, a ring may look like
	@verbatim
	+--> a --> b --> c -->+
	|                     |
	+<------<--------<----+
	@endverbatim
	where only the @c next pointer is drawn. The @c prev pointer of a node
	is always the opposite of @c next, i.e.,  @c p->next->prev==p->prev->next==p.
	In the following, we shall denote such a ring by [a,b,c]. Note that
	[b,c,a] and [c,a,b] are describing the same ring as [a,b,c]. A singleton ring
	has just one node, e.g., [a].

	The splicing operation between two rlnodes a and b means simply to
	swap their @c next pointers (also adjusting the 'prev' pointers appropriately).
	Splicing two nodes on different rings, joins the two rings. Splicing two
	nodes on the same ring, splits the ring. 
	For example, @c splice(a,c) on ring
	[a,b,c,d] would create two rings [a,d] and [b,c]. 
	A splice can be reversed by repeating it; continuing the previous example, 
	given rings [a,d] and [b,c], splice(a,c) will create ring [a,b,c,d] again.
	The precise definition of splice
	is the following:
	@code
	rlnode* splice(rlnode* a, rlnode* b) {
		swap(& a->next->prev, & b->next->prev);
		swap(& a->next, & b->next);
		return b;
	}
	@endcode
	In general, @c splice(a,b) applies the following transformation
	@verbatim
	[a, x...]  [b, y...]   ==>   [a, y..., b, x...]
	[a, x..., b, y...]     ==>   [a, y...]  [b, x...]
	@endverbatim

	To implement lists, an rlnode object is used 
	as _sentinel_,  that is, it holds no data and is not properly part of the 
	list. If L is the list node, then ring  [C, L, A, B]  represents the list {A,B,C}.
	The empty list is represented as [L].

	We now show some examples of list operations, implemented by splicing.
	Suppose that L is a pointer to the sentinel node of a list.
	Also, suppose that N is (pointer to) a node in a singleton ring [N]
	Then, the the following operation are implemented as shown (in pseudocode):
	@verbatim
	empty(L)              ::  return  L == L->next
	head(L)               ::  return  L->next
	tail(L)               ::  return  L->prev
	push_front(L, N)      ::  splice(L, N)
	push_back(L, N)       ::  splice(L->prev, N)
	pop_front(L)          ::  return splice(L, L->next)
	pop_back(L)           ::  return splice(L, L->prev) 
	remove(N)             ::  return splice(N->prev, N)
	insert_after(P, N)    ::  splice(P, N)
	insert_before(P, N)   ::  splice(P->prev, N)
	@endverbatim

	These operations can be used to perform other operations. For example,
	if L1 and L2 are two lists, then we can append the nodes of L2 to L1
	(leaving L2 empty), by the following two operations:
	@verbatim
	push_back(L1, L2);
	remove(L2);
	@endverbatim

	For more details on the implementation, please read the code of @ref util.h.

	Usage
	-----

	Resource lists are mostly useful as storage for lists of resources. The main type
	is the list node, type @c rlnode. Each @c rlnode object must be initialized before use,
	by calling either @c rlnode_init or @c rlnode_new.
	@code
	TCB* mytcb =...;
	FCB* myfcb =...;

	rlnode n1, n2;

	// The following four lines are equivalent 
	rlnode_init(& n1, mytcb); 
	rlnode_new(& n1)->tcb = mytcb;
	rlnode_init(& n1, NULL);  n1->tcb = mytcb;
	rlnode_new(& n1);  n1->tcb = mytcb;


	n1->fcb = myfcb;
	myfcb = n1->fcb;
	@endcode


	###  Creating lists

	A list is defined by a sentinel node. For example,
	@code
	rlnode mylist;  
	rlnode_new(&mylist);
	@endcode
	Note that, although we did not store a value into the sentinel node, we actually 
	could do so if desired.
	
	Once a list is created, it needs to be filled with data.
	There are routines for adding nodes to the head and tail of a list, or in an intermediate
	location. Also, lists can be compared for equality, have their length taken, checked for
	emptiness, etc.
	@see rlist_push_front
	@see rlist_push_back

	### Intrusive lists

	In order to add nodes to a list, we must allocate @c rlnode objects somewhere in memory.
	It is absolutely legal to use `malloc()` for this purpose, but we must add code to free
	the allocated memory, which can be annoying.

	If we wish to store objects of a particular kind however, we can use a different technique:
	we can store an rlnode pointer inside the object itself. A list built by this trick is called
	an *intrusive list*.

	For example, suppose we want to
	maintain a list of TCBs with high priority.
	@code
	rlnode hi_pri_list;  rlnode_new(&hi_pri_list);

	struct thread_control_block {
	 .... // other stuff
	 rlnode hi_pri_node;
	};

	// initialize the node
	TCB* newtcb = ...;
	rlnode_init(& newtcb->hi_pri_node, newtcb);

	// then, we can just add the node to the list
	rlist_push_back(& hi_pri_list, & newtcb->hi_pri_node);
	@endcode

	Because node @c hi_pri_node is stored inside the object, it is always available. The node
	can be removed and re-added to this or another list, and memory allocation/deallocation
	is not an issue.
	The implementation of tinyos3 uses this idea very extensively, in TCB, PCB and FCB.

	@{
 */


typedef struct process_control_block PCB;	/**< @brief Forward declaration */
typedef struct thread_control_block TCB;	/**< @brief Forward declaration */
typedef struct core_control_block CCB;		/**< @brief Forward declaration */
typedef struct device_control_block DCB;	/**< @brief Forward declaration */
typedef struct file_control_block FCB;		/**< @brief Forward declaration */

/** @brief A convenience typedef */
typedef struct resource_list_node * rlnode_ptr;

/**
	@brief List node
*/
typedef struct resource_list_node {
  
  /** @brief The list node's key.
     
     The key (data element) of a list node is 
     stored in a union of several pointer and integer types.
     This allows for easy access, without the need for casting. 
     For example,
     \code
     TCB* tcb = mynode->tcb;
     \endcode
     */
  union {
    PCB* pcb; 
    TCB* tcb;
    CCB* ccb;
    DCB* dcb;
    FCB* fcb;
    void* obj;
    rlnode_ptr node;
    intptr_t num;
    uintptr_t unum;
  };

  /* list pointers */
  rlnode_ptr prev;  /**< @brief Pointer to previous node */
  rlnode_ptr next;	/**< @brief Pointer to next node */
} rlnode;


/**
	@brief Initialize a node as a singleton ring.

	This function will initialize the pointers of a node 
	to form a singleton ring. The node is returned, so that
	one can write code such as
	\code
	rlnode n;  rlnode_new(&n)->num = 3;
	\endcode
	@pre @c p!=NULL
	@param p the node to initialize into a singleton
	@returns the node itself
 */
static inline rlnode* rlnode_new(rlnode* p) 
{ 
	p->prev = p->next = p; 
	return p;
}

/**
	@brief Initialize a node as a singleton ring.

	This function will initialize the pointers of a node 
	to form a singleton ring, and store the . The node is returned, so that
	one can write code such as
	\code
	rlnode n;  rlist_push_front(&L, rlnode_init(&n, obj));
	\endcode

	@pre @c p!=NULL
	@param p the node to initialize into a singleton
	@param ptr the pointer to store as the node key
	@returns the node itself
 */
static inline rlnode* rlnode_init(rlnode* p, void* ptr)  
{
	rlnode_new(p)->obj = ptr; 
	return p;
}


/** 
	@brief Swap two pointers to rlnode.
*/
static inline void rlnode_swap(rlnode_ptr *p, rlnode_ptr *q) 
{
  rlnode *temp;
  temp = *p;  *p = *q; *q = temp;  
}

/**
	@brief Splice two rlnodes.

	The splice operation swaps the @c next pointers of the two nodes,
	adjusting the @c prev pointers appropriately.

	@param a the first node
	@param b the second node
	@returns the second node, @c b
*/
static inline rlnode* rl_splice(rlnode *a, rlnode *b)
{
  rlnode_swap( &(a->next->prev), &(b->next->prev) );
  rlnode_swap( &(a->next), & (b->next) );
  return b;
}

/**
	@brief Remove node from a ring and turn it into singleton.

	This function will remove @c a from the ring that contains it.
	If @c a is a singleton ring, this function has no effect.
	@param a the node to remove from a ring
	@returns the removed node
*/
static inline rlnode* rlist_remove(rlnode* a) { rl_splice(a, a->prev); return a; }

/** @brief  Check a list for emptiness.

	@param a the list to check
	@returns true if the list is empty, else 0.
 */
static inline int is_rlist_empty(rlnode* a) { return a==a->next; }

/**
	@brief insert at the head of a list.

	Assuming that @c node is not in the ring of @c list, 
	this function inserts the ring  of @c node (often a singleton) 
	to the head of @c list.

	This function is equivalent to @c splice(list,node). 
  */
static inline void rlist_push_front(rlnode* list, rlnode* node) { rl_splice(list, node); }

/**
	@brief insert at the tail of a list.

	Assuming that @c node is not in the ring of @c list, 
	this function inserts the ring  of @c node (often a singleton) 
	to the head of @c list.

	This function is equivalent to @c splice(list->prev,node). 
  */
static inline void rlist_push_back(rlnode* list, rlnode* node) { rl_splice(list->prev, node); }

/**
	@brief Remove and return the head of the list.

	This function, applied on a non-empty list, will remove the head of 
	the list and return in.
*/
static inline rlnode* rlist_pop_front(rlnode* list) { return rl_splice(list, list->next); }

/**
	@brief Remove and return the tail of the list.

	This function, applied on a non-empty list, will remove the tail of 
	the list and return in.
*/
static inline rlnode* rlist_pop_back(rlnode* list) { return rl_splice(list, list->prev); }

/**
	@brief Return the length of a list.

	This function returns the length of a list.
	@note the cost of this operation is @f$ O(n) @f$  
*/
static inline size_t rlist_len(rlnode* list) 
{
	unsigned int count = 0;
	rlnode* p = list->next;
	while(p!=list) {
		p = p->next;
		count++;
	}
	return count;
}

/**
	@brief Check two lists for equality.

	@param L1 the first list
	@param L2 the second list
	@returns true iff two lists are equal, else false. 
 */
static inline int rlist_equal(rlnode* L1, rlnode* L2)
{
	rlnode *i1 = L1->next;
	rlnode *i2 = L2->next;

	while(i1!=L1) {
		if(i2==L2 || i1->num != i2->num)
			return 0;
		i1 = i1->next; 
		i2 = i2->next;
	}

	return i2==L2;
}

/**
	@brief Append the nodes of a list to another.

	After the append, @c lsrc becomes empty. The operation is
	@verbatim
	[ldest, X...] [lsrc, Y...]  ==> [ldest, X..., Y...]  [lsrc]
	@endverbatim
*/
static inline void rlist_append(rlnode* ldest, rlnode* lsrc)
{
	rlist_push_back(ldest, lsrc);
	rlist_remove(lsrc);
}

/**
	@brief Prepend the nodes of a list to another.

	After the append, @c lsrc becomes empty. The operation is
	@verbatim
	[ldest, X...] [lsrc, Y...]  ==> [ldest, Y..., X...]  [lsrc]
	@endverbatim
*/
static inline void rlist_prepend(rlnode* ldest, rlnode* lsrc)
{
	rlist_push_front(ldest, lsrc);
	rlist_remove(lsrc);
}

/**
	@brief Reverse a ring or list.

	This function will reverse the direction of a ring. 
  */
static inline void rlist_reverse(rlnode* l)
{
	rlnode *p = l;

	do {
		rlnode_swap(& p->prev, & p->next);
		p = p->next;
	} while(p != l);
}

/**
	@brief Find a node by key.

	Search and return the first node whose key is equal to a
	given key, else return a given node (which may be NULL).

	@param List the list to search
	@param key the key to search for in the list
	@param fail the node pointer to return on failure
  */
static inline rlnode* rlist_find(rlnode* List, void* key, rlnode* fail)
{
	rlnode* i= List->next;
	while(i!=List) {
		if(i->obj == key)
			return i;
		else
			i = i->next;
	}
	return fail;
}

/**
	@brief Move nodes 

	Append all nodes of Lsrc which satisfy pred (that is, pred(...) returns non-zero)
	to the end of Ldest.
*/
static inline void rlist_select(rlnode* Lsrc, rlnode* Ldest, int (*pred)(rlnode*))
{
	rlnode* I = Lsrc;
	while(I->next != Lsrc) {
		if(pred(I->next)) {
			rlnode* p = rlist_remove(I->next);
			rlist_push_back(Ldest, p);
		} else {
			I = I->next;
		}
	}
}


/* @} rlists */



/*
	Some helpers for packing and unpacking vectors of strings into
	(argl, args)
*/

/**
	@brief Return the total length of a string array.

	@param argc the length of the string array
	@param argv the string array
	@returns the total number of bytes in all the strings, including the
	    terminating zeros.
*/
static inline size_t argvlen(size_t argc, const char** argv)
{
	size_t l=0;
	for(size_t i=0; i<argc; i++) {
		l+= strlen(argv[i])+1;
	}	
	return l;
}


/**
	@brief Pack a string array into an argument buffer.

	Pack a string array into an argument buffer, which must
	be at least @c argvlen(argc,argv) bytes big.

	@param args the output argument buffer, which must be large enough
	@param argc the length of the input string array
	@param argv the input string array
	@returns the length of the output argument buffer
	@see argvlen
*/
static inline size_t argvpack(void* args, size_t argc, const char** argv)
{
	int argl=0;

	char* pos = args;
	for(size_t i=0; i<argc; i++) {
		const char *s = argv[i];
		while(( *pos++ = *s++ )) argl++;
	}
	return argl+argc;
}

/**
	@brief Return the number of strings packed in an argument buffer.

	This is equal to the number of zero bytes in the buffer.

	@param argl the length of the argument buffer
	@param args the argument buffer
	@returns the number of strings packed in @c args
*/
static inline size_t argscount(int argl, void* args)
{
	int n=0;
	char* a = args;

	for(int i=0; i<argl; i++)
		if(a[i]=='\0') n++;
	return n;	
}

/**
	@brief Unpack a string array from an argument buffer.

	The string array's length must be less than or equal to
	the number of zero bytes in @c args.

	@param argc the length of the output string array
	@param argv the output string array
	@param argl the length of the input argument buffer
	@param args the input argument buffer
	@returns the first location not unpacked

	@see argscount
*/
static inline void* argvunpack(size_t argc, const char** argv, int argl, void* args)
{
	char* a = args;
	for(int i=0;i<argc;i++) {
		argv[i] = a;
		while(*a++); /* skip non-0 */
	}
	return a;
}



/**
	@defgroup exceptions  An execption-like library.

	@brief An exception-like mechanism for C

	Exceptions are supported by many object-oriented languages,
	such as Java, C++ and Python, but not by C. This makes programming
	certain kinds of tasks somewhat complicated. These are
	tasks that can cause a thread to exit back through several layers of
	calls.  For example, a system call may lead to a stack of nested
	calls to execute the code of a driver. If processing is to be
	rolled back to the point of entry to the kernel, all calls in that
	stack need to propagate the error. This makes coding tedious and
	error-prone.

	In C, there is a standard-library facility that can be used to 
	implement such functionality, available by including `<setjmp.h>`.
	The help is in the form of functions @c setjmp() and @c longjmp 
	(and their vatiatons). In this 	library, these standard calls, 
	wrapped by some suitable macros, and using some GNU GCC extensions
	(nested functions),	provide some easy-to-use exception-like programming 
	structures.

	## Examples

	Before we describe the details, we show some examples of the library's
	use.

	A try-block is declared as follows:
	@code
	TRY_WITH(context) {

		ON_ERROR {
			printf("Error in what I was doing\n");
			// After this, execute the finally 
		}

		FINALLY(e) {
			if(e) 
				printf("Continuing after error\n");			
			else
				printf("Finished without error\n");
		}

		// do something here 
		if(error_happens)
			raise_exception(context);

		// or call a function that may call raise_exception()
		do_something_else();

		// If we leave here, FINALLY will be executed 
	}
	@endcode

	For example, one could do the following, to construct a
	composite resource:
	@code
	Resource r1, r2, r3;
	TRY_WITH(context) {
		lock_monitor();
		FINALLY(e) {
			unlock_monitor();
		}

		// This may raise_exception(...)
		r1 = acquire_resource1();
		ON_ERROR {
			release_resource1(r1);
		}

		// This may raise_exception(...)
		r2 = acquire_resource2(r2);

		ON_ERROR {
			release_resource2(r2);
		}
		
		// This may raise_exception(...)
		r3 = acquire_resrouce3(r1, r2);
	}
	return r3;
	@endcode

	## How it works

	The workings are based on the idea of an **exception stack**. The elements
	of this stack are called __exception stack frames__ (ESFs). Each thread should
	have its own exception stack. 
	When a TRY_WITH(...) block begins, a new ESF is pushed to the stack, and the
	block starts to execute. 
	
	Each ESF has two lists of functions of type @ exception_handler, which is defined 
	as  `void (*)(int)`. The nodes for these lists are `struct exception_handler_frame`
	objects.  Initially, the new ESF has empty lists.  The first list is the list
	of **catchers** and the second is the list of **finalizers**.

	As the TRY-block executes, execution reaches `FINALLY()` and `ON_ERROR` blocks.
	When a `FINALLY()` block is reached, a new handler is added to the finalizers list.
	When a `ON_ERROR` block is reached, a new handler is added to the catchers list.

	If execution arrives at the end of the TRY-block, the list of catchers is thrown
	away and the finalizers are executed (in reverse order, that is, last-in-first-out).

	If at some point the function `raise_exception()` is called, execution jumps
	back at the TRY-block at the top of the exception stack. There, each catcher is
	first executed, followed by all the finalizers. At the end, the ESF is popped 
	from the exception stack. Then, if at least one catcher 
	did execute, the exception is considered handled, and execution continues after
	the TRY-block. If however there was no catcher executed, and the exception stack
	is non-empty, then `raise_exception()` is called again, to repeat the process.

	An exception stack is defined simply as a pointer to `struct exception_stack_frame`.
	An __exception context__is a pointer to such a pointer, that is,
	@code
	typedef struct exception_stack_frame** exception_context;
	@endcode
	A context needs to be available to our code in two places: when a 
	`TRY_WITH(context)` block is defined, and when `raise_exception(context)` is
	called.
	
	One can simply define a context as a global variable:
	@code
	// at top level
	struct execution_stack_frame* exception_stack = NULL;
	#define TRY  TRY_WITH(&exception_stack)
	#define RAISE  raise_exception(&exception_stack)

	TRY {
		...
		RAISE;
		...
	}
	@endcode

	In a multi-threaded case, it is necessary to declare one context for each
	thread. This can be done at the TCB, for example.

	@code
	struct TCB {
		....
		struct execution_stack_frame* exception_stack = NULL;	
	}

	#define TRY  TRY_WITH(& CURTHREAD->exception_stack)
	#define RAISE  raise_exception(& CURTHREAD->exception_stack)
	@endcode

	## Performance

	Although setting up a TRY-block is relatively cheap (basically, a call to
	`setjmp` is done), it is better to avoid calling exception handlers.
	So, for very critical pieces of code, one could do
	@code
	TRY {
		lock_mutex();
		ON_ERROR {
			unlock_mutex();
		}

		... // stuff
	
		unlock_mutex();
	}
	@endcode
	instead of the more convenient
	@code
	TRY {
		lock_mutex();
		FINALLY(e) {
			unlock_mutex();
		}

		... // stuff	
	}
	@endcode

	The first case is faster, because it avoids a function call, when there is
	no exception, whereas the second will make a call to the `FINALLY` block, even
	without an exception raised. But, remember: __premature optimization is the
	source of all evil__.

	## Raising from inside an exception handler

	It is perfecly legal and supported to have exceptions raised from inside
	`ON_ERROR` or `FINALLY` blocks (or the functions they call).

	What happens in this case is that the exception handler execution is aborted
	and the processing continues with the next exception handler of the ESF.

	@{
 */


typedef void (*exception_handler)(int);


struct exception_handler_frame {
	exception_handler handler;
	struct exception_handler_frame* next;
};


struct exception_stack_frame
{
	struct exception_stack_frame * next;
	struct exception_handler_frame * catchers;
	struct exception_handler_frame * finalizers;
	jmp_buf jbuf;
};

typedef struct exception_stack_frame** exception_context;

void raise_exception(exception_context context);

void exception_unwind(exception_context context, int errcode);


/**
	@defgroup helpers Helpers for exceptions
	@brief  These are some internal helpers, not part of the public API.
	@internal
	@{
*/
static inline void __exc_push_frame(exception_context context, 
	struct exception_stack_frame* frame)
{
	frame->next = *context;
	*context = frame;
}

static inline struct exception_stack_frame* __exc_try(exception_context context, int errcode)
{
	if(errcode==0) 
		return *context;
	else {
		exception_unwind(context, errcode);
		return NULL;
	}
}

static inline struct exception_stack_frame* __exc_exit_try(exception_context context)
{
	(*context)->catchers = NULL;
	exception_unwind(context, 0);
	return NULL;
}

#define __concatenate_tokens(x,y) x ## y
#define __conc(z,w) __concatenate_tokens(z,w)

/** @} */

#define TRY_WITH(context) \
	struct exception_stack_frame __conc(__try_,__LINE__) = \
		{ .catchers=NULL, .finalizers=NULL };\
	__exc_push_frame((context), & __conc(__try_ , __LINE__) ); \
	int __conc(__exception_,__LINE__) = setjmp( __conc(__try_,__LINE__).jbuf); \
	__atomic_signal_fence(__ATOMIC_SEQ_CST);\
	for(struct exception_stack_frame* __frame = \
		__exc_try((context), __conc(__exception_, __LINE__) );\
		__frame != NULL ; \
		__frame = __exc_exit_try(context)) \

#define FINALLY(excname)  \
	struct exception_handler_frame __conc(__xframe_,__LINE__);\
	__conc(__xframe_, __LINE__).next = __frame->finalizers; \
	__frame->finalizers = & __conc(__xframe_, __LINE__) ;\
	auto void __conc(__action_, __LINE__) (int); \
	__conc(__xframe_,__LINE__).handler = __conc(__action_,__LINE__);\
	__atomic_signal_fence(__ATOMIC_SEQ_CST);\
	void __conc(__action_,__LINE__)(int excname) \


#define ON_ERROR  \
	struct exception_handler_frame __conc(__xframe_,__LINE__);\
	__conc(__xframe_, __LINE__).next = __frame->catchers; \
	__frame->catchers = & __conc(__xframe_, __LINE__) ;\
	auto void __conc(__action_, __LINE__) (int); \
	__conc(__xframe_,__LINE__).handler = __conc(__action_,__LINE__);\
	__atomic_signal_fence(__ATOMIC_SEQ_CST);\
	void __conc(__action_,__LINE__)(int __dummy) \


/** @}  exceptions */

#endif