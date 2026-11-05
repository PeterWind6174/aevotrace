.text
.globl probe
probe:
  lock andl $7, (%rcx)
  ret
