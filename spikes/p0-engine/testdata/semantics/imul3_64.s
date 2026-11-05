.text
.globl probe
probe:
  imulq $7, %rcx, %rax
  ret
