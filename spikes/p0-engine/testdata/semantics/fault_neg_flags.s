.text
.globl probe
probe:
  movl $0x11, %eax
  negl (%rdx)
  movl $456, %ebx
  ret
