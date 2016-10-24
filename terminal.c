#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <assert.h>
#include <error.h>
#include <errno.h>

/* 
	Tests that there is still input to the terminal. 
	When this goes to 0, the terminal exits on the
	next disconnect.
*/
int INPUT_OPEN=1;	


int confd, kbdfd;  /* The pipe file descriptors */

/* Polling array */
struct pollfd fds[4] = {
	{ 0, POLLIN, 0 },
	{ 1, POLLOUT, 0 },
	{ 0, POLLOUT, 0 },
	{ 0, POLLIN, 0 },
};

/* some operations on the polling  array */

/* Flip/negate fd field; poll ignores negative fds */
#define flip(i) (fds[i].fd = - fds[i].fd - 1)
#define ready(i)  (fds[i].revents & fds[i].events)
#define err(i)  (fds[i].revents & (POLLERR|POLLHUP))
#define polled(i)  (fds[i].fd>=0)

int state[4];  /* Last poll result */
int errst[4];  /* Whether error was detected */

/* Mnemonic macros */
#define IN state[0]
#define OUT state[1]
#define KBD state[2]
#define CON state[3]

#define INERR errst[0]
#define OUTERR errst[1]
#define KBDERR errst[2]
#define CONERR errst[3]

/* Helper */
void transfer_byte(int from, int to, int fromfd, int tofd)
{
	int rc;
	char buf;
	rc=read(fromfd, &buf, 1);
	if(rc==1) rc=write(tofd, &buf, 1);
#if EXIT_ON_STDIN_CLOSE
	if(rc==0) { 
		assert(fromfd==0); 
		if(! isatty(0)) { 
			INPUT_OPEN=0; close(0); 
		}
		fprintf(stderr, "Stdin closed\n"); 
	}
#endif
	flip(from); flip(to);  /* They are not ready any more */	
}

/* Loop transferring bytes between the streams */
void io_loop()
{
	/* acquire fds */
	fds[2].fd = kbdfd;
	fds[3].fd = confd;

	while(1) {
		/* Poll the files */
		poll(fds, 4, -1);

		/* Update indicators */
		for(int i=0; i<4; i++) {
			if(polled(i)) { 
				state[i]=ready(i);  
				if(state[i]) flip(i);  /* Flip ready files */
				errst[i]=err(i); 
			}
		}
		if(errst[0]) fprintf(stderr,"Error in 0\n");
		if(errst[1]) fprintf(stderr,"Error in 1\n");

		int XKBD = IN && KBD;
		int XCON = OUT && CON;

		/* Break if we have no transfers and some error */
		if(!XKBD && !XCON && (KBDERR||CONERR)) break;

		/* Do ready transfers */
		if( XKBD ) transfer_byte(0, 2, 0, kbdfd);
		if( XCON ) transfer_byte(3, 1, confd, 1);
	}

	close(confd);
	close(kbdfd);
}


const char* disconnected = "\n\033[5;41;1;37m   *** DISCONNECTED ***   \033[0m\n";
const char* connected = "\033[5;40;1;37m   *** CONNECTED ***   \033[0m\n";

/* This call will block until the pipe is opened by the peer */
int open_pipe(const char* fname, int flags) {
	int fd = open(fname, flags);
	if(fd==-1) 	error(1, errno, "opening %s", fname);
	return fd;
}

void mainloop(char* arg)
{
	char confname[10], kbdfname[10];
	sprintf(confname, "con%s", arg);
	sprintf(kbdfname, "kbd%s", arg);

	/* We close and re-open on every disconnect, in order to
	   sleep */

	while(INPUT_OPEN) {
	  	printf(disconnected); fflush(stdout);
		confd = open_pipe(confname, O_RDONLY);
		kbdfd = open_pipe(kbdfname, O_WRONLY);
		printf(connected); fflush(stdout);
		io_loop();
	}
}

void usage() 
{
	printf("usage: terminal <n>         where n = 0..3\n");
	exit(1);
}

int main(int argc, char** argv)
{
	if(argc!=2 || strlen(argv[1])!=1 || argv[1][0]<'0' || argv[1][0]>'3')
		usage();
	//signal(SIGPIPE, SIG_IGN);
	mainloop(argv[1]);
	return -1;  /* Does not return */
}

