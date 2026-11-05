.text
.globl probe
probe:
  lock orl $7, (%rcx)
  ret
