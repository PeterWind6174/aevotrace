.text
.globl probe
probe:
  lock incq (%rcx)
  ret
