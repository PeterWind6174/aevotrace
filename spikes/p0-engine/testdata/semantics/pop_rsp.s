.text
.globl probe
probe:
  popq %rsp
  movq %rsp, %rax
  ret
