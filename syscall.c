#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "syscall.h"

// User code makes a system call with INT T_SYSCALL.
// System call number in %eax.
// Arguments are placed by the compiler on the stack, from the user call to the C
// library system call function. The saved user %esp points
// to a saved program counter, and then the first argument.

// Fetch the int at addr from the current process.
int
fetchint(uint addr, int *ip)
{
  struct proc *curproc = myproc();

  if(addr >= curproc->sz || addr+4 > curproc->sz) // invalid stack address, ignore
    return -1;
  *ip = *(int*)(addr); // simple 4-byte dereference at addr
  return 0;
}

// Fetch the nul-terminated string at addr from the current process.
// Doesn't actually copy the string - just sets the pointer at address pp to point at it.
// Returns length of string, not including nul.
int
fetchstr(uint addr, char **pp)
{
  char *s, *ep;
  struct proc *curproc = myproc();

  if(addr >= curproc->sz)
    return -1;
  *pp = (char*)addr; // point to the address of the string
  ep = (char*)curproc->sz;
  for(s = *pp; s < ep; s++){ // find the end of the string
    if(*s == 0)
      return s - *pp; // return length of string read
  }
  return -1;
}

// Fetch the nth 32-bit system call argument.
int
argint(int n, int *ip)
{
  return fetchint((myproc()->tf->esp) + 4 + 4*n, ip); // the +4 because eip pushed after the arguments
}

// Fetch the nth word-sized system call argument as a pointer
// to a block of memory of size bytes.  Check that the pointer
// lies within the process address space.
int
argptr(int n, char **pp, int size)
{
  int i;
  struct proc *curproc = myproc();
 
  if(argint(n, &i) < 0)
    return -1;
  if(size < 0 || (uint)i >= curproc->sz || (uint)i+size > curproc->sz)
    return -1;
  *pp = (char*)i;
  return 0;
}

// Fetch the nth word-sized system call argument as a string pointer.
// Check that the pointer is valid and the string is nul-terminated.
// (There is no shared writable memory, so the string can't change
// between this check and being used by the kernel.)
int
argstr(int n, char **pp)
{
  int addr;
  if(argint(n, &addr) < 0)
    return -1;
  // addr is the nth argument, which is the address of a string too. (the argument was char *)
  return fetchstr(addr, pp);
}

// this stringent requirement of signature is so that the syscall table can be defined as an array of function pointers. Not inheriting from object, oof again.
extern int sys_chdir(void);
extern int sys_close(void);
extern int sys_dup(void);
extern int sys_exec(void);
extern int sys_exit(void);
extern int sys_fork(void);
extern int sys_fstat(void);
extern int sys_getpid(void);
extern int sys_kill(void);
extern int sys_link(void);
extern int sys_mkdir(void);
extern int sys_mknod(void);
extern int sys_open(void);
extern int sys_pipe(void);
extern int sys_read(void);
extern int sys_sbrk(void);
extern int sys_sleep(void);
extern int sys_unlink(void);
extern int sys_wait(void);
extern int sys_write(void);
extern int sys_uptime(void);
// extern int sys_worldpeace(void); // Added for worldpeace system call
// extern int sys_numberofprocesses(void); // Added for numberofprocesses system call
// extern int sys_whatsthestatus(void); // Added for whatsthestatus system call - notice the argument type is void.
// extern int sys_spawn(void); // Added for spawn system call
extern int sys_getpasize(void);

// the traptable is an array of function pointers
static int (*syscalls[])(void) = {
[SYS_fork]    sys_fork,
[SYS_exit]    sys_exit,
[SYS_wait]    sys_wait,
[SYS_pipe]    sys_pipe,
[SYS_read]    sys_read,
[SYS_kill]    sys_kill,
[SYS_exec]    sys_exec,
[SYS_fstat]   sys_fstat,
[SYS_chdir]   sys_chdir,
[SYS_dup]     sys_dup,
[SYS_getpid]  sys_getpid,
[SYS_sbrk]    sys_sbrk,
[SYS_sleep]   sys_sleep,
[SYS_uptime]  sys_uptime,
[SYS_open]    sys_open,
[SYS_write]   sys_write,
[SYS_mknod]   sys_mknod,
[SYS_unlink]  sys_unlink,
[SYS_link]    sys_link,
[SYS_mkdir]   sys_mkdir,
[SYS_close]   sys_close,
// [SYS_worldpeace] sys_worldpeace, // Added for worldpeace system call
// [SYS_numberofprocesses] sys_numberofprocesses, // Added for numberofprocesses system call
// [SYS_whatsthestatus] sys_whatsthestatus, // Added for whatsthestatus system call
// [SYS_spawn] sys_spawn, // Added for spawn system call
[SYS_getpasize] sys_getpasize,
};

void
syscall(void)
{
  int num;
  struct proc *curproc = myproc(); // so we are 

  // find the trap number, that libc puts in eax before calling trap (that's how it worked in asm syscalls too) and since after the trap call the trapframe copies the eax, we can just read it from there
  num = curproc->tf->eax;
  if(num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    // call the handler. The return value is stored in eax, again
    curproc->tf->eax = syscalls[num](); // indexing into the trap table/syscall table and calling the function
  } else {
    // invalid syscall number
    cprintf("%d %s: unknown sys call %d\n",
            curproc->pid, curproc->name, num);
    curproc->tf->eax = -1; // invalid return value
  }
}
