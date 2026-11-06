.text
.globl probe
probe:
  sarb %cl, %al
  ret
