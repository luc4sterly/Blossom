#ifndef ARM_JIT_HPP
#define ARM_JIT_HPP
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <unordered_map>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>

#include "core/cpu/page_regions.hpp"

class ARM_CPU;

/***
 * dynarmic backend for one ARM core (ARM11 MPCore or ARM946).
 *
 * dynarmic only models user mode, so everything privileged runs on Corgi's interpreter through
 * InterpreterFallback: coprocessor accesses (CP15/MMU/caches, VFP system registers), MSR to the
 * control field, CPS/SRS/RFE, exception returns (MOVS pc / LDM ^), WFI/WFE/SEV, and every
 * undefined/unpredictable encoding. Exception entry (SVC, IRQ, aborts) is done by ARM_CPU on the
 * synced state between calls to Jit::Run, so banked registers stay in ARM_CPU.
 *
 * Compiled code is keyed by a context stored in dynarmic's location descriptor (Blossom patch):
 * the translation table base (one id per address space) and whether VFP is enabled (FPEXC.EN),
 * so processes never run each other's code and the kernel's lazy VFP switching still traps.
 ***/
class ARM_JIT final : public Dynarmic::A32::UserCallbacks
{
    public:
        ARM_JIT(ARM_CPU& cpu, Dynarmic::ExclusiveMonitor* monitor);
        ~ARM_JIT() override;

        void run(int cycles);
        void reset();

        //Copies VFP state out of the JIT before the core goes back to the interpreter
        void leave();

        //Asks Jit::Run to return as soon as possible (pending interrupt)
        void request_exit();

        //Called after the interpreter executed an MCR to CP15. op = (CRn << 8) | (CRm << 4) | opc2
        void on_cp15_write(uint32_t op, uint32_t value, bool mapping_changed);

        uint64_t take_fallback_count() { uint64_t n = fallback_count; fallback_count = 0; return n; }

        // dynarmic callbacks
        std::optional<std::uint32_t> MemoryReadCode(Dynarmic::A32::VAddr vaddr) override;

        std::uint8_t MemoryRead8(Dynarmic::A32::VAddr vaddr) override;
        std::uint16_t MemoryRead16(Dynarmic::A32::VAddr vaddr) override;
        std::uint32_t MemoryRead32(Dynarmic::A32::VAddr vaddr) override;
        std::uint64_t MemoryRead64(Dynarmic::A32::VAddr vaddr) override;

        void MemoryWrite8(Dynarmic::A32::VAddr vaddr, std::uint8_t value) override;
        void MemoryWrite16(Dynarmic::A32::VAddr vaddr, std::uint16_t value) override;
        void MemoryWrite32(Dynarmic::A32::VAddr vaddr, std::uint32_t value) override;
        void MemoryWrite64(Dynarmic::A32::VAddr vaddr, std::uint64_t value) override;

        bool MemoryWriteExclusive8(Dynarmic::A32::VAddr vaddr, std::uint8_t value, std::uint8_t expected) override;
        bool MemoryWriteExclusive16(Dynarmic::A32::VAddr vaddr, std::uint16_t value, std::uint16_t expected) override;
        bool MemoryWriteExclusive32(Dynarmic::A32::VAddr vaddr, std::uint32_t value, std::uint32_t expected) override;
        bool MemoryWriteExclusive64(Dynarmic::A32::VAddr vaddr, std::uint64_t value, std::uint64_t expected) override;

        void InterpreterFallback(Dynarmic::A32::VAddr pc, std::size_t num_instructions) override;
        void CallSVC(std::uint32_t swi) override;
        void ExceptionRaised(Dynarmic::A32::VAddr pc, Dynarmic::A32::Exception exception) override;

        void AddTicks(std::uint64_t ticks) override;
        std::uint64_t GetTicksRemaining() override;
    private:
        ARM_CPU& cpu;
        //ARMv5 (ARM9): unaligned LDR reads the aligned word and rotates it, STR ignores bits 1:0
        bool legacy_alignment;
        std::unique_ptr<JitPageTable> page_table;
        std::shared_ptr<Dynarmic::A32::Coprocessor> cp15_thread_id;
        std::unique_ptr<Dynarmic::A32::Jit> jit;

        int cycle_target;
        uint64_t fallback_count = 0;
        //BLOSSOM_JIT_SAFE bit 0: no page table, every access goes through the callbacks
        bool no_page_table = false;

        //Things that happened inside Run and are handled once it returns
        std::exception_ptr pending_exception;
        bool data_abort_pending;
        uint32_t data_abort_addr;
        bool data_abort_write;
        bool prefetch_abort_pending;
        uint32_t prefetch_abort_addr;
        bool svc_pending;

        //Address space ids: translation table base (or "MMU off") -> small id in the context
        std::unordered_map<uint32_t, uint32_t> context_ids;
        uint32_t last_context_key;
        uint32_t last_context_id;
        uint32_t context;

        //Self-modifying code: 64-byte chunks of each page that compiled code was read from. Pages
        //with code are read-only in the shadow page table, so writes reach the callbacks, and a
        //write to a chunk with code invalidates the blocks that cover it. The ARM11 kernel and the
        //boot ROMs write code with the caches off and no cache maintenance, so I-cache operations
        //alone are not enough.
        std::unordered_map<uint32_t, uint64_t> code_chunks;

        void update_context();
        void sync_to_jit();
        void sync_from_jit();
        void sync_vfp_to_jit();
        void sync_vfp_from_jit();
        void clear_cache();

        void cache_page(uint32_t vaddr);
        void mark_code(uint32_t vaddr);
        void check_code_write(uint32_t vaddr, uint32_t size);
        void record_data_abort(uint32_t vaddr, bool is_write);
        void record_exception();

        template <typename T, typename F>
        T guarded_read(uint32_t vaddr, F read);
        template <typename F>
        void guarded_write(uint32_t vaddr, uint32_t size, F write);
};

#endif // ARM_JIT_HPP
