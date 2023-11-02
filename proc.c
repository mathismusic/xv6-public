#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

// process table. It is guarded by a lock, because it is shared across processes (the race condition for updates still exists).
// when is the lock acquired?
// - to create a new process
// - to change the state of the PCB
// - when the scheduler is checking for runnable processes
// - before putting a process to sleep (we need to set its state to SLEEPING, so)
struct {
  struct spinlock lock; // lock for the process table
  struct proc proc[NPROC]; // array of proc structs
} ptable;

static struct proc *initproc; // the init process

int nextpid = 1; // next pid to be assigned. It is incremented each time a new process is created. (never decremented)
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable"); // initialize the lock for the process table
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus; // index of the current cpu in the cpus array
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop. Wow, concurrency issues.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp; // stack pointer to the kernel stack of the process

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  // otherwise no unused entry found, we return
  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO; // embryo: process created, has not been initialized yet
  p->pid = nextpid++; // assign pid

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){ // returns page for the kernel stack of this process
    p->state = UNUSED; // back to unused
    return 0;
  }
  sp = p->kstack + KSTACKSIZE; // sp = one past the end of the kernel stack. Stack grows downwards

  // Leave room for trap frame - also stored on the kernel stack. Why is a trap frame required?
  sp -= sizeof *p->tf; // stack frame goes at the top of the kernel stack (highest address)
  p->tf = (struct trapframe*)sp; // a pointer to a struct is at the lowest address of the struct's memory

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret; // put the address of trapret next on the stack - 

  sp -= sizeof *p->context; // allocate space for the context struct
  p->context = (struct context*)sp; // set the context pointer to the lowest address of the context struct's memory
  memset(p->context, 0, sizeof *p->context); // set all bytes of the context struct to 0 -> trivial context
  p->context->eip = (uint)forkret; // set eip to the address of forkret -> so that on its first scheduling, it will start executing at forkret - we have got to return from the fork system call in the child, right?

  return p; // return this embryo process
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // allocproc returns an embryo process, so we need to initialize it

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf; // so that state so far is the same in the parent and in the child (note that the state is copied, so they now have two copies of old information. Note that COW is not implemented.

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  // the default name of child processes is the same as the parent
  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait() - wake it up.
  wakeup1(curproc->parent);

  // Pass abandoned children to init. Neat.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc); // wake up initproc if it is sleeping in wait()
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched(); // never to return. Because once this is switched out it will never run again since state = ZOMBIE. Nice.
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu(); // which cpu's scheduler is this?

  c->proc = 0; // pid starts at 0, rn init is to be run
  
  // infinite loop. Never return
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE) // just select the first runnable process
        continue;

      // Switch to chosen runnable process.  It is the process's job
      // to release ptable.lock (so that it can run) and then reacquire it
      // before jumping back to us.
      
      c->proc = p; // set current process on cpu to p
      switchuvm(p); // switch to user virtual memory - basically, load cr3 and a few other masks (base and bounds, for ex the bottom of the stack) into the cpu
      p->state = RUNNING; // change state to running, just before switching to it

      swtch(&(c->scheduler), p->context); // switch from scheduler process to it. Store scheduler context into c->scheduler for next time.
      // when we return here, it is because the process completed its quota/syscall/fault etc and sched() was called, switching from p to c->scheduler, which of course, returns here

      switchkvm(); // switch to kernel page table for the scheduler process

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0; // no user process running at the moment on this cpu
    }
    release(&ptable.lock);
  } 
  
  // one interesting point to note here. Notice how we were able to set 'p->state = RUNNING' after switching to p->pgdir. This would mean that p->pgdir contains references to kernel space addresses. Why not do the p->state = RUNNING before switching to p->pgdir? Well, no bit gain, since the code for swtch is also in kernel space! 
  
  // The abstraction of needing to access OS stuff while keeping the page directory of the user is thus needed - one solution (used here) is to maintain mappages of the kernel space in the user pgdir as well - and another, using more hardware support: use a cr4 register with the pa of the kernel page table - issue is how does the hardware know which one to walk when given an address? Many double walks (one successful, other unsuccessful) are required - not a good design. So we use the first solution.
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  // this function is called from three places: yield(), wait(), exit(). Its call stack thus is:
  // prev_calls (eg. eip just before alltraps -> eip just bfr trap) | wherever-it-came-from-eip | local args
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena; // save the interrupt enable flag
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here (this is the eip of the fork child that allocproc hardcodes).  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc). Hmm, how? eip is at forkret, so it's not really a function call, after this exits won't we just like go 4 bytes ahead and execute the next instruction?
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  // give up the cpu to the os to schedule something else
  sched();

  // we're back to this process - we have been woken up by wakeup1 and are ready to run

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

// world peace system call
void worldpeace(void) {
  cprintf("Systems are vital to World Peace !!\n");
}

// number of runnable process system call
int numberofprocesses(void) {
  int count = 0;
  struct proc *p;
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if(p->state == RUNNABLE) {
      count++;
    }
  }
  release(&ptable.lock);
  return count;
}

// added for whatsthestatus system call
int whatsthestatus(int pid) {
  struct proc *p;
  struct proc *in_question; // the process with the given pid
  acquire(&ptable.lock);

  // find in_question
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->pid == pid) {
      in_question = p;
      break;
    }
  }
  
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(in_question->parent == p)
      break;

  // print whatever is needed
  
  // first convert the state to string
  char *state;
  switch(in_question->state) {
    case UNUSED:
      state = "UNUSED";
      break;
    case EMBRYO:
      state = "EMBRYO";
      break;
    case SLEEPING:
      state = "SLEEPING";
      break;
    case RUNNABLE:
      state = "RUNNABLE";
      break;
    case RUNNING:
      state = "RUNNING";
      break;
    case ZOMBIE:
      state = "ZOMBIE";
      break;
    default:
      state = "UNKNOWN";
      break;
  }
  cprintf("%d %s %d %s\n", in_question->pid, state, p->pid, p->name);
  int ppid = p->pid; 
  release(&ptable.lock);
  return ppid;
}

// the spawn() system call
int spawn(int n, int *pids) {

  struct proc *parent = myproc();
  struct proc *np; // new process
  int allocated = 0;
  for (int j = 0; j < n; j++) {
    pids[j] = -1;
    // try to allocate process
    if ((np = allocproc()) == 0)
      continue;
    // try to copy page table for child process
    if ((np->pgdir = copyuvm(myproc()->pgdir, myproc()->sz)) == 0) {
      kfree(np->kstack);
      np->kstack = 0;
      np->state = UNUSED; // set it back to unused because page table allocation failed.
      continue;
    }

    // some copy business now
    np->sz = parent->sz; // same amount of memory allocated to both
    np->parent = parent;
    // *copy* trapframes - this is where child = parent so far comes in. One line of code.

    *np->tf = *parent->tf; // so that state so far is the same in the parent and in the child (note that the state is copied, so they now have two copies of old information. notice COW is not implemented).

    // copy open files and cwd (basically most of the proc struct for np)
    for (int i = 0; i < NOFILE; i++)
      if (parent->ofile[i])
        np->ofile[i] = filedup(parent->ofile[i]);
    np->cwd = idup(parent->cwd);

    // copy the name over (this is default behaviour from parent to child)
    safestrcpy(np->name, parent->name, sizeof(parent->name));

    // another important thing - setting the return value of the syscall in the child process. eax is sent as the return value to the process from return from trap
    np->tf->eax = 0;
    // we do not need to set parent's eax to the return value, doing `return ...` will set that (see usys.S)

    // set pid
    pids[j] = np->pid;
    allocated += 1;

    // set state to runnable
    acquire(&ptable.lock); // lock other processes, just for a second
    np->state = RUNNABLE;
    release(&ptable.lock);
  }
  return (!allocated)?-1:allocated;
}

// virtual to physical address
uint va2pa(uint virtual_addr) {
  // pte_t *pte = walkpgdir(myproc()->pgdir, (void *)virtual_addr, 0);
  pde_t *pde; pte_t *pte;
  pde = myproc()->pgdir;
  pde = &pde[PDX(virtual_addr)];
  pte = (pte_t*)P2V(PTE_ADDR(*pde)); // hey, wait, why is the physical address of the page table only 20 bits long? And not 32?
  // first get the 20bit mapping:
  uint pte_index = PTX(virtual_addr);
  uint pa20 = PTE_ADDR(pte[pte_index]);
  uint pa = pa20 | (virtual_addr & 0xFFF); // the last 12 bits are the offset
  return pa;
}

// get size of physical memory of process (not os) in pages
int getpasize(int pid) {
  struct proc *p;
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->pid == pid) break;
  }
  if (p == &ptable.proc[NPROC]) return -1; // no such process
  int count = 0;
  for (pde_t *pde = p->pgdir; pde < &p->pgdir[NPDENTRIES/2]; pde++) {
    if (!(*pde & PTE_P)) continue;
    pte_t *pgtable = (pte_t*)P2V(PTE_ADDR(*pde));
    for (int i = 0; i < NPTENTRIES; i++) {
      if (pgtable[i] & PTE_P) count++;
    }
  }
  return count;
}