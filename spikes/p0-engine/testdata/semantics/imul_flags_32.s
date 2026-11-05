.text
.globl probe
probe:
  imull %ebx, %eax
  ret
