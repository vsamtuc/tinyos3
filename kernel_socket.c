
#include "tinyos.h"


Fid_t Socket(port_t port)
{
	return NOFILE;
}

int Listen(Fid_t sock)
{
	return -1;
}


Fid_t Accept(Fid_t lsock)
{
	return NOFILE;
}


int Connect(Fid_t sock, port_t port, timeout_t timeout)
{
	return -1;
}


int ShutDown(Fid_t sock, shutdown_mode how)
{
	return -1;
}

