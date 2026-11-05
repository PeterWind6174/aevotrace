.text
.globl probe
probe:
  lock addq $1, (%rcx)
  ret
