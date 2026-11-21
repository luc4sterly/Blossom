# dynarmic in Blossom

Vendored from [azahar-emu/dynarmic](https://github.com/azahar-emu/dynarmic) at `a4660158`, with
`externals/` trimmed to what the x86-64 build needs (fmt, mcl, robin-map, xbyak). Zydis is left out:
`DYNARMIC_NO_ZYDIS` turns the x64 disassembler into a stub.

The pristine import is its own commit, so `git diff <that commit> -- externals/dynarmic` shows every
Blossom change. Upstream dynarmic only models user mode; Blossom emulates the whole console (LLE), so
the patches below let the kernel and the ARM9 firmware run through it.

## Build

* `src/dynarmic/CMakeLists.txt`, `externals/CMakeLists.txt`: Zydis is optional (`DYNARMIC_NO_ZYDIS`).
  `XBYAK_CPU_CACHE=0`, because old MinGW headers lack the CPU topology fields xbyak_util.h uses.
* `common/x64_disassemble.cpp`: stub when Zydis is absent.

## Address-space context in the location descriptor

* `frontend/A32/a32_location_descriptor.h`: a `context` field held in otherwise unused bits of the
  upper descriptor (`CONTEXT_MASK = 0x780000F8`). It is kept by every `Set*`, and is part of
  `UniqueHash` and `operator==`, so blocks compiled for one context never run in another.
  Blossom stores an 8-bit address-space id (from TTBR0) and a "VFP disabled" bit (FPEXC.EN = 0).
* `backend/x64/a32_jitstate.cpp`, `backend/x64/a32_emit_x64.cpp`: SetCpsr/SetFpscr keep the context
  bits instead of clearing them.
* `interface/A32/a32.h`, `backend/x64/a32_interface.cpp`: `Jit::Context()` / `Jit::SetContext()`.

## System instructions go to the embedder

New `UserConfig` / `TranslationOptions` fields:

* `interpret_system_instructions`: every privileged or system instruction ends the block with
  `Term::Interpret`, so the embedder's interpreter runs it with full privileged state. That covers
  all coprocessor instructions (ARM and Thumb-2), MSR writing the control field, CPS, SRS, RFE,
  WFI/WFE/SEV, and every `RaiseException` other than `NoExecuteFault` (undefined and unpredictable
  encodings included, since those are often legal ARMv5/ARMv6 privileged code). With the VFP
  disabled bit set in the context, VFP instructions are interpreted too, so the kernel's lazy VFP
  switching still takes its undefined-instruction trap.
* `fast_cp15_thread_id`: with the flag above, MRC p15, 0, Rd, c13, c0, {2,3,4} (thread id registers)
  stays compiled and goes through the configured CP15 coprocessor, and the CP15 barriers (MCR
  c7,c10,4 DSB, c7,c10,5 DMB and c7,c5,4 ISB) compile to nothing.
* `max_block_instructions`: caps a block's length. Without it, zero-filled memory translates into
  huge blocks of `andeq r0, r0, r0`. A block cut by the cap ends with `LinkBlock` to the next
  instruction (upstream only sets a terminal when the loop ends for one of its own reasons).
* `page_table_read_only_bit0`: bit 0 of a page table entry marks the page read-only. Fast-path
  writes (`EmitVAddrLookup` with `is_write`) to such a page fall back to the write callback, which
  lets the embedder see writes to code pages and to MMU-protected pages.

These are passed through in the x64, arm64 and riscv64 address spaces.

## Immediate invalidation

* `Jit::InvalidateCacheRangeNow` (x64; the other backends fall back to the deferred call): unlinks
  the blocks in a range at once, so it works from a memory callback. `InvalidateCacheRange` only
  acts when `Run` returns, and a `CacheInvalidation` halt is only noticed at the end of a block, so
  code that rewrote an instruction and then branched to it ran the stale block. Blossom uses it for
  writes to pages it has compiled code from, and for CP15 "invalidate I-cache line by MVA".

## Halts seen at block links

* `backend/x64/a32_emit_x64.cpp`: with cycle counting, `LinkBlock` also checks `halt_reason` before
  jumping to the next block, as the dispatcher does. Upstream only checks the cycle count, so a halt
  requested from a memory callback (an interrupt, a deferred cache invalidation) was ignored by a
  linked loop until the slice ran out.
* `PopRSBHint` and `FastDispatchHint` terminals also check halts and the cycle count before
  jumping. Upstream jumps straight to the next block, so a guest loop made only of indirect
  branches (a BX to itself) never left `Jit::Run`.
