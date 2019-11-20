
#ifndef __KERNEL_SIGNAL_H
#define __KERNEL_SIGNAL_H


/* Check if we have been sent a signal and take action */
void check_sigs();


/* Send a sig to a process */
void send_sig(PCB* pcb);



#endif
