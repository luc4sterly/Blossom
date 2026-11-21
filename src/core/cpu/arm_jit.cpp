#include <algorithm>
#include <array>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include "common/exceptions.hpp"
#include "common/profiler.hpp"
#include "core/cpu/arm.hpp"
#include "core/cpu/arm_jit.hpp"
#include "core/cpu/cp15.hpp"
#include "core/cpu/vfp.hpp"

using Dynarmic::HaltReason;
namespace A32 = Dynarmic::A32;

namespace
{

constexpr uint64_t TLB_MMIO = 1ULL << 63;
constexpr uint64_t TLB_READ = 1ULL << 62;
constexpr uint64_t TLB_WRITE = 1ULL << 61;
constexpr uint64_t TLB_EXEC = 1ULL << 60;
constexpr uint64_t TLB_FLAGS = 0xFULL << 60;

//Halt reasons Blossom uses. MemoryAbort makes dynarmic stop right after the memory access that
//requested it (check_halt_on_memory_access), with PC at the instruction that did the access.
constexpr HaltReason HALT_STOP = HaltReason::UserDefined1;
constexpr HaltReason HALT_ABORT = HaltReason::MemoryAbort;

//Only MRC p15, 0, Rt, c13, c0, {2,3,4} (thread ID registers) reaches this; everything else on
//CP15 is interpreted. Reads go straight to the CP15 registers through a pointer.
class ThreadIdCoprocessor final : public A32::Coprocessor
{
    private:
        uint32_t* regs;
    public:
        explicit ThreadIdCoprocessor(uint32_t* regs) : regs(regs) {}

        std::optional<Callback> CompileInternalOperation(bool, unsigned, A32::CoprocReg, A32::CoprocReg,
                                                         A32::CoprocReg, unsigned) override
        {
            return std::nullopt;
        }

        CallbackOrAccessOneWord CompileSendOneWord(bool, unsigned, A32::CoprocReg, A32::CoprocReg, unsigned) override
        {
            return {};
        }

        CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, A32::CoprocReg) override
        {
            return {};
        }

        CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, A32::CoprocReg CRn, A32::CoprocReg CRm,
                                                  unsigned opc2) override
        {
            if (!two && opc1 == 0 && CRn == A32::CoprocReg::C13 && CRm == A32::CoprocReg::C0 && opc2 >= 2 && opc2 <= 4)
                return &regs[opc2 - 2];
            return {};
        }

        CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, A32::CoprocReg) override
        {
            return {};
        }

        std::optional<Callback> CompileLoadWords(bool, bool, A32::CoprocReg, std::optional<std::uint8_t>) override
        {
            return std::nullopt;
        }

        std::optional<Callback> CompileStoreWords(bool, bool, A32::CoprocReg, std::optional<std::uint8_t>) override
        {
            return std::nullopt;
        }
};

}

ARM_JIT::ARM_JIT(ARM_CPU& cpu, Dynarmic::ExclusiveMonitor* monitor) : cpu(cpu)
{
    page_table = std::make_unique<JitPageTable>();
    cpu.cp15->set_jit_page_table(page_table.get());

    const bool is_arm9 = cpu.id == 9;
    legacy_alignment = is_arm9;

    A32::UserConfig conf;
    conf.callbacks = this;
    conf.arch_version = is_arm9 ? A32::ArchVersion::v5TE : A32::ArchVersion::v6K;
    if (!is_arm9 && monitor)
    {
        conf.global_monitor = monitor;
        conf.processor_id = cpu.id - 11;
    }

    conf.page_table = reinterpret_cast<std::array<std::uint8_t*, A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>*>(
        page_table->entries);
    conf.page_table_pointer_mask_bits = 2;
    conf.page_table_read_only_bit0 = true;
    //Accesses that straddle two pages go through the callbacks (the pages may not be contiguous)
    conf.detect_misaligned_access_via_page_table = 16 | 32 | 64;
    //On the ARM9 every misaligned word access needs the ARMv5 behaviour from the callbacks
    conf.only_detect_misalignment_via_page_table_on_page_boundary = !legacy_alignment;

    conf.interpret_system_instructions = true;
    conf.hook_hint_instructions = false;
    conf.check_halt_on_memory_access = true;
    conf.always_little_endian = true;
    conf.code_cache_size = 96 * 1024 * 1024;
    //Keeps blocks from overshooting the cycle budget by much (timers, other cores)
    conf.max_block_instructions = 64;

    if (!is_arm9)
    {
        cp15_thread_id = std::make_shared<ThreadIdCoprocessor>(cpu.cp15->get_thread_regs());
        conf.coprocessors[15] = cp15_thread_id;
    }

    //Debug switches to narrow down JIT bugs, as a sum of bits:
    //1 = no page table (every memory access goes through the callbacks), 2 = one instruction per
    //block, 4 = no dynarmic optimizations
    if (const char* env = getenv("BLOSSOM_JIT_SAFE"))
    {
        int safe = atoi(env);
        if (safe & 1)
            no_page_table = true;
        if (safe & 2)
            conf.max_block_instructions = 1;
        if (safe & 4)
            conf.optimizations = Dynarmic::no_optimizations;
        if (safe)
            fprintf(stderr, "[ARM%d] JIT debug switches: %d\n", cpu.id, safe);
    }

    //Block linking on the ARM11 still breaks the boot11/boot9 PXI handshake (core 0 waits in
    //boot11 for a PXI_SYNC value that never comes), so it stays off there. The ARM9 boots fine
    //with it. BLOSSOM_JIT_OPT=63 turns it on for both (about 2x faster).
    if (!is_arm9)
        conf.optimizations &= ~Dynarmic::OptimizationFlag::BlockLinking;

    //BLOSSOM_JIT_OPT=<mask>: dynarmic optimizations to keep (OptimizationFlag bits: 1 block
    //linking, 2 return stack buffer, 4 fast dispatch, 8 get/set elimination, 16 constant
    //propagation, 32 misc IR). Debug aid to find which one breaks something.
    if (const char* env = getenv("BLOSSOM_JIT_OPT"))
    {
        conf.optimizations = static_cast<Dynarmic::OptimizationFlag>(strtoul(env, nullptr, 0));
        fprintf(stderr, "[ARM%d] JIT optimizations: $%X\n", cpu.id, (unsigned)conf.optimizations);
    }

    jit = std::make_unique<A32::Jit>(conf);
    reset();
}

ARM_JIT::~ARM_JIT()
{
    cpu.cp15->set_jit_page_table(nullptr);
}

void ARM_JIT::reset()
{
    jit->Reset();
    jit->ClearCache();
    jit->ClearExclusiveState();
    page_table->invalidate_all();
    context_ids.clear();
    code_chunks.clear();
    global_context_id = ~0U;
    last_context_key = 0;
    last_context_id = ~0U;
    context = 0;

    pending_exception = nullptr;
    data_abort_pending = false;
    prefetch_abort_pending = false;
    svc_pending = false;
    irq_requested = false;
    cycle_target = 0;

    sync_vfp_to_jit();
}

void ARM_JIT::leave()
{
    sync_vfp_from_jit();
}

void ARM_JIT::request_exit()
{
    irq_requested = true;
    jit->HaltExecution(HALT_STOP);
}

void ARM_JIT::clear_cache()
{
    jit->ClearCache();
    //Pages stay read-only in the shadow table until their next slow write, which is harmless
    code_chunks.clear();
    Profiler::counters[Profiler::JIT_CACHE_CLEARS]++;
}

void ARM_JIT::update_context()
{
    uint32_t pc = cpu.gpr[15] - (cpu.CPSR.thumb ? 2 : 4);
    uint32_t key = cpu.cp15->get_address_space_key(pc);
    if (key != last_context_key || last_context_id == ~0U)
    {
        auto it = context_ids.find(key);
        if (it == context_ids.end())
        {
            //Ids are 8 bits wide. Running out means many address spaces were created over time;
            //start over with a clean cache.
            if (context_ids.size() >= 256)
            {
                clear_cache();
                context_ids.clear();
                global_context_id = ~0U;
            }
            uint32_t new_id = (uint32_t)context_ids.size();
            it = context_ids.emplace(key, new_id).first;
            if (key == 0xFFFFFFFE)
                global_context_id = new_id;
            Profiler::counters[Profiler::JIT_CONTEXTS]++;
        }
        last_context_key = key;
        last_context_id = it->second;
    }

    uint32_t c = ((last_context_id & 0xF) << 4) | ((last_context_id >> 4) << 27);
    if (!cpu.vfp || !cpu.vfp->is_enabled())
        c |= A32::Jit::CONTEXT_VFP_DISABLED;
    context = c;
}

void ARM_JIT::sync_to_jit()
{
    auto& regs = jit->Regs();
    for (int i = 0; i < 15; i++)
        regs[i] = cpu.gpr[i];
    //The interpreter keeps PC one instruction ahead (next instruction + 4 in ARM, + 2 in Thumb)
    regs[15] = cpu.gpr[15] - (cpu.CPSR.thumb ? 2 : 4);
    jit->SetCpsr(cpu.CPSR.get());
    jit->SetContext(context);
}

void ARM_JIT::sync_from_jit()
{
    const auto& regs = jit->Regs();
    for (int i = 0; i < 15; i++)
        cpu.gpr[i] = regs[i];
    cpu.CPSR.set(jit->Cpsr());
    cpu.gpr[15] = regs[15] + (cpu.CPSR.thumb ? 2 : 4);
}

void ARM_JIT::sync_vfp_to_jit()
{
    if (!cpu.vfp)
        return;
    auto& ext = jit->ExtRegs();
    for (int i = 0; i < 32; i++)
        ext[i] = cpu.vfp->get_reg32(i);
    jit->SetFpscr(cpu.vfp->get_fpscr());
}

void ARM_JIT::sync_vfp_from_jit()
{
    if (!cpu.vfp)
        return;
    const auto& ext = jit->ExtRegs();
    for (int i = 0; i < 32; i++)
        cpu.vfp->set_reg32(i, ext[i]);
    cpu.vfp->load_fpscr(jit->Fpscr());
}

void ARM_JIT::run(int cycles)
{
    cpu.cycles_ran = 0;
    cycle_target = cycles;

    update_context();
    sync_to_jit();

    while (true)
    {
        cpu.in_jit = true;
        jit->Run();
        cpu.in_jit = false;
        jit->ClearHalt(HALT_STOP | HALT_ABORT);
        Profiler::Scope prof_scope(Profiler::JIT_GLUE);

        sync_from_jit();

        if (pending_exception)
        {
            std::exception_ptr e = pending_exception;
            pending_exception = nullptr;
            data_abort_pending = prefetch_abort_pending = svc_pending = false;
            std::rethrow_exception(e);
        }

        if (data_abort_pending)
        {
            //PC is at the faulting instruction; the interpreter would be two instructions ahead
            data_abort_pending = false;
            cpu.gpr[15] += cpu.CPSR.thumb ? 2 : 4;
            cpu.data_abort(data_abort_addr, data_abort_write);
        }
        else if (prefetch_abort_pending)
        {
            prefetch_abort_pending = false;
            //The block that raised the fault must be retranslated once the page is mapped
            jit->InvalidateCacheRange(prefetch_abort_addr, 4);
            cpu.prefetch_abort(prefetch_abort_addr);
        }

        if (svc_pending)
        {
            svc_pending = false;
            cpu.swi();
        }

        //Same timing as the interpreter: an interrupt raised while this core runs is taken right
        //away, anything else that is pending (masked earlier, unmasked since) at the end of the
        //slice. Taking it as soon as CPSIE/MSR unmasks it, as before, made the kernel re-enter its
        //IRQ handler over and over before acknowledging the interrupt.
        if (irq_requested)
        {
            irq_requested = false;
            if (cpu.int_pending)
                cpu.int_check();
        }

        if (cpu.halted || cpu.cycles_ran >= cycle_target)
            break;

        update_context();
        sync_to_jit();
    }

    if (cpu.int_pending)
        cpu.int_check();
}

void ARM_JIT::on_cp15_write(uint32_t op, uint32_t value, bool mapping_changed)
{
    //The MMU was switched on or off: cached pages came from the other mapping
    if (mapping_changed)
        page_table->invalidate_all();

    switch (op)
    {
        case 0x750: //Invalidate entire instruction cache
        case 0x752: //Invalidate instruction cache line (set/way)
        case 0x770: //Invalidate both caches
            //Only code whose memory changed since it was compiled needs to go
            invalidate_changed_code();
            Profiler::counters[Profiler::JIT_CACHE_CLEARS]++;
            break;
        case 0x751: //Invalidate instruction cache line (MVA)
            jit->InvalidateCacheRangeNow(value & ~0x1FU, 32);
            Profiler::counters[Profiler::JIT_RANGE_INVALIDATIONS]++;
            break;
        default:
            break;
    }
}

void ARM_JIT::cache_page(uint32_t vaddr)
{
    if (no_page_table)
        return;
    uint32_t page = vaddr >> 12;
    uint64_t mem = (uint64_t)cpu.tlb_map[page];
    if ((mem & TLB_READ) && !(mem & TLB_MMIO))
    {
        bool read_only = !(mem & TLB_WRITE) || code_chunks.count(code_key(page));
        page_table->set(page, (uint8_t*)(mem & ~TLB_FLAGS), read_only);
    }
}

static uint32_t hash_chunk(const uint8_t* p)
{
    uint64_t h = 0xCBF29CE484222325ULL;
    for (int i = 0; i < 64; i += 8)
    {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001B3ULL;
    }
    return (uint32_t)(h ^ (h >> 32));
}

void ARM_JIT::mark_code(uint32_t vaddr, const uint8_t* host_page)
{
    uint32_t page = vaddr >> 12;
    auto it = code_chunks.find(code_key(page));
    if (it == code_chunks.end() || it->second.host != host_page)
    {
        if (it == code_chunks.end())
            it = code_chunks.emplace(code_key(page), CodePage{}).first;
        else
        {
            //Same address space, page now mapped elsewhere: what was compiled from it is stale
            jit->InvalidateCacheRangeNow(page << 12, 0x1000);
            Profiler::counters[Profiler::JIT_RANGE_INVALIDATIONS]++;
        }
        it->second.chunks = 0;
        it->second.host = host_page;
        //The page may be cached as writable; the next access caches it read-only
        page_table->invalidate_page(page);
    }
    uint32_t chunk = (vaddr >> 6) & 0x3F;
    CodePage& cp = it->second;
    if (!(cp.chunks & (1ULL << chunk)))
    {
        cp.chunks |= 1ULL << chunk;
        cp.hashes[chunk] = hash_chunk(host_page + chunk * 64);
    }
}

void ARM_JIT::invalidate_changed_code()
{
    Profiler::Scope prof_scope(Profiler::JIT_CODE_CHECK);
    for (auto it = code_chunks.begin(); it != code_chunks.end(); )
    {
        CodePage& cp = it->second;
        uint32_t page = (uint32_t)(it->first & 0xFFFFF);
        //Kernel code (TTBR1) is only ever changed by the kernel itself, and those writes are caught
        //as they happen
        if ((uint32_t)(it->first >> 20) == global_context_id)
        {
            ++it;
            continue;
        }
        for (uint32_t chunk = 0; chunk < 64; chunk++)
        {
            if (!(cp.chunks & (1ULL << chunk)))
                continue;
            if (hash_chunk(cp.host + chunk * 64) != cp.hashes[chunk])
            {
                jit->InvalidateCacheRangeNow((page << 12) + chunk * 64, 64);
                Profiler::counters[Profiler::JIT_CHANGED_CHUNKS]++;
                cp.chunks &= ~(1ULL << chunk);
            }
        }
        if (!cp.chunks)
            it = code_chunks.erase(it);
        else
            ++it;
    }
}

void ARM_JIT::check_code_write(uint32_t vaddr, uint32_t size)
{
    uint32_t last = vaddr + size - 1;
    for (uint32_t page = vaddr >> 12; page <= (last >> 12); page++)
    {
        auto it = code_chunks.find(code_key(page));
        if (it == code_chunks.end())
            continue;
        uint32_t start = std::max(vaddr, page << 12);
        uint32_t end = std::min(last, (page << 12) | 0xFFF);
        uint64_t mask = 0;
        for (uint32_t chunk = (start >> 6) & 0x3F; chunk <= ((end >> 6) & 0x3F); chunk++)
            mask |= 1ULL << chunk;
        if (it->second.chunks & mask)
        {
            //InvalidateCacheRange would only take effect once Run returns, and the block can
            //branch into the stale code before that
            jit->InvalidateCacheRangeNow(start, end - start + 1);
            it->second.chunks &= ~mask;
            Profiler::counters[Profiler::JIT_RANGE_INVALIDATIONS]++;
        }
    }
}

void ARM_JIT::record_data_abort(uint32_t vaddr, bool is_write)
{
    if (!data_abort_pending)
    {
        data_abort_pending = true;
        data_abort_addr = vaddr;
        data_abort_write = is_write;
    }
    jit->HaltExecution(HALT_ABORT);
}

void ARM_JIT::record_exception()
{
    //C++ exceptions cannot unwind through JIT code: keep it and rethrow it once Run returns
    if (!pending_exception)
        pending_exception = std::current_exception();
    jit->HaltExecution(HALT_ABORT);
}

template <typename T, typename F>
T ARM_JIT::guarded_read(uint32_t vaddr, F read)
{
    Profiler::counters[Profiler::JIT_SLOW_READS]++;
    Profiler::Scope prof_scope(Profiler::JIT_MEMORY);
    try
    {
        T value = read();
        cache_page(vaddr);
        return value;
    }
    catch (EmuException::ARMDataAbort& a)
    {
        record_data_abort(a.vaddr, false);
    }
    catch (...)
    {
        record_exception();
    }
    return 0;
}

template <typename F>
void ARM_JIT::guarded_write(uint32_t vaddr, uint32_t size, F write)
{
    Profiler::counters[Profiler::JIT_SLOW_WRITES]++;
    Profiler::Scope prof_scope(Profiler::JIT_MEMORY);
    try
    {
        write();
        if (!code_chunks.empty())
            check_code_write(vaddr, size);
        cache_page(vaddr);
    }
    catch (EmuException::ARMDataAbort& a)
    {
        record_data_abort(a.vaddr, true);
    }
    catch (...)
    {
        record_exception();
    }
}

std::optional<std::uint32_t> ARM_JIT::MemoryReadCode(A32::VAddr vaddr)
{
    Profiler::Scope prof_scope(Profiler::JIT_MEMORY);
    try
    {
        uint32_t page = vaddr >> 12;
        uint64_t mem = (uint64_t)cpu.tlb_map[page];
        if (!(mem & TLB_EXEC))
        {
            cpu.cp15->reload_tlb(vaddr);
            mem = (uint64_t)cpu.tlb_map[page];
            if (!(mem & TLB_EXEC))
                return std::nullopt;
        }
        if (mem & TLB_MMIO)
            EmuException::die("[ARM%d] PC points to MMIO $%08X", cpu.id, vaddr);
        const uint8_t* ptr = (const uint8_t*)(mem & ~TLB_FLAGS);
        uint32_t word;
        std::memcpy(&word, &ptr[vaddr & 0xFFF], sizeof(word));
        mark_code(vaddr, ptr);
        Profiler::counters[Profiler::JIT_CODE_READS]++;
        if (last_context_id == global_context_id && vaddr < cpu.cp15->get_ttbr1_boundary())
        {
            static bool warned = false;
            if (!warned)
                fprintf(stderr, "[ARM%d] JIT: kernel code branched to $%08X (TTBR0) without leaving the JIT; "
                        "it is compiled as shared code\n", cpu.id, vaddr);
            warned = true;
        }
        return word;
    }
    catch (...)
    {
        record_exception();
        return std::nullopt;
    }
}

std::uint8_t ARM_JIT::MemoryRead8(A32::VAddr vaddr)
{
    return guarded_read<uint8_t>(vaddr, [&] { return cpu.read8(vaddr); });
}

std::uint16_t ARM_JIT::MemoryRead16(A32::VAddr vaddr)
{
    return guarded_read<uint16_t>(vaddr, [&] { return cpu.read16(vaddr); });
}

std::uint32_t ARM_JIT::MemoryRead32(A32::VAddr vaddr)
{
    if (legacy_alignment && (vaddr & 0x3))
    {
        return guarded_read<uint32_t>(vaddr, [&]
        {
            uint32_t word = cpu.read32(vaddr & ~0x3U);
            int shift = (vaddr & 0x3) * 8;
            return (word >> shift) | (word << (32 - shift));
        });
    }
    return guarded_read<uint32_t>(vaddr, [&] { return cpu.read32(vaddr); });
}

std::uint64_t ARM_JIT::MemoryRead64(A32::VAddr vaddr)
{
    return guarded_read<uint64_t>(vaddr, [&] { return cpu.read64(vaddr); });
}

void ARM_JIT::MemoryWrite8(A32::VAddr vaddr, std::uint8_t value)
{
    guarded_write(vaddr, 1, [&] { cpu.write8(vaddr, value); });
}

void ARM_JIT::MemoryWrite16(A32::VAddr vaddr, std::uint16_t value)
{
    guarded_write(vaddr, 2, [&] { cpu.write16(vaddr, value); });
}

void ARM_JIT::MemoryWrite32(A32::VAddr vaddr, std::uint32_t value)
{
    if (legacy_alignment)
        vaddr &= ~0x3U;
    guarded_write(vaddr, 4, [&] { cpu.write32(vaddr, value); });
}

void ARM_JIT::MemoryWrite64(A32::VAddr vaddr, std::uint64_t value)
{
    guarded_write(vaddr, 8, [&] { cpu.write64(vaddr, value); });
}

//The global exclusive monitor already checked that this core holds the reservation; the
//write only succeeds if memory still holds the value LDREX read (cores run one at a time).
bool ARM_JIT::MemoryWriteExclusive8(A32::VAddr vaddr, std::uint8_t value, std::uint8_t expected)
{
    bool ok = false;
    guarded_write(vaddr, 1, [&] { if (cpu.read8(vaddr) == expected) { cpu.write8(vaddr, value); ok = true; } });
    return ok;
}

bool ARM_JIT::MemoryWriteExclusive16(A32::VAddr vaddr, std::uint16_t value, std::uint16_t expected)
{
    bool ok = false;
    guarded_write(vaddr, 2, [&] { if (cpu.read16(vaddr) == expected) { cpu.write16(vaddr, value); ok = true; } });
    return ok;
}

bool ARM_JIT::MemoryWriteExclusive32(A32::VAddr vaddr, std::uint32_t value, std::uint32_t expected)
{
    bool ok = false;
    guarded_write(vaddr, 4, [&] { if (cpu.read32(vaddr) == expected) { cpu.write32(vaddr, value); ok = true; } });
    return ok;
}

bool ARM_JIT::MemoryWriteExclusive64(A32::VAddr vaddr, std::uint64_t value, std::uint64_t expected)
{
    bool ok = false;
    guarded_write(vaddr, 8, [&] { if (cpu.read64(vaddr) == expected) { cpu.write64(vaddr, value); ok = true; } });
    return ok;
}

void ARM_JIT::InterpreterFallback(A32::VAddr pc, std::size_t num_instructions)
{
    (void)pc;
    (void)num_instructions; //Always 1 with interpret_system_instructions
    Profiler::counters[Profiler::JIT_FALLBACKS]++;
    fallback_count++;
    if (Profiler::enabled)
        fallback_pcs[pc]++;
    Profiler::Scope prof_scope(Profiler::JIT_FALLBACK);

    try
    {
        sync_from_jit();
        sync_vfp_from_jit();

        cpu.in_jit = false;
        cpu.interpret_one();
        cpu.in_jit = true;

        sync_vfp_to_jit();
        update_context();
        sync_to_jit();
    }
    catch (...)
    {
        cpu.in_jit = true;
        record_exception();
        return;
    }

    //WFI/WFE/a CP15 halt. A pending interrupt that the instruction unmasked waits for the end of
    //the slice, as on the interpreter.
    if (cpu.halted)
        jit->HaltExecution(HALT_STOP);
}

void ARM_JIT::print_fallback_stats()
{
    if (fallback_pcs.empty())
        return;
    std::vector<std::pair<uint32_t, uint64_t>> top(fallback_pcs.begin(), fallback_pcs.end());
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    fprintf(stderr, "[JIT] ARM%d fallbacks by PC:", cpu.id);
    for (size_t i = 0; i < top.size() && i < 8; i++)
    {
        uint32_t pc = top[i].first;
        uint64_t mem = (uint64_t)cpu.tlb_map[pc >> 12];
        uint32_t instr = 0;
        if ((mem & TLB_READ) && !(mem & TLB_MMIO))
            std::memcpy(&instr, (const uint8_t*)(mem & ~TLB_FLAGS) + (pc & 0xFFF), 4);
        fprintf(stderr, " $%08X:%08X x%llu", pc, instr, (unsigned long long)top[i].second);
    }
    fprintf(stderr, "\n");
    fallback_pcs.clear();
}

void ARM_JIT::CallSVC(std::uint32_t swi)
{
    (void)swi; //ARM_CPU::swi reads the comment field itself
    svc_pending = true;
    jit->HaltExecution(HALT_STOP);
}

void ARM_JIT::ExceptionRaised(A32::VAddr pc, A32::Exception exception)
{
    if (exception == A32::Exception::NoExecuteFault)
    {
        prefetch_abort_pending = true;
        prefetch_abort_addr = pc;
        jit->HaltExecution(HALT_STOP);
        return;
    }

    //With interpret_system_instructions every other case goes to InterpreterFallback
    try
    {
        EmuException::die("[ARM%d] Unexpected JIT exception %d at $%08X", cpu.id, (int)exception, pc);
    }
    catch (...)
    {
        record_exception();
    }
}

void ARM_JIT::AddTicks(std::uint64_t ticks)
{
    cpu.cycles_ran += (int)ticks;
}

std::uint64_t ARM_JIT::GetTicksRemaining()
{
    return cpu.cycles_ran < cycle_target ? (uint64_t)(cycle_target - cpu.cycles_ran) : 0;
}
