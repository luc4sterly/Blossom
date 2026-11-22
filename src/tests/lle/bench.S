@ Simple CPU benchmark for the test runner (--cycles): checksums and copies a buffer forever.
    .syntax unified
    .arch armv6k
    .arm
    .global _start
_start:
    ldr r8, =0x00200000     @ source
    ldr r9, =0x00300000     @ destination
    mov r0, #0
1:  str r0, [r8, r0, lsl #2]
    add r0, r0, #1
    cmp r0, #4096
    bne 1b
loop:
    mov r0, #0
    mov r1, #0
2:  ldr r2, [r8, r0, lsl #2]
    add r1, r1, r2, ror #3
    eor r1, r1, r0
    tst r1, #1
    addne r1, r1, #7
    str r1, [r9, r0, lsl #2]
    add r0, r0, #1
    cmp r0, #4096
    bne 2b
    bl leaf
    b loop
leaf:
    push {r4, lr}
    mov r4, r1, lsr #4
    pop {r4, pc}
