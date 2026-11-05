.text
.globl probe
probe:
  movl $0x11, %eax
  movl $0x22, %ecx
  movl $123, (%rdx)
  movl $456, %ebx
  ret
