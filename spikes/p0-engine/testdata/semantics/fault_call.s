.text
.globl probe
probe:
  callq probe2
  movl $456, %ebx
  ret
probe2:
  ret
