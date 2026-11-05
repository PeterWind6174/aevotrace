.text
.globl probe
probe:
  movl $0x11, %eax
  pushq %rax
  movl $456, %ebx
  ret
