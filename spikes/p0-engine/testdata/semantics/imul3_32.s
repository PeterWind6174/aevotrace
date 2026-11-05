.text
.globl probe
probe:
  imull $7, %ecx, %eax
  ret
