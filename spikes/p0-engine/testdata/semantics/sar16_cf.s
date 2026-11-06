.text
.globl probe
probe:
  sarw %cl, %ax
  jc 1f
  movl $0, %eax
  ret
1:
  movl $1, %eax
  ret
