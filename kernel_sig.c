
#include "kernel_cc.h"
#include "kernel_proc.h"
#include "kernel_sig.h"


/*
	This mechanism is implemented in the non-preemptive domain, and
	to this end requires a spinlock.

	A thread can trigger its pending signals only in certain contexts,
	so that signal handlers can be assured to work correctly. 

	Current implementation: it must be in user mode, that is,
	(a) trying to enter the kernel
	(b) leaving the kernel
	(c) returning to user mode from an interrupt handler

 */


Mutex sig_spinlock = MUTEX_INIT;


void check_sigs()
{
	int sigs=0;

	/* Get the list of currently raised signals */
	int preempt = preempt_off;
	spin_lock(&sig_spinlock);

	sigs = CURPROC->sigkill;
	CURPROC->sigkill = 0;

	spin_unlock(&sig_spinlock);
	if(preempt) preempt_on;
	
	if(sigs) Exit(-1);
}



int sys_Kill(Pid_t pid)
{
	if(pid==1) { set_errcode(EPERM); return -1; }

	PCB* target = get_pcb(pid);
	if(pid==0 || target==NULL) { set_errcode(EINVAL); return -1; }

	/* deliver the signal to the target process */
	int preempt = preempt_off;
	spin_lock(&sig_spinlock);
	target->sigkill |= 1;
	spin_unlock(&sig_spinlock);
	if(preempt) preempt_on;

	/* Make sure the thread is awake */
	wakeup(target->main_thread);
	
	return 0;
}
