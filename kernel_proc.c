
#include <assert.h>
#include "kernel_cc.h"
#include "kernel_sys.h"
#include "kernel_proc.h"
#include "kernel_streams.h"
#include "kernel_fs.h"
#include "kernel_exec.h"

#ifndef NVALGRIND
#include <valgrind/valgrind.h>
#endif

/* 
 The process table and related system calls:
 - Spawn
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
	pcb->xargs = XARGS_NULL;

	for(int i=0;i<MAX_FILEID;i++)
		pcb->FIDT[i] = NULL;

	rlnode_init(& pcb->children_list, NULL);
	rlnode_init(& pcb->exited_list, NULL);
	rlnode_init(& pcb->children_node, pcb);
	rlnode_init(& pcb->exited_node, pcb);
	wqueue_init(& pcb->child_exit, &wchan_wait_child);
}


static PCB* pcb_freelist;
static PCB* pcb_usedlist;

void initialize_processes()
{
	/* initialize the PCBs */
	for(Pid_t p=0; p<MAX_PROC; p++) {
		initialize_PCB(&PT[p]);
	}

	/* use the parent field to build a free list */
	PCB* pcbiter;
	pcb_freelist = NULL;
	pcb_usedlist = NULL;
	for(pcbiter = PT+MAX_PROC; pcbiter!=PT; ) {
		--pcbiter;
		pcbiter->parent = pcb_freelist;
		pcb_freelist = pcbiter;
	}

	process_count = 0;

	/* Execute a null "scheduler" process */
	if(sys_Spawn(NULL,0,NULL)!=0)
		FATAL("The scheduler process does not have pid==0");
}


/*
   Must be called with kernel_mutex held
 */
PCB* acquire_PCB()
{
	PCB* pcb = NULL;

	if(pcb_freelist==NULL) {
		pcb_freelist = pcb_usedlist;
		pcb_usedlist = NULL;
	}

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
	pcb->parent = pcb_usedlist;
	pcb_usedlist = pcb;
	process_count--;
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

	Task call =  CURPROC->xargs.task;
	int argl = CURPROC->xargs.argl;
	void* args = CURPROC->xargs.args;

	exitval = call(argl,args);
	Exit(exitval);
}


/* 
   Create and wake up the thread for the main function. 
 */
void process_exec(PCB* proc, struct exec_args* xargs)
{
	/* Set the main thread's function */
	proc->xargs = *xargs;

	if(xargs->task != NULL) {
		proc->main_thread = spawn_thread(proc, start_main_thread);
		wakeup(proc->main_thread);
	}
}

/*
	Clear the process's execution info.

	Note: this will potentially release a DSO providing user-space code, make sure
	`proc` is not, and will not, execute user-space code after this call!
 */
void process_clear_exec_info(PCB* proc)
{

	
	if(proc->xargs.args) free(proc->xargs.args);
	if(proc->xargs.lease) dlobj_decref(proc->xargs.lease);

	proc->xargs = XARGS_NULL;
	proc->errcode = 0;
}


_Noreturn void process_exit_thread()
{
	/* We need to release the kernel lock before we die! */
	kernel_unlock();
	exit_thread();
}


/* The system call */
int sys_Exec(const char* pathname, const char* argv[], const char* envp[])
{
	struct exec_args XA;
	int rc = exec_program(&XA, pathname, (void*)argv, envp, 0);
	if(rc) { return -1; }

	process_clear_exec_info(CURPROC);
	process_exec(CURPROC, &XA);
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

	if(parent==NULL) {
		if(get_pid(newproc) == 1) {
			/* This is init, we need to prepare resources that will be inherited. */

			/* Make the current and root directories point at the mounted system root */
			newproc->root_dir = pin_inode(mount_table, mount_table->root_dir);
			newproc->cur_dir = newproc->root_dir;
			repin_inode(newproc->cur_dir);

		}
		return;
	}

	/* link from parent */
	rlist_push_front(& parent->children_list, & newproc->children_node);

	/* get the root and current directories */
	newproc->root_dir = parent->root_dir;
	repin_inode(newproc->root_dir);

	newproc->cur_dir = parent->cur_dir;
	repin_inode(newproc->cur_dir);

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

	/* Renewed lease from parent */
	struct dlobj* lease = NULL;

	if(get_pid(newproc)<=1) 
		/* Processes with pid<=1 (the scheduler and the init process) 
		   are parentless and are treated specially. */
		process_init_resources(newproc, NULL);
	else {
		process_init_resources(newproc, CURPROC);
		lease = CURPROC->xargs.lease;
		if(lease) dlobj_incref(lease);
	}

	/* Copy the argument */
	void* args_copy = NULL;
	if(argl) {
		args_copy = xmalloc(argl);
		memcpy(args_copy, args, argl);
	}

	/* 
		Execute the new program.

		This must be the last thing we do, because once we wakeup the new thread it may run! 
		So we need to have finished the initialization of the PCB.
	 */
	struct exec_args XA = {.task = call, .argl = argl, .args = args_copy, .lease = lease };
	process_exec(newproc, &XA);

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

	/* Release directory handles */
	unpin_inode(curproc->root_dir);
	unpin_inode(curproc->cur_dir);

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





