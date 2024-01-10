#include "mmu.h"

# so the compiled code, on syscall, had an 'int' instruction (part of the hardware isa) on a syscall. It was called on a syscall with the syscall number (or index in the interrupt vector table) passed.


# vectors.S sends all interrupts here. We are in the kernel, using the kernel stack of the process that got interrupted.
.globl alltraps # the asm version of extern

alltraps:
  # Build trap frame
  
  pushl %ds # push the global data segment
  pushl %es # push the extra data segment
  pushl %fs # push the file system data segment
  pushl %gs # push the general data segment
  pushal # push all the registers! lol wow! The registers are pushed in order - eax, ecx, edx, ebx, eip, ebp, esi, edi.

  # Set up data segments.̦
  movw $(SEG_KDATA<<3), %ax # load the kernel data segment
  movw %ax, %ds # set the data segment to the kernel data segment
  movw %ax, %es # set the extra data segment to the kernel data segment

  # Call trap(tf), where tf=%esp

  pushl %esp # push the stack pointer as the argument to the trap call - it is the virtual address of the trap frame

  # call the trap function. This is a function call, not a hardware trap. The hardware trap is the int instruction. The trap in bash is a trap command to catch signals.
  call trap 
  
  addl $4, %esp # pop the argument (tf*) to the trap call off the stack

# lol nice - the return from the trap call falls through to trapret...
.globl trapret

trapret:
  # popus registerus
  popal
  popl %gs
  popl %fs
  popl %es
  popl %ds
  addl $0x8, %esp  # trapno and errcode. Why are we popping them off? Because it is of no need to the caller - action has already been taken based off these values 

  # the reverse of the int instruction (return from interrupt)
  iret # what's iret? it's the return from interrupt instruction. it's like ret, but it pops the flags and cs as well which the int instruction pushed onto the stack
