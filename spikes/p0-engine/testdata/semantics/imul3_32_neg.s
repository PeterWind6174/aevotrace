.text
.globl probe
probe:
  imull $-3, %ecx, %eax
  ret
