#ifndef __KERNEL_SYS_H
#define __KERNEL_SYS_H

#include "bios.h"
#include "tinyos.h"

#define SYSCALLS \
SYSCALL(Exec, int, (Task task, int argl, void* args), (task, argl, args))\
SYSCALLV(Exit, (int exitval), (exitval))\
SYSCALL(GetPid, int, (void), ())\
SYSCALL(GetPPid, int, (void), ())\
SYSCALL(WaitChild, Pid_t, (Pid_t proc, int* exitval), (proc, exitval))\
SYSCALL(CreateThread, Tid_t, (Task task, int argl, void* args), (task, argl, args))\
SYSCALL(ThreadSelf, Tid_t, (void), ())\
SYSCALL(ThreadJoin, int, (Tid_t tid, int* exitval), (tid, exitval))\
SYSCALL(ThreadDetach, int, (Tid_t tid), (tid))\
SYSCALLV(ThreadExit, (int exitval), (exitval))\
SYSCALL(GetTerminalDevices, unsigned int, (), ())\
SYSCALL(OpenTerminal, Fid_t, (unsigned int termno), (termno))\
SYSCALL(OpenNull, Fid_t, (), ())\
SYSCALL(Read,int,(Fid_t fd, char *buf, unsigned int size), (fd,buf,size))\
SYSCALL(Write,int,(Fid_t fd, const char *buf, unsigned int size), (fd,buf,size))\
SYSCALL(Close,int,(Fid_t fd),(fd))\
SYSCALL(Dup2,int, (Fid_t oldfd, Fid_t newfd), (oldfd,newfd))\
SYSCALL(Pipe, int, (pipe_t* pipe), (pipe))\
SYSCALL(Socket, Fid_t, (port_t port), (port))\
SYSCALL(Listen, int, (Fid_t sock), (sock))\
SYSCALL(Accept, Fid_t, (Fid_t lsock), (lsock))\
SYSCALL(Connect, int, (Fid_t sock, port_t port, timeout_t timeout), (sock, port, timeout))\
SYSCALL(ShutDown, int, (Fid_t sock, shutdown_mode how), (sock, how))\
SYSCALL(OpenInfo, Fid_t, (), ())\



#define SYSCALL(NAME, RET, SIG, ARGS)\
RET sys_ ## NAME SIG;

/* without return */
#define SYSCALLV(NAME, SIG, ARGS)\
void sys_ ## NAME SIG;

SYSCALLS

#undef SYSCALL
#undef SYSCALLV

#endif