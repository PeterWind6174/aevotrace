.text
.globl probe
probe:
  popq %rax
  movl $456, %ebx
  ret
