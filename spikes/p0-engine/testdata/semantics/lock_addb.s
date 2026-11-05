.text
.globl probe
probe:
  lock addb $1, (%rcx)
  ret
