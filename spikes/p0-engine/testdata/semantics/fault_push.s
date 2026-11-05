.text
.globl probe
probe:
  pushq %rax
  movl $456, %ebx
  ret
