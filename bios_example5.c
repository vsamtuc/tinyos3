
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <error.h>
#include <unistd.h>

#include "bios.h"


/*
	A simple program to demonstrate using serial devices.

	To run this program, you must execute two terminals
	in two different windows first. Otherwise, the program
	will block.

 */

ssize_t read_serial(void *cookie, char *buf, size_t size)
{
	uint core = cpu_core_id;
	ssize_t ret = 0;
	char c = '\0';

	while(size>0 && c!='\n') {
		/* get a character */
		if(! bios_read_serial(core, &c)) 
		{
			cpu_disable_interrupts();

			/*
				Wait in a busy loop, polling the driver.
			 */
			while(! bios_read_serial(core, &c))
				;
			cpu_enable_interrupts();			
		}

		*buf = c;
		buf++; 
		ret++;
		size--;
}

	return ret;
}



ssize_t write_serial(void* cookie, const char* buf, size_t size) 
{
	uint core = cpu_core_id;
	ssize_t ret = 0;

	while(size > 0) {
		char c = *buf;

		if(bios_write_serial(core, c))
		{
			size --;
			buf ++;
			ret ++;
		}
	}

	return ret;
}


cookie_io_functions_t bios_serial_funcs = {
	read_serial, write_serial, NULL, NULL
};


void bootfunc()
{
	uint mycore = cpu_core_id;
	bios_serial_interrupt_core(mycore, SERIAL_TX_READY, mycore);
	bios_serial_interrupt_core(mycore, SERIAL_RX_READY, mycore);

	/*
		open a buffered stream for convenience
	 */
	FILE* fterm = fopencookie(NULL, "r+", bios_serial_funcs);

	char* linebuf = NULL;
	size_t linebuf_len;

	int n=3;

	while(n--) {

		fprintf(fterm, "Type a line: "); fflush(fterm);
		int rc = getline(&linebuf, &linebuf_len, fterm);
		if(rc==-1) {
			char msg[1024];
			char* message = strerror_r(errno, msg, 1024);
			fprintf(fterm, "error in reading input: %s\n", message);
			fflush(fterm);
		} else
			fprintf(fterm, "You typed: %s", linebuf); 
		fflush(fterm);

		if(strcmp(linebuf, "exit")==0)
			break;
	}

	fprintf(fterm, "Bye bye\n"); 
	fflush(fterm);


	if(linebuf)
		free(linebuf);

	fclose(fterm);
}


int main()
{
	fprintf(stderr, "Please make sure that you have 2 terminals running, or else"
		" this process will be stuck.\nIf you do not see a message to tell you to"
		" 'Type a line', please either start terminals 0 and 1 or kill this process"
		" (by hitting <Control>-C).\n");
	vm_boot(bootfunc, 2, 2);
}
