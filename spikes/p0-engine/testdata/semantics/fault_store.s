.text
.globl probe
probe:
  movl $0x55, %eax
  movl $123, (%rdx)
  movl $456, %ebx
  ret
