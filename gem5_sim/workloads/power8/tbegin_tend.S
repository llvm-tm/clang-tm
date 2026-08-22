.globl _start
_start:
    tbegin.
    beq+ 1f
    li 3, 1
    b 2f
1:
    li 3, 0
2:
    li 0, 1
    sc
