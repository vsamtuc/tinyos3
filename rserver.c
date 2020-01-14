

/*************************************

	A remote server

***************************************/

#define REMOTE_SERVER_DEFAULT_PORT 20

/*
  The server's "global variables".
 */
struct __rs_globals
{
	/* a global flag */
	int quit;

	/* server related */
	port_t port;
	Tid_t listener;
	Fid_t listener_socket;

	/* Statistics */
	size_t active_conn;
	size_t total_conn;

	/* used so that each connection gets a unique id */
	size_t conn_id_counter;
	
	/* used to log connection messages */
	rlnode log;
	size_t logcount;
	
	/* Synchronize with active threads */
	Mutex mx;
	CondVar conn_done;
};

#define GS(name) (((struct __rs_globals*) __globals)->name)

/* forward decl */
static int rsrv_client(int sock, void* __globals);
static void log_message(void* __globals, const char* msg, ...)
	__attribute__((format(printf,2,3)));
static void log_init(void* __globals);
static void log_print(void* __globals);
static void log_truncate(void* __globals);

static int rsrv_listener_thread(int port, void* __globals);

/* the thread that accepts new connections */
static int rsrv_listener_thread(int port, void* __globals)
{
	Fid_t lsock = Socket(port);
	if(Listen(lsock) == -1) {
		printf("Cannot listen to the given port: %d\n", port);
		return -1;
	}
	GS(listener_socket) = lsock;

	/* Accept loop */
	while(1) {
		Fid_t sock = Accept(lsock);
		if(sock==NOFILE) {
			/* We failed! Check if we should quit */
			if(GS(quit)) return 0;
			log_message(__globals, "listener(port=%d): failed to accept!\n", port);
		} else {
			GS(active_conn)++;
			GS(total_conn)++;
			Tid_t t = CreateThread(rsrv_client, sock, __globals);
			ThreadDetach(t);
		}
	}
	return 0;
}


/*  The main server process */
int RemoteServer(size_t argc, const char** argv)
{
	/* Create the globals */
	struct __rs_globals __global_obj;
	struct __rs_globals *__globals = &__global_obj;

	GS(mx) = MUTEX_INIT;
	GS(conn_done) = COND_INIT;
	
	GS(quit) = 0;
	GS(port) = REMOTE_SERVER_DEFAULT_PORT;
	GS(active_conn) = 0;
	GS(total_conn) = 0;
	GS(conn_id_counter) = 0;

	log_init(__globals);

	/* Start a thread to listen on */
	GS(listener) = CreateThread(rsrv_listener_thread, GS(port), __globals);
	
	/* Enter the server console */
	char* linebuff = NULL;
	size_t lineblen = 0;
	FILE* fin = fidopen(0,"r");

	while(1) {
		printf("Type h for help, or a command: ");
		int rc;
		again:
		rc = getline(&linebuff, &lineblen, fin);
		if(rc==-1 && ferror(fin) && errno==EINTR) { 
			clearerr(fin); 
			goto again; 
		}
		if(rc==-1 && feof(fin)) {
			break;
		}

		assert(linebuff!=NULL);

		if(strcmp(linebuff, "q\n")==0) {
			/* Quit */
			GS(quit) = 1;
			printf("Quitting\n");
			Close(GS(listener_socket));
			ThreadJoin(GS(listener), NULL);

			Mutex_Lock(&GS(mx));
			while(GS(active_conn)>0) {
				printf("Waiting %zu connections ...\n", GS(active_conn));
				Cond_Wait(&GS(mx), &GS(conn_done));
			}
			Mutex_Unlock(&GS(mx));
			
			
			log_truncate(__globals);
			break;
		} else if(strcmp(linebuff, "s\n")==0) {
			/* Show statistics */
			printf("Connections: active=%4zd total=%4zd\n", 
				GS(active_conn), GS(total_conn));
		} else if(strcmp(linebuff, "h\n")==0) {
			printf("Commands: \n"
			       "q: quit the server\n"
			       "h: print this help\n"
			       "s: show statistics\n"
			       "l: show the log\n");
		} else if(strcmp(linebuff, "l\n")==0) {
			log_print(__globals);
		} else if(strcmp(linebuff, "\n")==0) {
		} else {
			printf("Unknown command: '%s'\n", linebuff);
		}
	}

	fclose(fin);
	free(linebuff);
	return 0;
}

typedef struct {
	rlnode node;
	char message[0];
} logrec;

/* log a message */
static void log_message(void* __globals, const char* msg, ...)
{
	/* put the log record in a memory buffer */
        char* buffer = NULL;
        size_t buffer_size;
        FILE* output = open_memstream(&buffer, &buffer_size);

	/* At the head of the buffer, reserve space for a logrec */
	fseek(output, sizeof(logrec), SEEK_SET);

	/* Add the message */
        va_list ap;
        va_start (ap, msg);
        vfprintf (output, msg, ap);
        va_end (ap);
        fclose(output);

	/* Append the record */
	logrec *rec = (logrec*) buffer;
	Mutex_Lock(& GS(mx));
	rlnode_new(& rec->node)->num = ++GS(logcount);
	rlist_push_back(& GS(log), & rec->node);
	Mutex_Unlock(& GS(mx));
}

/* init the log */
static void log_init(void* __globals)
{
	rlnode_init(& GS(log), NULL);
	GS(logcount)=0;
}

/* Print the log to the console */
static void log_print(void* __globals)
{
	Mutex_Lock(& GS(mx));
	for(rlnode* ptr = GS(log).next; ptr != &GS(log); ptr=ptr->next) {
		logrec *rec = (logrec*)ptr;
		printf("%6d: %s\n", rec->node.num, rec->message);
	}
	Mutex_Unlock(& GS(mx));
}

	
/* truncate the log */
static void log_truncate(void* __globals)
{
	rlnode list;
	rlnode_init(&list, NULL);
	
	Mutex_Lock(& GS(mx));
	rlist_append(& list, &GS(log));
	Mutex_Unlock(& GS(mx));

	/* Free the memory ! */
	while(list.next != &list) {
		rlnode *rec = rlist_pop_front(&list);
		free(rec);
	}
}



/* Helper to receive a message */
static int recv_message(Fid_t sock, void* buf, size_t len)
{
	size_t count = 0;
	while(count<len) {
		int rc = Read(sock, buf+count, len-count);
		if(rc<1) break;  /* Error or end of stream */
		count += rc;
	}
	return count==len;
}

/* Helper to execute a remote process */
static int rsrv_process(size_t argc, const char** argv)
{
	checkargs(2);
	Fid_t sock = atoi(argv[1]);

	/* Fix the streams */
	assert(sock!=0 && sock!=1);	
	Dup2(sock, 0);
	Dup2(sock, 1);
	Close(sock);

	
	/* Execute */
	int exitstatus;
	WaitChild(SpawnProgram(argv[2], argc-2, argv+2), &exitstatus);
	return exitstatus;
}

/* this server thread serves a remote cliend */
static int rsrv_client(int sock, void* __globals)
{
	/* Get your client id */
	Mutex_Lock(&GS(mx));
	size_t ID = ++GS(conn_id_counter);
	Mutex_Unlock(&GS(mx));

	log_message(__globals, "Client[%6zu]: started", ID);
	
        /* Get the command from the client. The protocol is
	   [int argl, void* args] where argl is the length of
	   the subsequent message args.
	 */
	int argl;
	if(! recv_message(sock, &argl, sizeof(argl))) {
		log_message(__globals,
			    "Cliend[%6zu]: error in receiving request, aborting", ID);
		goto finish;
	}
		
	assert(argl>0 && argl <= 2048);
	{
		char args[argl];
		if(! recv_message(sock, args, argl)) {
			log_message(__globals,
				    "Cliend[%6zu]: error in receiving request, aborting", ID);
			goto finish;		
		}
		
		/* Prepare to execute subprocess */
		size_t argc = argscount(argl, args);	
		const char* argv[argc+2];
		argv[0] = "rsrv_process";
		char sock_value[32];
		sprintf(sock_value, "%d", sock);
		argv[1] = sock_value;
		argvunpack(argc, argv+2, argl, args);
	
		/* Now, execute the message in a new process */
		int exitstatus;
		Pid_t pid = Execute(rsrv_process, argc+2, argv);
		Close(sock);
		WaitChild(pid, &exitstatus);
	
		log_message(__globals, "Client[%6zu]: finished with status %d",
			    ID, exitstatus);
	}

finish:
	Mutex_Lock(&GS(mx));
	GS(active_conn)--;
	Cond_Broadcast(& GS(conn_done));
	Mutex_Unlock(&GS(mx));
	return 0;
}

/*********************
   the client program
************************/

/* helper for RemoteClient */
static void send_message(Fid_t sock, void* buf, size_t len)
{
	size_t count = 0;
	while(count<len) {
		int rc = Write(sock, buf+count, len-count);
		if(rc<1) break;  /* Error or End of stream */
		count += rc;
	}
	if(count!=len) {
		printf("In client: I/O error writing %zu bytes (%zu written)\n", len, count);
		Exit(1);
	}
}

/* the remote client program */
int RemoteClient(size_t argc, const char** argv)
{
	checkargs(1);
	
	/* Create a socket to the server */
	Fid_t sock = Socket(NOPORT);
	if(Connect(sock, REMOTE_SERVER_DEFAULT_PORT, 1000)==-1) {
		printf("Could not connect to the server\n");
		return -1;
	}

	assert(sock!=NOFILE);

	/* Make up the message */
	int argl = argvlen(argc-1, argv+1);
	char args[argl];
	argvpack(args, argc-1, argv+1);

	/* Send message */
	send_message(sock, &argl, sizeof(argl));
	send_message(sock, args, argl);
	ShutDown(sock, SHUTDOWN_WRITE);

	/* Read the server data and display */
	char c;
	FILE* fin = fidopen(sock, "r");
	FILE* fout = fidopen(1, "w");
	while((c=fgetc(fin))!=EOF) {
		fputc(c, fout);
	}
	fclose(fin);
	fclose(fout);
	return 0;
}

