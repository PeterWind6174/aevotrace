.text
.globl probe
probe:
  movl (%rcx), %eax
  movl $123, (%rdx)
  ret
