
#include "tinyos.h"
#include "kernel_proc.h"


Fid_t sys_Socket(port_t port)
{
	set_errcode(ENOSYS);
	return NOFILE;
}

int sys_Listen(Fid_t sock)
{
	set_errcode(ENOSYS);
	return -1;
}


Fid_t sys_Accept(Fid_t lsock)
{
	set_errcode(ENOSYS);
	return NOFILE;
}


int sys_Connect(Fid_t sock, port_t port, timeout_t timeout)
{
	set_errcode(ENOSYS);
	return -1;
}


int sys_ShutDown(Fid_t sock, shutdown_mode how)
{
	set_errcode(ENOSYS);
	return -1;
}

