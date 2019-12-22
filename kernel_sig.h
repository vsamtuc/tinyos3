#ifndef __KERNEL_SIGNAL_H
#define __KERNEL_SIGNAL_H


/**
	@brief Check raised signals of current process.

	This function is called from certain well-defined points
	in the kernel code. It takes the currently raised signals
	that are not masked, and dispatches the corresponding
	signal handlers.

	Currently, the points where signals are checked are the points
	where a process is:
	(a) trying to enter the kernel by calling some system call,
	(b) leaving the kernel after some system call, 
	(c) returning to the process code from an interrupt handler,
		including the \c ALARM interrupt, i.e., at the beginning
		of a quantum,
	(d) returning from Cond_Wait
 */
void check_sigs();


/* Send a sig to a process */
void send_sig(PCB* pcb);



#endif
