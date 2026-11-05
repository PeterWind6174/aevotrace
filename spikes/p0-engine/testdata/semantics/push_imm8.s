.text
.globl probe
probe:
  pushq $-1
  popq %rax
  movq %rsp, %rbx
  ret
