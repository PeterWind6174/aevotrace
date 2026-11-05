.text
.globl probe
probe:
  imulw $-3, %cx, %ax
  ret
