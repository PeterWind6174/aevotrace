.text
.globl probe
probe:
  imull $7, %ecx, %eax
  addl $1, (%rdx)
  movl $456, %ebx
  ret
