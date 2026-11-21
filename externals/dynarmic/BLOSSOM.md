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
  stays compiled and goes through the configured CP15 coprocessor.
* `max_block_instructions`: caps a block's length. Without it, zero-filled memory translates into
  huge blocks of `andeq r0, r0, r0`.
* `page_table_read_only_bit0`: bit 0 of a page table entry marks the page read-only. Fast-path
  writes (`EmitVAddrLookup` with `is_write`) to such a page fall back to the write callback, which
  lets the embedder see writes to code pages and to MMU-protected pages.

These are passed through in the x64, arm64 and riscv64 address spaces.
