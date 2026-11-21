@ Blossom LLE CPU test (ARM11 MPCore, ARMv6K + VFPv2).
@ Runs with the MMU off at first, loaded at physical 0x00100000 with FCRAM mapped at 0.
@ Checks the system-level behaviour the JIT hands to the interpreter: exception entry/return,
@ banked registers, CP15 (thread ID, MMU, I-cache), lazy VFP enable (FPEXC.EN), address space
@ switches with the same virtual code address, data aborts, self-modifying code and LDREX/STREX.
@ Output goes through SWI $FF (one character in r0), which the test runner prints.
@ Build: see build.sh next to this file.

    .syntax unified
    .arch armv6k
    .fpu vfpv2
    .arm

    .equ STACK_TOP,     0x00800000
    .equ TABLE1,        0x00400000      @ L1 translation tables (16 KB aligned)
    .equ TABLE2,        0x00404000
    .equ FUNC_A_PA,     0x00500000      @ "mov r0, #1; bx lr", seen at VA 0x40000000 through TABLE1
    .equ FUNC_B_PA,     0x00600000      @ "mov r0, #2; bx lr", seen at VA 0x40000000 through TABLE2
    .equ SMC_FUNC,      0x00700000      @ patched at run time
    .equ ALIAS_VA,      0x50000000      @ maps to physical 0x00700000 (data alias test)
    .equ UNMAPPED_VA,   0x60000000

    .section .text.start, "ax"
    .global _start
_start:
    @ Supervisor mode, IRQ/FIQ off
    cpsid if, #0x13
    ldr sp, =STACK_TOP

    @ Stacks for the other modes
    cps #0x17               @ abort
    ldr sp, =STACK_TOP - 0x1000
    cps #0x1B               @ undefined
    ldr sp, =STACK_TOP - 0x2000
    cps #0x12               @ IRQ
    ldr sp, =STACK_TOP - 0x3000
    cps #0x1F               @ system
    ldr sp, =STACK_TOP - 0x4000
    cps #0x13

    @ Exception vectors at 0 (low vectors): ldr pc, [pc, #0x18]
    mov r0, #0
    ldr r1, =0xE59FF018
    mov r2, #8
1:  str r1, [r0], #4
    subs r2, r2, #1
    bne 1b
    ldr r1, =vector_table
    mov r0, #0x20
    mov r2, #8
1:  ldr r3, [r1], #4
    str r3, [r0], #4
    subs r2, r2, #1
    bne 1b

    bl test_svc
    bl test_banked
    bl test_vfp
    bl test_tls
    bl test_mmu
    bl test_asid
    bl test_abort
    bl test_smc
    bl test_smc_nocache
    bl test_excl
    bl test_long

    ldr r0, =str_done
    bl print
hang:
    b hang

@ ---------------------------------------------------------------------------------------------
@ print: r0 = NUL-terminated string. Clobbers r0-r1.
print:
    mov r1, r0
1:  ldrb r0, [r1], #1
    cmp r0, #0
    bxeq lr
    svc #0xFF
    b 1b

@ result: r0 = test name, r1 = nonzero if the test passed
result:
    push {r4, lr}
    mov r4, r1
    bl print
    cmp r4, #0
    ldrne r0, =str_ok
    ldreq r0, =str_bad
    bl print
    pop {r4, pc}

@ ---------------------------------------------------------------------------------------------
@ SVC from user mode: the handler runs in supervisor mode with SPSR = user, adds 1 to r0 and
@ returns with MOVS pc, lr.
test_svc:
    push {r4-r6, lr}
    mov r5, sp
    mrs r6, cpsr
    cps #0x10               @ user
    mov r0, #41
    svc #0x12
    mov r4, r0
    mrs r0, cpsr
    and r0, r0, #0x1F
    svc #0x13               @ back to supervisor (handler sets SPSR mode to SVC)
    mov sp, r5
    cmp r0, #0x10           @ we were in user mode between the two calls
    cmpeq r4, #42
    ldreq r1, =svc_spsr_seen
    ldreq r1, [r1]
    andeq r1, r1, #0x1F
    cmpeq r1, #0x10
    moveq r1, #1
    movne r1, #0
    ldr r0, =str_svc
    bl result
    pop {r4-r6, pc}

svc_handler:
    push {r1-r2}
    ldr r1, [lr, #-4]
    and r1, r1, #0xFF
    mrs r2, spsr
    cmp r1, #0x12
    ldreq r1, =svc_spsr_seen
    streq r2, [r1]
    addeq r0, r0, #1
    beq 2f
    cmp r1, #0x13           @ return to the caller in supervisor mode
    biceq r2, r2, #0x1F
    orreq r2, r2, #0x13
    msreq spsr_cxsf, r2
2:  pop {r1-r2}
    movs pc, lr

@ ---------------------------------------------------------------------------------------------
@ Banked SP/LR survive mode switches (CPS and MSR).
test_banked:
    push {r4-r7, lr}
    mov r7, sp
    cps #0x12
    mov r4, sp
    ldr sp, =0x11112222
    mov lr, #0x33
    cps #0x17
    mov r6, sp
    ldr sp, =0x44445555
    mov r5, sp
    mov sp, r6              @ keep the abort stack usable
    cmp r5, sp
    beq banked_fail
    mrs r0, cpsr
    bic r0, r0, #0x1F
    orr r0, r0, #0x12       @ IRQ mode through MSR
    msr cpsr_c, r0
    mov r5, sp
    mov r6, lr
    mov sp, r4
    cps #0x13
    mov sp, r7
    ldr r0, =0x11112222
    cmp r5, r0
    b banked_check
banked_fail:
    cps #0x13
    mov sp, r7
    mov r5, #0
banked_check:
    ldr r0, =0x11112222
    cmp r5, r0
    cmpeq r6, #0x33
    moveq r1, #1
    movne r1, #0
    ldr r0, =str_banked
    bl result
    pop {r4-r7, pc}

@ ---------------------------------------------------------------------------------------------
@ Undefined handler: a VFP instruction with FPEXC.EN = 0 enables VFP and retries it (lazy VFP
@ context switching, like the 3DS kernel does); anything else is skipped.
und_handler:
    push {r0-r1}
    ldr r0, =und_count
    ldr r1, [r0]
    add r1, r1, #1
    str r1, [r0]
    ldr r0, [lr, #-4]
    and r0, r0, #0x0E00
    cmp r0, #0x0A00         @ coprocessor 10/11
    bne 1f
    vmrs r0, fpexc
    tst r0, #0x40000000
    bne 1f
    orr r0, r0, #0x40000000
    vmsr fpexc, r0
    pop {r0-r1}
    subs pc, lr, #4         @ retry
1:  pop {r0-r1}
    movs pc, lr             @ skip

@ ---------------------------------------------------------------------------------------------
@ Lazy VFP: with FPEXC.EN = 0 the first VFP instruction traps, then runs; disabling it again must
@ trap again even though that code already ran with VFP enabled.
test_vfp:
    push {r4, lr}
    ldr r0, =und_count
    mov r1, #0
    str r1, [r0]
    mov r0, #0
    vmsr fpexc, r0          @ (EN = 0; this is a system register write, it never traps)
    bl vfp_add
    mov r4, r0
    mov r0, #0
    vmsr fpexc, r0
    bl vfp_add
    ldr r1, =0x40400000     @ 1.0f + 2.0f
    cmp r0, r1
    cmpeq r4, r1
    ldreq r0, =und_count
    ldreq r0, [r0]
    cmpeq r0, #2
    moveq r1, #1
    movne r1, #0
    ldr r0, =str_vfp
    bl result
    pop {r4, pc}

vfp_add:
    ldr r0, =0x3F800000
    ldr r1, =0x40000000
    vmov s0, r0             @ traps the first time when EN = 0
    vmov s1, r1
    vadd.f32 s2, s0, s1
    vmov r0, s2
    bx lr

@ ---------------------------------------------------------------------------------------------
@ CP15 thread ID registers (the JIT reads c13 straight from CP15 state)
test_tls:
    push {lr}
    ldr r0, =0x1234ABCD
    mcr p15, 0, r0, c13, c0, 3
    ldr r0, =0x0BADF00D
    mcr p15, 0, r0, c13, c0, 2
    mrc p15, 0, r1, c13, c0, 3
    mrc p15, 0, r2, c13, c0, 2
    ldr r0, =0x1234ABCD
    cmp r1, r0
    ldreq r0, =0x0BADF00D
    cmpeq r2, r0
    moveq r1, #1
    movne r1, #0
    ldr r0, =str_tls
    bl result
    pop {pc}

@ ---------------------------------------------------------------------------------------------
@ Build two translation tables: identity-mapped sections for the first 16 MB, ALIAS_VA -> 0x00700000,
@ and VA 0x40000000 -> FUNC_A_PA (TABLE1) or FUNC_B_PA (TABLE2). Then enable the MMU on TABLE1.
@ Section descriptor: PA | AP=3 (bits 10-11) | type 2
    .equ SECTION, 0xC02

test_mmu:
    push {r4-r6, lr}
    @ Clear both tables
    ldr r0, =TABLE1
    mov r1, #0
    mov r2, #0x8000
1:  str r1, [r0], #4
    subs r2, r2, #4
    bne 1b
    @ Identity map 0-16 MB in both
    ldr r4, =TABLE1
    ldr r5, =TABLE2
    mov r0, #0
    ldr r3, =SECTION
2:  orr r1, r3, r0, lsl #20
    str r1, [r4, r0, lsl #2]
    str r1, [r5, r0, lsl #2]
    add r0, r0, #1
    cmp r0, #16
    bne 2b
    @ Alias
    ldr r1, =(0x00700000 | SECTION)
    ldr r0, =(ALIAS_VA >> 20)
    str r1, [r4, r0, lsl #2]
    str r1, [r5, r0, lsl #2]
    @ Same VA, different code
    ldr r0, =(0x40000000 >> 20)
    ldr r1, =(FUNC_A_PA | SECTION)
    str r1, [r4, r0, lsl #2]
    ldr r1, =(FUNC_B_PA | SECTION)
    str r1, [r5, r0, lsl #2]
    @ The two functions
    ldr r0, =FUNC_A_PA
    ldr r1, =0xE3A00001     @ mov r0, #1
    ldr r2, =0xE12FFF1E     @ bx lr
    stmia r0, {r1, r2}
    ldr r0, =FUNC_B_PA
    ldr r1, =0xE3A00002     @ mov r0, #2
    stmia r0, {r1, r2}

    @ TTBCR = 0, TTBR0 = TABLE1, all domains manager, enable the MMU
    mov r0, #0
    mcr p15, 0, r0, c2, c0, 2
    ldr r0, =TABLE1
    mcr p15, 0, r0, c2, c0, 0
    mvn r0, #0
    mcr p15, 0, r0, c3, c0, 0
    mov r0, #0
    mcr p15, 0, r0, c8, c7, 0           @ invalidate TLB
    mrc p15, 0, r0, c1, c0, 0
    orr r0, r0, #1
    mcr p15, 0, r0, c1, c0, 0

    @ Write through the alias, read through the identity mapping
    ldr r0, =ALIAS_VA + 0x100
    ldr r1, =0xCAFEBABE
    str r1, [r0]
    ldr r0, =0x00700100
    ldr r2, [r0]
    cmp r1, r2
    moveq r1, #1
    movne r1, #0
    ldr r0, =str_mmu
    bl result
    pop {r4-r6, pc}

@ ---------------------------------------------------------------------------------------------
@ Same virtual code address in two address spaces must run the code of the current one.
test_asid:
    push {r4-r6, lr}
    mov r6, #0x40000000
    blx r6                  @ TABLE1 -> 1
    mov r4, r0
    ldr r0, =TABLE2
    bl switch_table
    blx r6                  @ TABLE2 -> 2
    mov r5, r0
    ldr r0, =TABLE1
    bl switch_table
    blx r6                  @ TABLE1 again -> 1
    cmp r4, #1
    cmpeq r5, #2
    cmpeq r0, #1
    moveq r1, #1
    movne r1, #0
    ldr r0, =str_asid
    bl result
    pop {r4-r6, pc}

switch_table:
    mcr p15, 0, r0, c2, c0, 0
    mov r0, #0
    mcr p15, 0, r0, c8, c7, 0           @ invalidate TLB
    bx lr

@ ---------------------------------------------------------------------------------------------
@ Data abort on an unmapped address: the handler records the fault address and skips the access.
test_abort:
    push {lr}
    ldr r0, =abort_addr
    mov r1, #0
    str r1, [r0]
    ldr r0, =UNMAPPED_VA + 0x24
    mov r2, #7
    ldr r2, [r0]            @ aborts; r2 keeps 7
    ldr r0, =abort_addr
    ldr r0, [r0]
    ldr r1, =UNMAPPED_VA + 0x24
    cmp r0, r1
    cmpeq r2, #7
    moveq r1, #1
    movne r1, #0
    ldr r0, =str_abort
    bl result
    pop {pc}

dabort_handler:
    push {r0-r1}
    mrc p15, 0, r0, c6, c0, 0           @ fault address
    ldr r1, =abort_addr
    str r0, [r1]
    pop {r0-r1}
    subs pc, lr, #4         @ skip the aborted instruction

@ ---------------------------------------------------------------------------------------------
@ Self-modifying code: patch an instruction, invalidate the I-cache, run the new code.
test_smc:
    push {r4-r5, lr}
    ldr r5, =SMC_FUNC
    ldr r0, =0xE3A00005     @ mov r0, #5
    ldr r1, =0xE12FFF1E     @ bx lr
    stmia r5, {r0, r1}
    mov r0, #0
    mcr p15, 0, r0, c7, c5, 0
    blx r5
    mov r4, r0
    ldr r0, =0xE3A00006     @ mov r0, #6
    str r0, [r5]
    mov r0, #0
    mcr p15, 0, r0, c7, c10, 4          @ drain write buffer
    mcr p15, 0, r0, c7, c5, 0           @ invalidate entire I-cache
    blx r5
    cmp r4, #5
    cmpeq r0, #6
    moveq r1, #1
    movne r1, #0
    ldr r0, =str_smc
    bl result
    pop {r4-r5, pc}

@ ---------------------------------------------------------------------------------------------
@ Code rewritten with no cache maintenance (the caches are off, as in the boot ROMs and early
@ kernel): the next call has to run the new instruction.
test_smc_nocache:
    push {r4-r5, lr}
    ldr r5, =SMC_FUNC + 0x100
    ldr r0, =0xE3A00007     @ mov r0, #7
    ldr r1, =0xE12FFF1E     @ bx lr
    stmia r5, {r0, r1}
    mov r0, #0
    mcr p15, 0, r0, c7, c5, 0
    blx r5
    mov r4, r0
    ldr r0, =0xE3A00008     @ mov r0, #8
    str r0, [r5]
    blx r5
    cmp r4, #7
    cmpeq r0, #8
    moveq r1, #1
    movne r1, #0
    ldr r0, =str_smc2
    bl result
    pop {r4-r5, pc}

@ ---------------------------------------------------------------------------------------------
@ LDREX/STREX: a clean pair succeeds, a STREX after CLREX fails.
test_excl:
    push {lr}
    ldr r0, =excl_word
    mov r1, #10
    str r1, [r0]
    ldrex r1, [r0]
    add r1, r1, #1
    strex r2, r1, [r0]      @ r2 = 0 (success)
    ldrex r1, [r0]
    clrex
    add r1, r1, #1
    strex r3, r1, [r0]      @ r3 = 1 (failure)
    ldr r1, [r0]
    cmp r1, #11
    cmpeq r2, #0
    cmpeq r3, #1
    moveq r1, #1
    movne r1, #0
    ldr r0, =str_excl
    bl result
    pop {pc}

@ ---------------------------------------------------------------------------------------------
unused_handler:
    b unused_handler

@ ---------------------------------------------------------------------------------------------
@ A straight run of unconditional instructions longer than the JIT's block cap: the block has to
@ end and continue in the next one.
test_long:
    push {lr}
    mov r1, #0
    .rept 200
    add r1, r1, #1
    .endr
    cmp r1, #200
    movne r1, #0
    ldr r0, =str_long
    bl result
    pop {pc}

    .ltorg

vector_table:
    .word unused_handler    @ reset
    .word und_handler
    .word svc_handler
    .word unused_handler    @ prefetch abort
    .word dabort_handler
    .word unused_handler
    .word unused_handler    @ IRQ
    .word unused_handler    @ FIQ

str_ok:     .asciz "OK "
str_bad:    .asciz "BAD "
str_svc:    .asciz "SVC "
str_banked: .asciz "BANKED "
str_vfp:    .asciz "VFP "
str_tls:    .asciz "TLS "
str_mmu:    .asciz "MMU "
str_asid:   .asciz "ASID "
str_abort:  .asciz "ABORT "
str_smc:    .asciz "SMC "
str_smc2:   .asciz "SMC2 "
str_excl:   .asciz "EXCL "
str_long:   .asciz "LONG "
str_done:   .asciz "DONE"
    .balign 4

    .section .data
    .balign 4
svc_spsr_seen:  .word 0
und_count:      .word 0
abort_addr:     .word 0
excl_word:      .word 0
