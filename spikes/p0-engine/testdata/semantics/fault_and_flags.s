.text
.globl probe
probe:
  movl $0x11, %eax
  andl $0xff, (%rdx)
  movl $456, %ebx
  ret
