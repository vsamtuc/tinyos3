
#include <assert.h>
#include "kernel_cc.h"
#include "kernel_sys.h"
#include "kernel_proc.h"
#include "kernel_streams.h"

#ifndef NVALGRIND
#include <valgrind/valgrind.h>
#endif


/* 
 The process table and related system calls:
 - Vfork
 - Exec
 - Exit
 - WaitPid
 - GetPid
 - GetPPid

 */

/*==============================

  Process table 

 ============================== */

/* The process table is implemented as an array of PCB */
PCB PT[MAX_PROC];

/* Number of used pids */
unsigned int process_count;

/* wait channels */
static wait_channel wchan_wait_child = { SCHED_JOIN, "wait_child" };


PCB* get_pcb(Pid_t pid)
{
  if(pid<0 || pid>=MAX_PROC) return NULL;
  return PT[pid].pstate==FREE ? NULL : &PT[pid];
}

Pid_t get_pid(PCB* pcb)
{
  return pcb==NULL ? NOPROC : pcb-PT;
}

/* Initialize a PCB */
static inline void initialize_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->argl = 0;
  pcb->args = NULL;
  pcb->vfork_state = NULL;

  for(int i=0;i<MAX_FILEID;i++)
    pcb->FIDT[i] = NULL;

  rlnode_init(& pcb->children_list, NULL);
  rlnode_init(& pcb->exited_list, NULL);
  rlnode_init(& pcb->children_node, pcb);
  rlnode_init(& pcb->exited_node, pcb);
  wqueue_init(& pcb->child_exit, &wchan_wait_child);
}


static PCB* pcb_freelist;

void initialize_processes()
{
  /* initialize the PCBs */
  for(Pid_t p=0; p<MAX_PROC; p++) {
    initialize_PCB(&PT[p]);
  }

  /* use the parent field to build a free list */
  PCB* pcbiter;
  pcb_freelist = NULL;
  for(pcbiter = PT+MAX_PROC; pcbiter!=PT; ) {
    --pcbiter;
    pcbiter->parent = pcb_freelist;
    pcb_freelist = pcbiter;
  }

  process_count = 0;

  /* Execute a null "idle" process */
  if(sys_Spawn(NULL,0,NULL)!=0)
    FATAL("The scheduler process does not have pid==0");
}


/*
  Must be called with kernel_mutex held
*/
PCB* acquire_PCB()
{
  PCB* pcb = NULL;

  if(pcb_freelist != NULL) {
    pcb = pcb_freelist;
    pcb->pstate = ALIVE;
    pcb_freelist = pcb_freelist->parent;
    process_count++;
  }

  return pcb;
}

/*
  Must be called with kernel_mutex held
*/
void release_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->parent = pcb_freelist;
  pcb_freelist = pcb;
  process_count--;
}


Fid_t sys_OpenInfo()
{
	return NOFILE;
}


/*=============================================
 
 Program execution
 
 ============================================*/

/*
	This function is provided as an argument to Spawn,
	to execute the main thread of a process.
*/
static void start_main_thread()
{
	int exitval;

	Task call =  CURPROC->main_task;
	int argl = CURPROC->argl;
	void* args = CURPROC->args;

	exitval = call(argl,args);
	Exit(exitval);
}


/* 
	Create and wake up the thread for the main function. 
*/
void process_exec(PCB* proc, Task call, int argl, void* args)
{
	/* Set the main thread's function */
	proc->main_task = call;

	/* Copy the arguments to new storage, owned by the new process */
	proc->argl = argl;
	if(args!=NULL) {
		proc->args = malloc(argl);
		memcpy(proc->args, args, argl);
	}
	else
		proc->args=NULL;

	if(call != NULL) {
		proc->main_thread = spawn_thread(proc, start_main_thread);
		wakeup(proc->main_thread);
	}
}



void sys_Exec(Task call, int argl, void* args)
{

}



/*=============================================
 
 Process creation
 
 ============================================*/



void process_init_resources(PCB* newproc, PCB* parent)
{
	/* link to parent */
	newproc->parent = parent;

	/* Clear the kill flag */
	newproc->sigkill = 0;

	if(parent==NULL) return;

	/* link from parent */
	rlist_push_front(& parent->children_list, & newproc->children_node);

	/* Inherit file streams from parent */
	for(int i=0; i<MAX_FILEID; i++) {
		   newproc->FIDT[i] = parent->FIDT[i];
		   if(newproc->FIDT[i])
		      FCB_incref(newproc->FIDT[i]);
	}
}




/*
	System call to create a new process.
 */
Pid_t sys_Spawn(Task call, int argl, void* args)
{
	/* The new process PCB */
	PCB* newproc = acquire_PCB();

	if(newproc == NULL) {
		/* We have run out of PIDs! */
		set_errcode(EAGAIN);
		return NOPROC;
	}

	if(get_pid(newproc)<=1) 
		/* Processes with pid<=1 (the scheduler and the init process) 
		   are parentless and are treated specially. */
		process_init_resources(newproc, NULL);
	else
		process_init_resources(newproc, CURPROC);

	/* 
		This must be the last thing we do, because once we wakeup the new thread it may run! 
		So we need to have finished the initialization of the PCB.
	*/
	process_exec(newproc, call, argl, args);

	return get_pid(newproc);
}



/*=============================================
 
	Getting information
 
 ============================================*/



/* System call */
Pid_t sys_GetPid()
{
  	return get_pid(CURPROC);
}


Pid_t sys_GetPPid()
{
	return get_pid(CURPROC->parent);
}


int sys_GetError()
{
	return CURPROC->errcode;
}


/*=============================================
 
 Wait on processes for state change
 
 ============================================*/


static void cleanup_zombie(PCB* pcb, int* status)
{
	assert(pcb->pstate == ZOMBIE);
	if(status != NULL)
		*status = pcb->exitval;

	rlist_remove(& pcb->children_node);
	rlist_remove(& pcb->exited_node);

	release_PCB(pcb);
	assert(pcb->pstate == FREE);
}



static Pid_t wait_for_specific_child(PCB* parent, Pid_t cpid, int* status)
{

	/* Legality checks */
	if((cpid<0) || (cpid>=MAX_PROC)) {
  		set_errcode(ESRCH);
    	return NOPROC;
	}

  	/* 
  		Condition:
  		child==NULL or child->parent!=parent or child->pstate==ZOMBIE  

		Action:
		if child==NULL || child->parent!=parent
			return error ECHILD
		else
			cleanup and return status of child

  		It is complicated to write the condition as an expression inside while(), 
  		because get_pcb(cpid) may return a different thing every time a waiter is woken up! 
  		We write it more verbosely as below:
  	 */
  	PCB* child;
	while(1)
	{
  		child = get_pcb(cpid);

  		/* If the condition is true proceed */
  		if(child == NULL
  			|| child->parent!=parent
  			|| child->pstate == ZOMBIE
  			)
  			break;
  		/* Else wait */
		kernel_wait(& parent->child_exit);
    }

    /* This is the action */

    /* error */
	if( child == NULL || child->parent != parent)
	{
		set_errcode(ECHILD);
		return NOPROC;
	}

	/* good case */
	cleanup_zombie(child, status);  
  	return cpid;
}


static Pid_t wait_for_any_child(PCB* parent, int* status)
{
	/* 
		Condition: parent has no children or some child is exited
		Action:
			If parent has no children
				return error ECHILD
			else
				cleanup and return status of some exited child
	*/
	while(! is_rlist_empty(& parent->children_list)
		&& is_rlist_empty(& parent->exited_list))
	{
		kernel_wait(& parent->child_exit);
	}	

  	/* Make sure I have children! */
	if(is_rlist_empty(& parent->children_list)) {
		set_errcode(ECHILD);
		return NOPROC;
	}

	PCB* child = parent->exited_list.next->pcb;
	Pid_t cpid = get_pid(child);
	assert(child->pstate == ZOMBIE);
	cleanup_zombie(child, status);

  	return cpid;
}


Pid_t sys_WaitChild(Pid_t cpid, int* status)
{
  /* Wait for specific child. */
  if(cpid != NOPROC) {
    return wait_for_specific_child(CURPROC, cpid, status);
  }
  /* Wait for any child */
  else {
    return wait_for_any_child(CURPROC, status);
  }

}


/*=============================================
 
	Process termination
 
 ============================================*/



void restore_vfork(PCB*);

void sys_Exit(int exitval)
{

  /* Right here, we must check that we are not the boot task. If we are, 
     we must wait until all processes exit. */
  if(sys_GetPid()==1) {
    while(sys_WaitChild(NOPROC,NULL)!=NOPROC);
  }

  PCB *curproc = CURPROC;  /* cache for efficiency */

  /* Save the exit status */
  curproc->exitval = exitval;

  /* Do all the other cleanup we want here, close files etc. */

  /* Disconnect my main_thread */
  curproc->main_thread = NULL;
  if(curproc->args) {
    free(curproc->args);
    curproc->args = NULL;
  }

  /* Clean up FIDT */
  for(int i=0;i<MAX_FILEID;i++) {
    if(curproc->FIDT[i] != NULL) {
      FCB_decref(curproc->FIDT[i]);
      curproc->FIDT[i] = NULL;
    }
  }

  /* Reparent any children of the exiting process to the 
     initial task */
  PCB* initpcb = get_pcb(1);
  while(!is_rlist_empty(& curproc->children_list)) {
    rlnode* child = rlist_pop_front(& curproc->children_list);
    child->pcb->parent = initpcb;
    rlist_push_front(& initpcb->children_list, child);
  }

  /* Add exited children to the initial task's exited list 
     and signal the initial task */
  if(!is_rlist_empty(& curproc->exited_list)) {
    rlist_append(& initpcb->exited_list, &curproc->exited_list);
    kernel_broadcast(& initpcb->child_exit);
  }

  /* Put me into my parent's exited list */
  if(curproc->parent != NULL) {   /* Maybe this is init */
    rlist_push_front(& curproc->parent->exited_list, &curproc->exited_node);
    kernel_broadcast(& curproc->parent->child_exit);
  }


  /* Now, mark the process as exited. */
  curproc->pstate = ZOMBIE;

  /* Bye-bye cruel world */
  restore_vfork(curproc);
  kernel_unlock();
  exit_thread();
}





/*=============================================
 
	Change of executable in process
 
 ============================================*/


struct vfork_co_state
{
	cpu_context_t thread_ctx;
	void* region_top;
	size_t region_size;
	void* stack;

	/* coroutine stuff */
	cpu_context_t co_ctx;
	void* co_ss_sp;
};

void vfork_call_co(cpu_context_t* save_ctx, struct vfork_co_state* state)
{
	/* 
		Take the stack frame of this call as the start of the region to save.
		To do this, we need to take enough space for the top of the stack
		at the moment that cpu_swap_context() actually switches context.

		We estimate this to be the base of the current stack frame minus
		another 256 bytes. Then, we round down to align with the page size.
	 */
	state->region_top = (void*)(
			(uintptr_t)(__builtin_frame_address(0) - 256) 
		& 
			~((uintptr_t)SYSTEM_PAGE_SIZE-1)
		);
	state->region_size = CURTHREAD->ss_size - (state->region_top - CURTHREAD->ss_sp);
	fprintf(stderr,"Region size to save = %lu\n", state->region_size);
	cpu_swap_context(save_ctx, & state->co_ctx);
}

void restore_vfork(PCB* pcb)
{
	struct vfork_co_state* state = pcb->vfork_state;
	if(state) {
		pcb->vfork_state = NULL;
		vfork_call_co(NULL, state);
	}
}

static void vfork_co_func()
{
	struct vfork_co_state* state = CURPROC->vfork_state;

	/* save the active stack region of the current thread */
	state->stack = aligned_alloc(SYSTEM_PAGE_SIZE, state->region_size);
	memcpy(state->stack, state->region_top, state->region_size);

	/* Prepare for a return into the parent context */
	PCB* parent = CURPROC->parent;

	/* yield to the thread */
	cpu_swap_context(& state->co_ctx, & state->thread_ctx);

	/* restore the saved stack region */
	memcpy(state->region_top, state->stack, state->region_size);
	free(state->stack);

	/* Prepare for a return into the child context */
	CURPROC = parent;

	/* yield to the thread */
	cpu_swap_context(& state->co_ctx, & state->thread_ctx);
}


Pid_t sys_Vfork()
{
	/* The new process PCB */
	PCB* newproc = acquire_PCB();

	if(newproc == NULL) {
		/* We have run out of PIDs! */
		set_errcode(EAGAIN);
		return NOPROC;
	}

	assert(get_pid(newproc)>1); /* Or else, who did we fork? */
	assert(CURPROC!=NULL);

	process_init_resources(newproc, CURPROC);

  	/* 
  		The main thread has already been executed, we do not need these!
  		N.B. there is a possible race here, in addition with signals!
  	 */
  	newproc->argl = 0;
	newproc->args=NULL;

	/* Set the main thread's function to NULL */
	newproc->main_task = CURPROC->main_task;

	/* Allocate the vfork coroutine state */

	struct vfork_co_state* state = (struct vfork_co_state*) malloc(sizeof(struct vfork_co_state));
	newproc->vfork_state = state;

	/* allocate a very small stack for this co-routine, just 2 pages!  */
	// getcontext(& state->thread_ctx);
	size_t co_ss_size = SYSTEM_PAGE_SIZE << 1;
	state->co_ss_sp = aligned_alloc(SYSTEM_PAGE_SIZE, co_ss_size);
	cpu_initialize_context(&state->co_ctx, state->co_ss_sp, co_ss_size, vfork_co_func);

#ifndef NVALGRIND
	int valgrind_stack_id = VALGRIND_STACK_REGISTER(state->co_ss_sp, state->co_ss_sp + co_ss_size);
#endif

	/*==================
		Switch to the coroutine. 
		The saved context will be restored twice. The first time it will
		be restored as the child and the second time it will be restored as 
		the parent.
	====================*/
	CURPROC = newproc;
	vfork_call_co(&state->thread_ctx, state);

	if(CURPROC == newproc) {
		return 0;
	} else {
		/* stack is restored, clean up coroutine state */
#ifndef NVALGRIND
	VALGRIND_STACK_DEREGISTER(valgrind_stack_id);
#endif
		free(state->co_ss_sp);
		free(state);

		return get_pid(newproc);
	}
}



