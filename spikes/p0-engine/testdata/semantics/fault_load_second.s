.text
.globl probe
probe:
  movl $0x11223344, %ebx
  movl (%rcx), %eax
  movl $123, (%rdx)
  ret
