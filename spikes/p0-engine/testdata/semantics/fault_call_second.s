.text
.globl probe
probe:
  movl $0x11, %eax
  callq probe2
  movl $456, %ebx
  ret
probe2:
  ret
