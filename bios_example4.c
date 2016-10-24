
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "bios.h"


/* 
	A very simple program to demonstrate alarms and 
	interrupt handlers.
*/


/* interrupt handler */
void handle_alarm()
{
	printf("ALARM in core %d\n", cpu_core_id);
}

/* core boot func */
void bootfunc()
{
	fprintf(stderr, "Core %d\n", cpu_core_id);

	for(int i=0; i<3; i++) {
		/* handle alarm only for even i */
		if((i&1) == 0) {     
			cpu_interrupt_handler(ALARM, handle_alarm);
		} else {
			cpu_interrupt_handler(ALARM, NULL);
		}

		/* Reset the timer.
			Note: setting this to a very small value may 
			deadlock, if the timer expires before cpu_core_halt() 
			is called. 
		*/
		bios_set_timer(1000000); 

		fprintf(stderr, "Core %d is sleeping, i=%d\n", cpu_core_id, i);
		usleep(2000000);

		fprintf(stderr, "Core %d woke up, i=%d\n", cpu_core_id, i);
	}

	fprintf(stderr, "Finished with core %d\n", cpu_core_id);
}


int main()
{
	vm_boot(bootfunc, 2, 0);

	return 0;
}