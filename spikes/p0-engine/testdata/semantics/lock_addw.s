.text
.globl probe
probe:
  lock addw $1, (%rcx)
  ret
