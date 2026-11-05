.text
.globl probe
probe:
  imulq %rcx, %rax
  ret
