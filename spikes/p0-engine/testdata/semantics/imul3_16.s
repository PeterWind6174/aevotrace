.text
.globl probe
probe:
  imulw $7, %cx, %ax
  ret
