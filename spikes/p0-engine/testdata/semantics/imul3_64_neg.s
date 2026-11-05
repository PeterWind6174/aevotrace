.text
.globl probe
probe:
  imulq $-3, %rcx, %rax
  ret
