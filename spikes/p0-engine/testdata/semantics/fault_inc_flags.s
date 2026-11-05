.text
.globl probe
probe:
  movl $0x11, %eax
  incl (%rdx)
  movl $456, %ebx
  ret
