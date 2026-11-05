.text
.globl probe
probe:
  pushq $-2147483648
  popq %rax
  movq %rsp, %rbx
  ret
