.text
.globl probe
probe:
  lock decq (%rcx)
  ret
