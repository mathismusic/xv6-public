# Context switch
#
#   void swtch(struct context **old, struct context *new);
# 
# Save the current registers on the stack, creating
# a struct context, and save its address in *old.
# Switch stacks to new and pop previously-saved registers.

.globl swtch
swtch:
  # store arguments in registers
  movl 4(%esp), %eax # eax = (esp+4) -> esp+4 is the first argument (**old)
  movl 8(%esp), %edx # edx = (esp+8) -> esp+8 the second argument

  # Save context of old process
  pushl %ebp # push ebp ==: sub esp, 4; mov ebp, (esp). esp points to the top (lowest address) of the stack
  pushl %ebx
  pushl %esi
  pushl %edi

  # Switch stacks
  movl %esp, (%eax) # update context of old process (update proc->context (address in eax) basically)
  movl %edx, %esp # the ingenious thing. We actually set esp = edx = new_process->context. esp satisfies the invariant that it always points to the context of the currently executing process, so yup this is the "context" switch

  # Load new callee-saved registers into edi etc. Notice the ingenuity - pop pops off of esp and modifies esp. And esp is the new context. The push and pop are actually at different locations, though it may sound symmetric.
  popl %edi
  popl %esi
  popl %ebx
  popl %ebp # 
  ret

# wait a second. This procedure does NOT respect stack is callee-saved. How will the C code after this (that assumes callee saved) work? 