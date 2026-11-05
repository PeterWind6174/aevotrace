.text
.globl probe
probe:
  movl $0x11, %eax
  addl $1, (%rdx)
  movl $456, %ebx
  ret
