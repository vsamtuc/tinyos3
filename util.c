
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

