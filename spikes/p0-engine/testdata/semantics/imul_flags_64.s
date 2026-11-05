.text
.globl probe
probe:
  imulq %rbx, %rax
  ret
