.text
.globl probe
probe:
  lock subl $1, (%rcx)
  ret
