@ Regression test: a loop made only of an indirect branch (BX to itself). The JIT used to jump
@ through its fast dispatch without checking the cycle count, so Jit::Run never returned.
@ The test passes if the runner finishes its cycle budget.
    .syntax unified
    .arch armv6k
    .arm
    .global _start
_start:
    adr r2, 1f
1:  bx r2
