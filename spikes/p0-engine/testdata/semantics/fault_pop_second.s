.text
.globl probe
probe:
  movl $0x11, %eax
  popq %rax
  movl $456, %ebx
  ret
