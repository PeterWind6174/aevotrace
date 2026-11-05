.text
.globl probe
probe:
  movl $0x11, %eax
  xorl $0x55, (%rdx)
  movl $456, %ebx
  ret
