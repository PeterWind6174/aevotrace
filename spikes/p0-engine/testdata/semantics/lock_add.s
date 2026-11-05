.text
.globl probe
probe:
  lock addl $1, (%rcx)
  ret
