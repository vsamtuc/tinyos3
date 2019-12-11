#ifndef __KERNEL_SYS_H
#define __KERNEL_SYS_H

/**
	@file kernel_sys.h
	@brief System call invocation

	This file contains macros which implement the invocation of TinyOS system calls from
	proceses. The main implementation issue is to enforce monitor semantics:
	the kernel is locked by a call to @c kernel_lock() just as a system call begins,
	and is unlocked by a call to @c kernel_unlock() as the system call returns.

	For each system call function, there is an implementation function whose name is prefixed by "sys_",
	where the call is forwarded. For example, system call @c Exec() is forwarded to function @c sys_Exec().

	@defgroup kernel  The TinyOS kernel

  */


#include "bios.h"
#include "tinyos.h"

#define SYSCALLS \
SYSCALL(GetError, int, (void), ())\
SYSCALL(Spawn, int, (Task task, int argl, void* args), (task, argl, args))\
SYSCALLV(Exec, (Task task, int argl, void* args), (task, argl, args))\
SYSCALLV(Exit, (int exitval), (exitval))\
SYSCALL(GetPid, int, (void), ())\
SYSCALL(GetPPid, int, (void), ())\
SYSCALL(Kill, int, (Pid_t pid), (pid))\
SYSCALL(WaitChild, Pid_t, (Pid_t proc, int* exitval), (proc, exitval))\
SYSCALL(CreateThread, Tid_t, (Task task, int argl, void* args), (task, argl, args))\
SYSCALL(ThreadSelf, Tid_t, (void), ())\
SYSCALL(ThreadJoin, int, (Tid_t tid, int* exitval), (tid, exitval))\
SYSCALL(ThreadDetach, int, (Tid_t tid), (tid))\
SYSCALLV(ThreadExit, (int exitval), (exitval))\
SYSCALL(GetTerminalDevices, unsigned int, (), ())\
SYSCALL(OpenTerminal, Fid_t, (unsigned int termno), (termno))\
SYSCALL(OpenNull, Fid_t, (), ())\
SYSCALL(Open, Fid_t, (const char* pathname, int flags), (pathname, flags))\
SYSCALL(Seek, intptr_t, (int fid, intptr_t offset, int whence), (fid, offset, whence))\
SYSCALL(Stat, int, (const char* pathname, struct Stat* statbuf), (pathname, statbuf))\
SYSCALL(Link, int, (const char* pathname, const char* newpath), (pathname, newpath))\
SYSCALL(Unlink, int, (const char* pathname), (pathname))\
SYSCALL(MkDir, int, (const char* pathname), (pathname))\
SYSCALL(RmDir, int, (const char* pathname), (pathname))\
SYSCALL(ChDir, int, (const char* pathname), (pathname))\
SYSCALL(GetCwd, int, (char* buffer, unsigned int size), (buffer, size))\
SYSCALL(Mount, int, (Dev_t dev, const char* mpoint, const char* fstype, unsigned int paramc, mount_param* paramv),(dev,mpoint,fstype,paramc, paramv))\
SYSCALL(Umount, int, (const char* mpoint),(mpoint))\
SYSCALL(StatFs, int, (const char* mpoint, struct StatFs* statfs), (mpoint, statfs))\
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