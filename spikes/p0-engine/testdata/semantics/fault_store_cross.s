.text
.globl probe
probe:
  movl $0x12345678, (%rdx)
  ret
