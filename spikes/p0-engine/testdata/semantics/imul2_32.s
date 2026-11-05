.text
.globl probe
probe:
  imull %ecx, %eax
  ret
