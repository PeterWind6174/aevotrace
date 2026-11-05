.text
.globl probe
probe:
  lock xorl $7, (%rcx)
  ret
