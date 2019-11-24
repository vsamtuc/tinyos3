
#include <assert.h>
#include "kernel_cc.h"
#include "kernel_sys.h"
#include "kernel_proc.h"
#include "kernel_streams.h"

#ifndef NVALGRIND
#include <valgrind/valgrind.h>
#endif


/*
	Thread snapshots: this is a mechanism for implementing Vfork().
 */

static void load_snapshot(thread_snapshot* snap)
{
	size_t size = (CURTHREAD->ss_sp + CURTHREAD->ss_size)-snap->sp;
	memcpy(snap->sp, snap->data, size);
}

void restore_snapshot()
{
	if(is_rlist_empty(&CURTHREAD->snapshots)) return;
	thread_snapshot* snap = (thread_snapshot*) rlist_pop_front(&CURTHREAD->snapshots)->obj;
	assert(snap!=NULL);
	assert(CURTHREAD == snap->tcb);

	/* 
		Ok, we must restore ourselves.

		This is tricky, because we are running on the thread we are about to restore! 

		Thus, if we are running on a whose address is inside the snapshot,
		we must do the restoration on another stack! Thankfully it doesn't
		have to be very big, so we can use the unused space on our stack!

		Otoh, if our current stack frame is above the snapshot, we should be fine!
	*/

	if((uintptr_t)snap->sp > (uintptr_t) __builtin_frame_address(0) )
	{
		/* We can directly copy the snapshot and jump to it! */
		load_snapshot(snap);
		setcontext(&snap->context);
	} 
	/* 
		We can use the top of the thread stack for our mini context.
		The stack we need should be minimal.
	 */
#define SNAP_LOAD_SS_SIZE 1024
	assert((snap->sp - CURTHREAD->ss_sp) > SNAP_LOAD_SS_SIZE);
	ucontext_t ctx;
	assert( (void*)&ctx - CURTHREAD->ss_sp > SNAP_LOAD_SS_SIZE );
	getcontext(&ctx);	
	ctx.uc_link = &snap->context;
	ctx.uc_stack.ss_sp = CURTHREAD->ss_sp;
	ctx.uc_stack.ss_size = SNAP_LOAD_SS_SIZE;
	ctx.uc_stack.ss_flags = 0;
	makecontext(&ctx, (void*)load_snapshot, 1, snap);
	setcontext(&ctx);
}


static void save_snapshot(thread_snapshot* snap, volatile int* snap_flag)
{
	snap->tcb = CURTHREAD;
	rlnode_init(& snap->snapnode, snap);
	rlist_push_front(& CURTHREAD->snapshots, & snap->snapnode);
	snap->sp = (void*) snap->context.uc_mcontext.gregs[REG_RSP];
	ssize_t size = (CURTHREAD->ss_sp+CURTHREAD->ss_size)-snap->sp;
	assert(size >= 0);
#if 0
	fprintf(stderr, "Snapshot size=%lu\n", size);
#endif
	snap->data = xmalloc(size);
	*snap_flag = 0;
	memcpy(snap->data, snap->sp, size);
}

thread_snapshot* take_snapshot()
{
	/* Create a snapshot thread */
	thread_snapshot* snap = (thread_snapshot*)malloc(sizeof(thread_snapshot));

	/* Status flag: 1 for taking the snapshot, 0 for restoration */
	volatile int snap_flag = 1;

	/* Save the context on which we will return */
	getcontext(& snap->context);

	/* We are returning after the snapshot is restored  */
	if(snap_flag==0) {
		free(snap->data);
		free(snap);
		return NULL;
	}

	/* Save and return the snapshot */
	save_snapshot(snap, &snap_flag);
	return snap;
}






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
  pcb->snapshot = NULL;

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

void process_clear_exec_info(PCB* proc)
{
	if(proc->args != NULL) free(proc->args);
	proc->argl = 0;
	proc->args = NULL;
	proc->main_task = NULL;
}


void process_exit_thread()
{

  	/*
		Note: if the exiting thread is snapshotted, the previous snapshot will be restored and
		restore_snapshot() will not return.
		Else, if there are no snapshots to be restored, restore_snapshot() returns, and the
		thread is terminated by the call to exit_thread().
	*/
	restore_snapshot();
	/* We need to release the kernel lock before we die! */
  	kernel_unlock();
  	exit_thread();
}


void sys_Exec(Task call, int argl, void* args)
{
	process_clear_exec_info(CURPROC);
	process_exec(CURPROC, call, argl, args);
	process_exit_thread();
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
#if 0
	if(get_pcb(1) && get_pcb(1)->pstate==ALIVE) {
		Pid_t cpid = sys_Vfork();
		if(cpid==0) {
			sys_Exec(call,argl,args);
		}
		return cpid;		
	}
#endif

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



void restore_snapshot();

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
  process_clear_exec_info(curproc);

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

	process_exit_thread();
}


/*=============================================
 
	Fork
 
 ============================================*/



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
	newproc->main_task=NULL;

	Pid_t newpid = get_pid(newproc);
	assert(CURTHREAD==CURPROC->main_thread);
	PCB* oldproc = CURPROC;

	/* Ok, we are ready to fork */
	thread_snapshot* snap = take_snapshot();
	if(snap != NULL) {
		/* Return in the child process */
		newproc->main_thread = CURTHREAD;
		oldproc->snapshot = snap;
		CURPROC = newproc;
		return 0;
	} else {
		/* Return in the parent process */
		oldproc->snapshot = NULL;
		CURPROC = oldproc;
		return newpid;
	}


}



