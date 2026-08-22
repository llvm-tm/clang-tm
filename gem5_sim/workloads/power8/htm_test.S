.text
.align 4
array:
    .long 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15

.globl _start
_start:
    mr 31, 1

    /* get array address using PC-relative */
    bl get_pc
get_pc:
    mflr 9
    addi 9, 9, (array - get_pc)

    li 5, 16            /* n = 16 elements */
    li 7, 0             /* sum = 0 */
    li 4, 0             /* i = 0 */

    tbegin.
    beq+ 1f
    b no_tx
1:
tx_loop:
    cmpld 4, 5
    bge tx_done
    sldi 6, 4, 2
    lwzx 8, 9, 6        /* load array[i] */
    add 7, 7, 8         /* sum += array[i] */
    addi 4, 4, 1
    b tx_loop
tx_done:
    tend.

    li 3, 0             /* r3 = 0 (success) */
    b exit

no_tx:
    li 3, 1             /* r3 = 1 (no transaction) */

exit:
    li 0, 1             /* SYS_exit */
    sc
