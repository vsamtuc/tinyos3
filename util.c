
#include <stdarg.h>
#include "util.h"


/**********************
	Pairing heap
 **********************/


/* 
	Make heap2 the first child of heap1.
	Precondition: heap2->next == NULL
 */
rlnode* rheap_link(rlnode* heap1, rlnode* heap2);


/*
	Remove a node from the heap and take it into a heap of its own.
	Precondition: node is in the subtree of heap
	Precondition: heap != node
 */
void rheap_unlink(rlnode* node);


rlnode* rheap_init(rlnode* node) 
{
	/* Mark prev of node */
	node->prev = pointer_marked(node);
	node->next = NULL;
	return node;
}

/* h2 becomes the first child of h1 */
rlnode* rheap_link(rlnode* heap1, rlnode* heap2)
{
	assert(heap2->next == NULL);
	heap2->next = heap1->prev; 
	heap1->prev = heap2;
	return heap1;
}

size_t rheap_size(rlnode* heap)
{
	if(heap==NULL) return 0;
	size_t c = 1;
	for(rlnode* p=heap->prev; ! pointer_is_marked(p); p=p->next)
		c += rheap_size(p);
	return c;
}



void __rheap_unlink(rlnode* node, rlnode* parent)
{	
	assert(node != NULL);
	assert(parent != NULL);
	/* pointer-hop until we find pointer pointing to node */
	rlnode** ptr = &parent->prev;
	assert(! pointer_is_marked(*ptr));
	while( (*ptr) != node ) {
		ptr = & (*ptr)->next;
		assert(! pointer_is_marked(*ptr));
	}
	/* redirect pointer to node->next */
	*ptr = node->next;
	/* mark node as unlinked */
	node->next = NULL;
}

void rheap_unlink(rlnode* node)
{
	rlnode* parent = rheap_parent(node);
	__rheap_unlink(node, parent);
}


rlnode* rheap_meld(rlnode* heap1, rlnode* heap2, rlnode_less_func lessf)
{
	if(heap1 == NULL)
		return heap2;
	else if(heap2 == NULL)
		return heap1;

	assert(heap1!=NULL && heap2!=NULL);
	if(lessf(heap1, heap2)) 
		return rheap_link(heap1, heap2);
	else
		return rheap_link(heap2, heap1);
}


static inline rlnode* __rheap_merge_pairs(rlnode* hlist, rlnode_less_func lessf)
{
	if(hlist==NULL || hlist->next==NULL) return hlist;
	/* two or more nodes in the list ... */
	rlnode* h0 = hlist;
	rlnode* h1 = hlist->next;
	rlnode* hlist2 = h1->next;
	h0->next = h1->next = NULL;
	return rheap_meld( 
			rheap_meld(h0,h1, lessf),
			__rheap_merge_pairs(hlist2, lessf),
			lessf
		);
}

rlnode* rheap_delmin(rlnode* heap, rlnode_less_func lessf)
{
	assert(heap != NULL);

	/* 
		First, we need to make the list of children of heap a proper singly-linked list.
		To do this, we pointer-hop until we locate a marked pointer, and set it to NULL.
	 */
	rlnode **p;
	for(p = &heap->prev;  ! pointer_is_marked(*p); p=&(*p)->next) {};
	rlnode* heapm = *p;
	assert(pointer_unmarked(heapm)==heap);

	/* Make the child list NULL-terminated */
	*p = NULL;  

	/* Save it, this list will be pair-merged */
	rlnode* hlist = heap->prev;

	/* Reset heap node to a legal heap without any children */
	heap->prev = heapm;

	/* This is the most critical step (performance-wise). */
	return __rheap_merge_pairs(hlist, lessf);
}


rlnode* rheap_delete(rlnode* heap, rlnode* node, rlnode_less_func lessf)
{
	/* Base case: node==heap */
	if(node == heap) return rheap_delmin(heap, lessf);

	/* Unlink the node from the heap */
	rheap_unlink(node);

	/* Unlink the node from its children */
	rlnode* nh = rheap_delmin(node, lessf);

	/* Meld children and rest of heap */
	return rheap_meld(heap, nh, lessf);
}


rlnode* rheap_decrease(rlnode* heap, rlnode* node, rlnode_less_func lessf)
{
	if(node == heap) return heap;

	rlnode* parent = rheap_parent(node);
	if( !lessf(node, parent) ) 	return heap;

	/* Sorry, we must do some more work... */
	__rheap_unlink(node, parent);
	return rheap_meld(node, heap, lessf);	
}


rlnode* rheap_from_ring(rlnode* ring, rlnode_less_func lessf)
{
	/* First, things first... */
	if(ring == NULL) return NULL;
	if(ring == ring->next) return rheap_init(ring);

	/* We do this by hand... */
	ring->prev->next = NULL;
	for(rlnode* p = ring; p!=NULL; p=p->next) 
		p->prev = pointer_marked(p);

	return __rheap_merge_pairs(ring, lessf);
}


void __rheap_add_to_list(rlnode* heap, rlnode* L)
{
	rlnode* hmark = pointer_marked(heap);
	while(heap->prev != hmark) {
		rlnode* child = heap->prev;
		heap->prev = child->next;
		__rheap_add_to_list(child, L);
	}
	rlist_push_back(L, rlnode_new(heap));
}


rlnode* rheap_to_ring(rlnode* heap)
{
	if(heap==NULL) return NULL;

	rlnode L;  
	rlnode_new(&L);
	__rheap_add_to_list(heap, &L);

	assert(!is_rlist_empty(&L));
	return rl_splice(&L, L.prev);
}





/************************************************************************
	rtree implementation
	---------------------

	We use rlnodes, with prev<->left and next<->right.
	We implement the red-black algorithm, as described in
	[Sedgewick, 2008, 
      http://citeseerx.ist.psu.edu/viewdoc/summary?doi=10.1.1.139.282]

    We treat the tree as a 2-3 tree of \c rlnode, with a marking on the
    \c next pointer to store the color.

****************************************************************************/


static inline int  _red(rlnode* n) { return n && pointer_is_marked(n->next); }
static inline void _set_red(rlnode* n) {n->next =  pointer_marked(n->next); }
static inline void _set_black(rlnode* n) {n->next =  pointer_unmarked(n->next); }
static inline void _set_flip_color(rlnode* n) { n->next =  pointer_mark_flipped(n->next); }
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
static inline void _color_flip(rlnode* h)
{
	_set_flip_color(h);
	_set_flip_color(_left(h));
	_set_flip_color(_right(h));
}

static inline rlnode* _fix_up(rlnode* h)
{
	if(_red(_right(h)) && !_red(_left(h))) h = _rotate_left(h);
	if(_red(_left(h)) && _red(_left(_left(h)))) h = _rotate_right(h);
	if(_red(_left(h)) && _red(_right(h)) ) _color_flip(h);
	return h;	
}


static inline rlnode* _move_red_left(rlnode* h)
{
	_color_flip(h);
	if(_red(_left(_right(h)))) {
		_set_right(h, _rotate_right(_right(h)));
		h = _rotate_left(h);
		_color_flip(h);
	}
	return h;
}

static inline rlnode* _move_red_right(rlnode* h)
{
	_color_flip(h);
	if(_red(_left(_left(h)))) {
		h = _rotate_right(h);
		_color_flip(h);
	}
	return h;	
}


void rtree_init(rlnode* node, rlnode_key key)
{
	node->key = key;
	node->prev = node->next = NULL;
	_set_red(node);
}


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

		int cmp = cmpf(node->key, h->key);
		if(cmp<0)  _set_left(h, insert(_left(h)));
		else       _set_right(h, insert(_right(h)));

		return _fix_up(h);
	}

	tree = insert(tree);
	_set_black(tree);
	return tree;
}


rlnode* rtree_delete(rlnode* tree, rlnode* node, rtree_cmp cmpf)
{
	/* This will point to the deleted node */
	rlnode* deleted = NULL;

	/* Delete the minimum node in subtree h and let 'deleted' point to it
	   Precondition: h != NULL */
	rlnode* delete_min(rlnode* h)
	{
		if(_left(h)==NULL) { deleted = h; return NULL; }

		if(!_red(_left(h)) && !_red(_left(_left(h))))
			h = _move_red_left(h);

		_set_left(h, delete_min(_left(h)));

		return _fix_up(h);
	}


	rlnode* delete(rlnode* h)
	{
		if(cmpf(node->key, h->key)<0) {
			if( !_red(_left(h)) && !_red(_left(_left(h)))  ) 
				h = _move_red_left(h);
			_set_left(h, delete(_left(h)));
		} else {
			if(_red(_left(h))) h = _rotate_right(h);
			if(cmpf(node->key, h->key)==0 && _right(h)==NULL) {
				deleted=h; return NULL;
			}
			if( !_red(_right(h)) && !_red(_left(_right(h)))  )
				h = _move_red_right(h);

			if(cmpf(node->key, h->key)==0) {
				_set_right(h, delete_min(_right(h)) );
				/* Replace h with deleted, delete h */
				deleted->prev = h->prev;
				deleted->next = h->next;
				rlnode* tmp = h;
				h = deleted;
				deleted = tmp;
			} else {
				_set_right(h, delete(_right(h)));				
			}

		}
		return _fix_up(h);
	}


	tree = delete(tree);
	if(deleted) { 
		assert(deleted==node);
		deleted->prev = deleted->next = NULL; _set_red(deleted); 
	}
	if(tree) _set_black(tree);
	return tree;
}






/************************************************************************
	rdict implementation
	---------------------

	In the following let B stand for dict.bucketno and S for dict.size

	The bucket array is  B+1 elements long.

	The first B elements are initialized as sentinels via pointer marking,
	with a value pointing to themselves.

	The last bucket is initialized to NULL.

	When iterating over a bucket, chasing the next pointer in rlnodes,
	if we come upon a marked pointer, we know that it is the location of the 
	bucket.


 **********************************************************************/


/* Return an iterator designating a bucket */
static inline rdict_iterator __rdict_bucket_begin(rdict* dict, hash_value h)
{
	return & dict->buckets[h % dict->bucketno];
}


/* Indicate if an iterator is at the end of a bucket. */
static inline int __rdict_bucket_end(rdict_iterator pos)
{
	return pointer_is_marked(*pos);
}


/* It at bucket end, move forward, else return as is */
static inline rdict_iterator __rdict_forward(rdict_iterator iter)
{
	while(__rdict_bucket_end(iter)) 
		iter = 1 + (rdict_bucket*)pointer_unmarked(*iter);
	return iter;
}


/* Pushing an element into the bucket list */
static inline rdict_iterator __rdict_iter_push(rdict_iterator pos, rlnode* elem)
{
	assert(*pos);  /* not END */
	elem->next = *pos;
	*pos = elem;
	return pos;
}

/* Pop an element via iterator */
static inline rlnode* __rdict_iter_pop(rdict_iterator pos)
{
	assert(*pos && !__rdict_bucket_end(pos)); /* not END, not bucket end */
	rlnode* elem = *pos;
	*pos = (*pos)->next;
	elem->next = elem;
	return elem;	
}


static inline rdict_iterator __rdict_bucket_find(rdict_iterator pos, hash_value hash, rlnode_key key, rdict_equal equalf)
{
	for( ; !__rdict_bucket_end(pos); pos = & (*pos)->next ) {
		if((*pos)->hash == hash && equalf(*pos, key)) break;
	}
	return pos;
}


static inline rdict_iterator __rdict_bucket_find_node(rdict_iterator pos, rlnode* node)
{
	for(; ! __rdict_bucket_end(pos); pos = &(*pos)->next)
		if(*pos == node) break;
	return pos;
}


/* Locate and remove an element in the bucket */
static inline rlnode* __rdict_bucket_remove(rdict_iterator pos, rlnode* elem)
{
	for(; ! __rdict_bucket_end(pos); pos = &(*pos)->next)
		if(*pos == elem) 
			return __rdict_iter_pop(pos);
	return NULL;
}


/*
	Initialize the bucket list with sentinel values.
 */
static inline void __rdict_allocate(rdict* dict, unsigned long buckno)
{
	dict->policy(RDICT_ALLOCATE, dict, (buckno+1)*sizeof(rdict_bucket));
	for(size_t i=0; i<buckno; i++) {
		dict->buckets[i] = pointer_marked(dict->buckets+i);
	}
	dict->buckets[buckno] = NULL; /* sentinel */
	dict->bucketno = buckno;
}


/*
	Remove every element without triggering a resize 
 */
static inline void __rdict_clear(rdict* dict)
{
	for(unsigned long i = 0; i < dict->bucketno; i++) {
		while(! pointer_is_marked(dict->buckets[i])) {
			rlnode* elem = dict->buckets[i];
			dict->buckets[i] = elem->next;
			elem->next = elem;
		}
	}
	dict->size = 0;
}

/*
	This is the action called by all size-changing operations.
 */
static inline void __rdict_size_changed(rdict* dict)
{
	va_list va;
	dict->policy(RDICT_SIZE_CHANGED, dict, va);
}


/* ====================================


	The public API of rdict
	
 ====================================== */


void rdict_initialize(rdict* dict, rdict_policy policy, rlnode_key pdata, unsigned long bucketno)
{
	dict->size = 0;
	dict->policy = policy;
	dict->policy_data = pdata;
	dict->bucketno = bucketno;
	dict->buckets = NULL;

	/* Init has allocated the buckets array */
	assert(bucketno>0);
	__rdict_allocate(dict, bucketno);
}


void rdict_destroy(rdict* dict)
{
	va_list ap;
	if(dict->buckets) {
		__rdict_clear(dict);
	}
	dict->policy(RDICT_DESTROY, dict, ap);
}



rdict_iterator rdict_begin(rdict* dict)
{
	return __rdict_forward(dict->buckets);
}


rdict_iterator rdict_next(rdict_iterator pos)
{
	assert(! __rdict_bucket_end(pos));
	return __rdict_forward(&(*pos)->next);
}


void rdict_resize(rdict* dict, unsigned long new_bucketno)
{ 
	if(new_bucketno==dict->bucketno) return;

	/* Push all nodes to a stack */
	rlnode* stack = NULL;
	for(unsigned long i = 0; i < dict->bucketno; i++) {
		rlnode* bucket = dict->buckets[i];
		while(! pointer_is_marked(bucket)) {
			rlnode* next = bucket->next;
			bucket->next = stack;
			stack = bucket;
			bucket = next;
		}
	}

	/* Allocate the new buckets */
	__rdict_allocate(dict, new_bucketno);

	/* Insert the nodes to the new buckets */
	while(stack) {
		rlnode* node = stack;
		stack = node->next;
		__rdict_iter_push(dict->buckets+(node->hash % dict->bucketno), node);
	}
}


void rdict_node_update(rlnode* elem, hash_value new_hash, rdict* dict)
{
	if(elem != elem->next) {
		/* size remains unchanged, avoid resizing the hash table */
		assert(dict != NULL);
		rlnode* found = __rdict_bucket_remove(__rdict_bucket_begin(dict, elem->hash), elem);
		assert(found == elem);
		if(found) __rdict_iter_push(__rdict_bucket_begin(dict, new_hash), found);
	}

	elem->hash = new_hash;
}


rdict_iterator rdict_find(rdict* dict, hash_value hash, rlnode_key key, rdict_equal equalf)
{
	rdict_iterator iter = __rdict_bucket_find(__rdict_bucket_begin(dict, hash), hash, key, equalf);
	return __rdict_bucket_end(iter)? rdict_end(dict) : iter;
}


rdict_iterator rdict_find_next(rdict* dict, rdict_iterator pos, hash_value hash, rlnode_key key, rdict_equal equalf)
{
	assert(! __rdict_bucket_end(pos));
	rdict_iterator iter = __rdict_bucket_find( & (*pos)->next, hash, key, equalf);
	return __rdict_bucket_end(iter)? rdict_end(dict) : iter;	
}


rdict_iterator rdict_find_node(rdict* dict, rlnode* node)
{
	for(rdict_iterator I = __rdict_bucket_begin(dict, node->hash);
		! __rdict_bucket_end(I);  I = &(*I)->next )
		if(*I == node)
			return I;
	return rdict_end(dict);
}


rdict_iterator rdict_insert(rdict* dict, rlnode* elem)
{
	rdict_iterator pos = __rdict_iter_push(__rdict_bucket_begin(dict, elem->hash), elem);
	dict->size++;
	__rdict_size_changed(dict);
	return pos;
}


rlnode* rdict_remove(rdict* dict, rlnode* elem)
{
	if(elem == elem->next) return NULL;
	elem = __rdict_bucket_remove(__rdict_bucket_begin(dict, elem->hash), elem);
	if(elem != NULL) {
		dict->size--;
		__rdict_size_changed(dict);
	}
	return elem;
}


rlnode* rdict_pop(rdict* dict, rdict_iterator pos)
{
	rlnode* node = __rdict_iter_pop(pos);
	dict->size --;
	__rdict_size_changed(dict);
	return node;
}



void rdict_apply(rdict* dict, void (*func)(rlnode*))
{
	for(rdict_iterator i=rdict_begin(dict); i!=rdict_end(dict); i = rdict_next(i))
		func(*i);	
}


void rdict_apply_removed(rdict* dict, void (*func)(rlnode*))
{
	unsigned long orig_size = dict->size;
	for(rdict_iterator i=rdict_begin(dict); i!=rdict_end(dict); i = __rdict_forward(i)) {
		rlnode* elem = __rdict_iter_pop(i);
		dict->size --;  /* reduce the size before calling func */
		func(elem);
	}
	assert(dict->size==0);
	if(orig_size!=0) __rdict_size_changed(dict);
}


/* ==========================================================


	Various policies policies 


   ========================================================== */


/*
	From the C++ libaries, a list of prime numbers suitable for sizing dictionaries
 */
#define NUM_DISTINCT_SIZES 62
static const size_t prime_hash_table_sizes[NUM_DISTINCT_SIZES+1] =
    {
      /* 0     */              5ul,
      /* 1     */              11ul, 
      /* 2     */              23ul, 
      /* 3     */              47ul, 
      /* 4     */              97ul, 
      /* 5     */              199ul, 
      /* 6     */              409ul, 
      /* 7     */              823ul, 
      /* 8     */              1741ul, 
      /* 9     */              3469ul, 
      /* 10    */              6949ul, 
      /* 11    */              14033ul, 
      /* 12    */              28411ul, 
      /* 13    */              57557ul, 
      /* 14    */              116731ul, 
      /* 15    */              236897ul,
      /* 16    */              480881ul, 
      /* 17    */              976369ul,
      /* 18    */              1982627ul, 
      /* 19    */              4026031ul,
      /* 20    */              8175383ul, 
      /* 21    */              16601593ul, 
      /* 22    */              33712729ul,
      /* 23    */              68460391ul, 
      /* 24    */              139022417ul, 
      /* 25    */              282312799ul, 
      /* 26    */              573292817ul, 
      /* 27    */              1164186217ul,
      /* 28    */              2364114217ul, 
      /* 29    */              4294967291ul,
      /* 30    */              8589934583ull,
      /* 31    */              17179869143ull,
      /* 32    */              34359738337ull,
      /* 33    */              68719476731ull,
      /* 34    */              137438953447ull,
      /* 35    */              274877906899ull,
      /* 36    */              549755813881ull,
      /* 37    */              1099511627689ull,
      /* 38    */              2199023255531ull,
      /* 39    */              4398046511093ull,
      /* 40    */              8796093022151ull,
      /* 41    */              17592186044399ull,
      /* 42    */              35184372088777ull,
      /* 43    */              70368744177643ull,
      /* 44    */              140737488355213ull,
      /* 45    */              281474976710597ull,
      /* 46    */              562949953421231ull, 
      /* 47    */              1125899906842597ull,
      /* 48    */              2251799813685119ull, 
      /* 49    */              4503599627370449ull,
      /* 50    */              9007199254740881ull, 
      /* 51    */              18014398509481951ull,
      /* 52    */              36028797018963913ull, 
      /* 53    */              72057594037927931ull,
      /* 54    */              144115188075855859ull,
      /* 55    */              288230376151711717ull,
      /* 56    */              576460752303423433ull,
      /* 57    */              1152921504606846883ull,
      /* 58    */              2305843009213693951ull,
      /* 59    */              4611686018427387847ull,
      /* 60    */              9223372036854775783ull,
      /* 61    */              18446744073709551557ull,
    	/* In case of overflow */
    							SIZE_MAX
    };


static int prime_size_index(size_t size)
{
	if(size < prime_hash_table_sizes[0]) return 0;
	if(size >= prime_hash_table_sizes[NUM_DISTINCT_SIZES-1]) return NUM_DISTINCT_SIZES;

	int low=0, high=NUM_DISTINCT_SIZES-1;

	while(high-low > 1) {
		int mid = (high+low) >> 1;
		if(prime_hash_table_sizes[mid] <= size) low = mid;
		else high = mid;
	}
	return high;
}


size_t rdict_next_greater_prime_size(size_t size, int shift)
{
	int ret = prime_size_index(size)+shift;
	if(ret<0) ret=0; else if(ret>NUM_DISTINCT_SIZES) ret=NUM_DISTINCT_SIZES;
	return prime_hash_table_sizes[ret];
}



/* ---------------------------------------------------------
	Default hashing policy
   --------------------------------------------------------- */


int rdict_default_policy(enum rdict_policy_op op, rdict* dict, ...)
{
	va_list ap;

	switch(op) {
	case RDICT_DESTROY:
		free(dict->buckets);
		dict->buckets = NULL;
		dict->bucketno = 0;
		return 0;

	case RDICT_SIZE_CHANGED:
		/* 
			The load factor is size/bucketno.
			If the load factor is more than 1, resize.
			If the load factor is less than 1/8, resize.
		 */
		if( (dict->size > dict->bucketno) || ( (dict->size << 3) < dict->bucketno ) ) {
			unsigned long newbucketno = rdict_next_greater_prime_size(dict->size, 0);
			rdict_resize(dict, newbucketno);
		}
		return 0;

	case RDICT_ALLOCATE:
		{
			va_start(ap, dict);
			size_t bytes = va_arg(ap, size_t);
			va_end(ap);

			dict->buckets = realloc(dict->buckets, bytes);
		}
		return 0;
	default:
		return -1;
	}
}




/* ---------------------------------------------------------
	... hashing policy
   --------------------------------------------------------- */
/* ---------------------------------------------------------
	... hashing policy
   --------------------------------------------------------- */
/* ---------------------------------------------------------
	... hashing policy
   --------------------------------------------------------- */

/**********************
	Packer
 **********************/

static void packer_ensure_space(packer* p, size_t isize)
{
	size_t newsize = p->size;
	if(newsize==0) newsize = 16;
	while(isize > newsize - p->pos) newsize <<=1;
	if(newsize!=p->size) { p->buffer = realloc(p->buffer, newsize); p->size=newsize; }
	assert(p->size >= p->pos + isize);
}

void packer_free(packer* p) { free(p->buffer); }


/* Two length functions */
static inline size_t  __len(const char* p) { return strlen(p)+1; }
static inline size_t  __nlen(const char* p, size_t n) {
	size_t nlen = strnlen(p, n);
	return nlen + (nlen<n) ? 1 : 0;
}

static inline void* __cur(packer* p) { return p->buffer+p->pos; }

void mempack(packer* p, const void* item, size_t isize)
{
	packer_ensure_space(p, isize);
	memcpy(p->buffer+p->pos, item, isize);
	p->pos += isize;
}

void strpack(packer* p, const char* str)
{
	mempack(p, str, __len(str));
}

void strnpack(packer* p, const char* str, size_t n)
{
	mempack(p, str, __nlen(str,n));
}


size_t memunpack(packer* p, void* loc, size_t isize) 
{
	memcpy(loc, p->buffer+p->pos, isize);
	p->pos += isize;
	return isize;
}

size_t strunpack(packer* p, char* loc)
{
	return memunpack(p, loc, __len(__cur(p)));
}

size_t strnunpack(packer* p, char* loc, size_t n)
{
	return memunpack(p, loc, __nlen(__cur(p),n));
}

void* memget(packer* p, size_t isize)
{
	void* ret = p->buffer+p->pos;
	p->pos += isize;
	return ret;
}

char* strget(packer* p)
{
	return memget(p, __len(__cur(p)));
}

char* strnget(packer* p, size_t n)
{
	return memget(p, __nlen(__cur(p),n));
}




/**********************
	Exceptions
 **********************/


void raise_exception(exception_context context)
{
	if(*context) {
		__atomic_signal_fence(__ATOMIC_SEQ_CST);
		longjmp((*context)->jbuf, 1);
	}
}


void exception_unwind(exception_context context, int errcode)
{
	/* Get the top frame */
	struct exception_stack_frame* frame = *context;

	/* handle exception */
	int captured = 0;

	/* First execute catchers one by one */
	while(frame->catchers) {
		captured = 1;
		struct exception_handler_frame *c = frame->catchers;
		/* Pop it from the list, just in case it throws() */
		frame->catchers = c->next;
		c->handler(errcode);
	}

	/* Execute finalizers one by one */
	while(frame->finalizers) {
		struct exception_handler_frame *fin = frame->finalizers;
		frame->finalizers = fin->next;

		fin->handler(errcode);
	}
 	
	/* pop this frame */
	*context = frame->next;

 	/* propagate */
	if(errcode && !captured) 
		raise_exception(context);
}

