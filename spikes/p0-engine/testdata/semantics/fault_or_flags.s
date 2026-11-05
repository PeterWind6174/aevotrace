.text
.globl probe
probe:
  movl $0x11, %eax
  orl $0x100, (%rdx)
  movl $456, %ebx
  ret
