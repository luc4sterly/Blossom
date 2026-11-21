#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <sstream>
#include <utility>
#include "core/cpu/arm.hpp"
#include "core/cpu/arm_disasm.hpp"
#include "core/cpu/arm_interpret.hpp"
#include "core/cpu/vfp.hpp"
#include "common/common.hpp"
#ifdef BLOSSOM_HAS_JIT
#include "core/cpu/arm_jit.hpp"
#endif
#include "core/emulator.hpp"

//The SVC/IPC trace reads guest memory on every SVC and floods stdout, which ends up costing more
//than the JIT saves. Enable it with BLOSSOM_TRACE_SVC=1.
//BLOSSOM_TRACE_SVC=2 prints no per-SVC lines; instead, every 60 frames, each process's SVCs and
//service requests are counted on one line on stderr (see ARM_CPU::print_svc_stats). Small enough to share.
static int trace_svc_level()
{
    static const int level = [] {
        const char* env = getenv("BLOSSOM_TRACE_SVC");
        return env ? atoi(env) : 0;
    }();
    return level;
}

//BLOSSOM_SKIP_SETUP=1: when a process reads cfg block 0x00110000 (system setup required) or
//0x000D0000 (accepted EULA version), the reply is patched to "setup done" and EULA 0x7F7F, as
//Azahar does, so NS boots the HOME Menu instead of System Settings' first-time setup.
static bool skip_setup_enabled()
{
    static const bool enabled = [] {
        const char* env = getenv("BLOSSOM_SKIP_SETUP");
        return env && env[0] && env[0] != '0';
    }();
    return enabled;
}

static bool trace_svc_enabled()
{
    return trace_svc_level() >= 1 || skip_setup_enabled();
}

static bool trace_svc_lines()
{
    return trace_svc_level() == 1;
}

struct CfgRequest { uint32_t block, size; };
static std::deque<CfgRequest> cfg_requests;

//(pid, name) -> (what, count)
static std::map<std::pair<int, std::string>, std::map<std::string, uint64_t>> svc_stats;

static void count_svc(int pid, const std::string& procname, const std::string& what)
{
    if (trace_svc_level() >= 2)
        svc_stats[{pid, procname}][what]++;
}

void ARM_CPU::print_svc_stats(int frame)
{
    if (trace_svc_level() < 2 || frame % 60)
        return;
    for (auto& [proc, counts] : svc_stats)
    {
        fprintf(stderr, "[SVCSTAT] frame %d PID%d[%s]:", frame, proc.first, proc.second.c_str());
        for (auto& [what, n] : counts)
            fprintf(stderr, " %s x%llu", what.c_str(), (unsigned long long)n);
        fprintf(stderr, "\n");
    }
    svc_stats.clear();
}

//BLOSSOM_TRACE_MMIO=1 logs every I/O register access by the ARM11 cores to stderr (physical
//address). Accesses repeated in a polling loop are counted instead of printed. Both CPU backends go
//through here, so interpreter and JIT logs can be compared line by line. The ARM9 is left out on
//purpose: it writes console keys to the AES engine, and these logs get shared.
static int trace_mmio_level()
{
    static const int level = [] {
        const char* env = getenv("BLOSSOM_TRACE_MMIO");
        return env ? atoi(env) : 0;
    }();
    return level;
}

static bool trace_mmio_enabled()
{
    return trace_mmio_level() >= 1;
}

//BLOSSOM_TRACE_MMIO=2 also logs every ARM11 write to memory (virtual address, lower-case 'w').
//JIT writes that hit the page table skip ARM_CPU::write*, so use it with BLOSSOM_JIT_SAFE=1.
static bool trace_writes_enabled()
{
    return trace_mmio_level() >= 2;
}

static void trace_mmio(int id, char kind, int bits, uint32_t addr, uint32_t value)
{
    //Accesses already seen recently with the same value (a polling loop of up to 8 accesses)
    //are only counted
    struct Access { char kind; int bits; uint32_t addr, value; };
    struct History { Access recent[8]; int count = 0, next = 0; uint64_t skipped = 0; };
    static History history[16];
    static uint64_t lines = 0;
    const uint64_t MAX_LINES = 2000000;

    History& h = history[id & 0xF];
    for (int i = 0; i < h.count; i++)
    {
        const Access& r = h.recent[i];
        if (r.kind == kind && r.bits == bits && r.addr == addr && r.value == value)
        {
            h.skipped++;
            return;
        }
    }
    h.recent[h.next] = {kind, bits, addr, value};
    h.next = (h.next + 1) % 8;
    if (h.count < 8)
        h.count++;

    if (lines >= MAX_LINES)
        return;
    if (h.skipped)
    {
        fprintf(stderr, "[MMIO] ARM%d   (%llu repeated accesses)\n", id, (unsigned long long)h.skipped);
        h.skipped = 0;
        lines++;
    }
    fprintf(stderr, "[MMIO] ARM%d %c%d $%08X = $%0*X\n", id, kind, bits, addr, bits / 4, value);
    lines++;
    if (lines == MAX_LINES)
        fprintf(stderr, "[MMIO] line limit reached, tracing stops\n");
}

uint64_t ARM_CPU::global_exclusive_start[4];
uint64_t ARM_CPU::global_exclusive_end[4];

void get_hos_process_info(ARM_CPU& cpu, int& pid, std::string& name)
{
    //This is kinda complicated. The PID and Codeset pointer are stored in different locations in KProcess
    //depending on kernel version. There's not a good way to detect this other than trying all possible locations
    //until we find one that doesn't data abort.
    static int offs = 16;
    uint32_t process_ptr;
    try
    {
        process_ptr = cpu.read32(0xFFFF9004);
    }
    catch (const EmuException::ARMDataAbort&)
    {
        process_ptr = 0;
    }
    if (!process_ptr)
        return;
    while (offs >= 0)
    {
        try
        {
            name = "";
            pid = cpu.read32(process_ptr + 0xAC + offs);
            uint32_t codeset_ptr = cpu.read32(process_ptr + 0xA8 + offs);
            for (int i = 0; i < 8; i++)
                name += (char)cpu.read8(codeset_ptr + 0x50 + i);
            return;
        }
        catch (const EmuException::ARMDataAbort&)
        {
            offs -= 8;
            if (offs < 0)
            {
                EmuException::die("[ARM] Unable to find process info!");
            }
        }
    }
}

uint32_t PSR_Flags::get()
{
    uint32_t reg = 0;
    reg |= negative << 31;
    reg |= zero << 30;
    reg |= carry << 29;
    reg |= overflow << 28;
    reg |= q_overflow << 27;
    for (int i = 0; i < 4; i++)
        reg |= ge[i] << (16 + i);

    reg |= irq_disable << 7;
    reg |= fiq_disable << 6;
    reg |= thumb << 5;

    reg |= mode;
    return reg;
}

void PSR_Flags::set(uint32_t value)
{
    negative = value & (1 << 31);
    zero = value & (1 << 30);
    carry = value & (1 << 29);
    overflow = value & (1 << 28);
    q_overflow = value & (1 << 27);
    for (int i = 0; i < 4; i++)
        ge[i] = value & (1 << (16 + i));

    irq_disable = value & (1 << 7);
    fiq_disable = value & (1 << 6);
    thumb = value & (1 << 5);

    mode = (PSR_MODE)(value & 0x1F);
}

ARM_CPU::ARM_CPU(Emulator* e, int id, CP15* cp15, VFP* vfp) : e(e), id(id), cp15(cp15), vfp(vfp)
{
    in_jit = false;
    test_console = false;
    debug_cycles = 0;
}

ARM_CPU::~ARM_CPU() = default;

bool ARM_CPU::jit_available()
{
#ifdef BLOSSOM_HAS_JIT
    return true;
#else
    return false;
#endif
}

bool ARM_CPU::set_jit_enabled(bool enabled, Dynarmic::ExclusiveMonitor* monitor)
{
#ifdef BLOSSOM_HAS_JIT
    if (!enabled)
    {
        if (jit)
            jit->leave();
        jit.reset();
        return true;
    }
    if (!jit)
        jit = std::make_unique<ARM_JIT>(*this, monitor);
    return true;
#else
    (void)monitor;
    return !enabled;
#endif
}

bool ARM_CPU::is_jit_enabled() const
{
    return jit != nullptr;
}

void ARM_CPU::take_debug_stats(uint64_t& fallbacks, uint64_t& cycles)
{
    fallbacks = 0;
#ifdef BLOSSOM_HAS_JIT
    if (jit)
        fallbacks = jit->take_fallback_count();
#endif
    cycles = debug_cycles;
    debug_cycles = 0;
}

void ARM_CPU::print_jit_stats()
{
#ifdef BLOSSOM_HAS_JIT
    if (jit)
        jit->print_fallback_stats();
#endif
}

void ARM_CPU::set_test_console(bool enabled)
{
    test_console = enabled;
}

std::string ARM_CPU::get_reg_name(int id)
{
    if (id < 10)
    {
        std::stringstream output;
        output << "r" << id;
        return output.str();
    }
    else
    {
        switch (id)
        {
            case 10:
                return "sl";
            case 11:
                return "fp";
            case 12:
                return "ip";
            case 13:
                return "sp";
            case 14:
                return "lr";
            case 15:
                return "pc";
        }
    }
    return "??";
}

void ARM_CPU::reset()
{
    CPSR.mode = PSR_SUPERVISOR;
    CPSR.fiq_disable = true;
    CPSR.irq_disable = true;

    tlb_map = cp15->get_tlb_mapping();

    if (id == 9)
        jp(0xFFFF0000, true);
    else
        jp(0, true);

    prefetch_abort_occurred = false;
    can_disassemble = false;
    event_pending = false;
    halted = false;
    waiting_for_event = false;
    int_pending = false;

    local_exclusive_start = 0;
    local_exclusive_end = 0;

#ifdef BLOSSOM_HAS_JIT
    if (jit)
        jit->reset();
#endif
}

void ARM_CPU::interpret_one()
{
    try
    {
        if (CPSR.thumb)
        {
            fetch_new_instr_ptr(gpr[15] - 2);
            if (prefetch_abort_occurred)
                throw EmuException::ARMPrefetchAbort(gpr[15] - 2);
            uint16_t instr = *(uint16_t*)&instr_ptr[(gpr[15] - 2) & 0xFFF];
            gpr[15] += 2;
            if (can_disassemble)
                printf("[$%08X] $%04X  %s\n", gpr[15] - 4, instr, ARM_Disasm::disasm_thumb(*this, instr).c_str());
            ARM_Interpreter::interpret_thumb(*this, instr);
        }
        else
        {
            fetch_new_instr_ptr(gpr[15] - 4);
            if (prefetch_abort_occurred)
                throw EmuException::ARMPrefetchAbort(gpr[15] - 4);
            uint32_t instr = *(uint32_t*)&instr_ptr[(gpr[15] - 4) & 0xFFF];
            gpr[15] += 4;
            if (can_disassemble)
                printf("[$%08X] $%08X  %s\n", gpr[15] - 8, instr, ARM_Disasm::disasm_arm(*this, instr).c_str());
            ARM_Interpreter::interpret_arm(*this, instr);
        }
        cycles_ran++;
    }
    catch (EmuException::ARMDataAbort& a)
    {
        data_abort(a.vaddr, a.is_write);
    }
    catch (EmuException::ARMPrefetchAbort& p)
    {
        prefetch_abort(p.vaddr);
    }
}

void ARM_CPU::run(int cycles)
{
    if (halted)
        return;

#ifdef BLOSSOM_HAS_JIT
    if (jit)
    {
        jit->run(cycles);
        debug_cycles += cycles_ran;
        return;
    }
#endif

    try
    {
        cycles_ran = 0;
        while (!halted && cycles_ran < cycles)
        {
            if (CPSR.thumb)
            {
                if (prefetch_abort_occurred)
                    throw EmuException::ARMPrefetchAbort(gpr[15] - 2);
                uint16_t instr = *(uint16_t*)&instr_ptr[(gpr[15] - 2) & 0xFFF];
                if ((gpr[15] & 0xFFF) == 0)
                    fetch_new_instr_ptr(gpr[15]);
                gpr[15] += 2;
                if (can_disassemble)
                {
                    printf("[$%08X] $%04X  %s\n", gpr[15] - 4, instr, ARM_Disasm::disasm_thumb(*this, instr).c_str());
                    //print_state();
                }
                ARM_Interpreter::interpret_thumb(*this, instr);
            }
            else
            {
                if (prefetch_abort_occurred)
                    throw EmuException::ARMPrefetchAbort(gpr[15] - 4);
                uint32_t instr = *(uint32_t*)&instr_ptr[(gpr[15] - 4) & 0xFFF];
                if ((gpr[15] & 0xFFF) == 0)
                    fetch_new_instr_ptr(gpr[15]);
                gpr[15] += 4;
                if (can_disassemble)
                {
                    printf("[$%08X] $%08X  %s\n", gpr[15] - 8, instr, ARM_Disasm::disasm_arm(*this, instr).c_str());
                    //print_state();
                }
                ARM_Interpreter::interpret_arm(*this, instr);
            }
            cycles_ran++;
        }
    }
    catch (EmuException::ARMDataAbort& a)
    {
        data_abort(a.vaddr, a.is_write);
    }
    catch (EmuException::ARMPrefetchAbort& p)
    {
        prefetch_abort(p.vaddr);
    }

    debug_cycles += cycles_ran;
    if (int_pending)
        int_check();
}

void ARM_CPU::print_state()
{
    for (int i = 0; i < 16; i++)
    {
        printf("%s:$%08X", get_reg_name(i).c_str(), gpr[i]);
        if (i % 4 == 3)
            printf("\n");
        else
            printf("\t");
    }
    printf("CPSR:$%08X\n", CPSR.get());
}

void ARM_CPU::jp(uint32_t addr, bool change_thumb_state)
{
    //Hack to autoboot cartridge
    /*if (addr == 0x1060B8 && (gpr[15] - 8 == 0x106054))
    {
        uint32_t process_ptr = read32(0xFFFF9004);
        uint32_t pid = read32(process_ptr + 0xB4);
        if (pid == 15)
            return;
    }*/

    gpr[15] = addr;
    fetch_new_instr_ptr(addr);

    if (change_thumb_state)
        CPSR.thumb = addr & 0x1;

    if (CPSR.thumb)
    {
        gpr[15] &= ~0x1;
        gpr[15] += 2;
    }
    else
    {
        gpr[15] &= ~0x3;
        gpr[15] += 4;
    }
}

void ARM_CPU::data_abort(uint32_t addr, bool is_write)
{
    printf("[ARM%d] Data abort at vaddr $%08X\n", id, addr);

    cp15->set_data_abort_regs(addr, is_write);

    uint32_t value = CPSR.get();
    SPSR[PSR_ABORT].set(value);
    LR_abt = gpr[15];

    update_reg_mode(PSR_ABORT);
    CPSR.mode = PSR_ABORT;
    CPSR.irq_disable = true;

    if (cp15->has_high_exceptions())
        jp(0xFFFF0000 + 0x10, true);
    else
        jp(0x10, true);
}

void ARM_CPU::prefetch_abort(uint32_t addr)
{
    prefetch_abort_occurred = false;
    if (id == 9)
        EmuException::die("[ARM%d] Prefetch abort at vaddr $%08X\n", id, addr);

    printf("[ARM%d] Prefetch abort at vaddr $%08X\n", id, addr);

    cp15->set_prefetch_abort_regs(addr);

    uint32_t value = CPSR.get();
    SPSR[PSR_ABORT].set(value);
    LR_abt = addr;
    LR_abt += (CPSR.thumb) ? 2 : 4;

    update_reg_mode(PSR_ABORT);
    CPSR.mode = PSR_ABORT;
    CPSR.irq_disable = true;

    if (cp15->has_high_exceptions())
        jp(0xFFFF0000 + 0x0C, true);
    else
        jp(0x0C, true);
}

void ARM_CPU::swi()
{
    uint8_t op;
    if (!CPSR.thumb)
        op = read32(gpr[15] - 8) & 0xFF;
    else
        op = read16(gpr[15] - 4) & 0xFF;

    if (id == 9 || test_console)
    {
        //printf("SWI $%02X!\n", op);
        if (op == 0xFF)
        {
            unsigned char c = gpr[0] & 0xFF;
            if (c == 0xFF)
                printf("\n");
            else
                printf("%c", c);
            return;
        }
    }
    else if (trace_svc_enabled())
    {
        //uint32_t process_ptr = read32(0xFFFF9004);
        //printf("SVC $%02X, PID%d\n", op, read32(process_ptr + 0xB4));
        if (trace_svc_lines())
            printf("[ARM%d] SVC $%02X!\n", id, op);
        //printf("SVC $%04X!\n", gpr[7]);

        //Trace of service sessions: remember which handle srv gave out for which name, so that
        //SendSyncRequest lines can say which service they go to.
        static std::map<std::pair<int, uint32_t>, std::string> pending_service; //(pid, tls) -> name
        static std::map<std::pair<int, uint32_t>, std::string> service_handles; //(pid, handle) -> name
        {
            uint32_t tls = cp15->mrc(0, 0xD, 0x3, 0x0);
            int pid;
            std::string procname;
            get_hos_process_info(*this, pid, procname);
            auto it = pending_service.find({pid, tls});
            if (it != pending_service.end())
            {
                //The thread runs again, so srv has replied into its command buffer
                uint32_t result = read32(tls + 0x84);
                uint32_t handle = read32(tls + 0x8C);
                if (trace_svc_lines())
                    printf("(PID%d[%s]) srv GetServiceHandle \"%s\" -> result $%08X, handle $%08X\n",
                           pid, procname.c_str(), it->second.c_str(), result, handle);
                if (result == 0)
                    service_handles[{pid, handle}] = it->second;
                pending_service.erase(it);
            }
            //Result of this thread's last service request: the reply sits in its command buffer
            //when it next does an SVC (if it hasn't written a new request with another command
            //id yet). Failures are counted as "FAIL service:header=result".
            struct PendingReply { std::string service; uint32_t header, block, size, ptr; };
            static std::map<std::pair<int, uint32_t>, PendingReply> pending_reply;
            auto rep_it = pending_reply.find({pid, tls});
            if (rep_it != pending_reply.end())
            {
                uint32_t reply_header = read32(tls + 0x80);
                uint32_t result = read32(tls + 0x84);
                const PendingReply& pr = rep_it->second;
                bool same_cmd = (reply_header >> 16) == (pr.header >> 16);
                if (same_cmd && (result & 0x80000000))
                {
                    char what[96];
                    snprintf(what, sizeof(what), "FAIL %s:%08X=%08X", pr.service.c_str(),
                             pr.header, result);
                    count_svc(pid, procname, what);
                    if (trace_svc_level() >= 2)
                    {
                        static int fail_lines = 0;
                        if (fail_lines++ < 400)
                            fprintf(stderr, "[SVCFAIL] PID%d[%s] %s\n", pid, procname.c_str(), what);
                    }
                }
                pending_reply.erase(rep_it);
            }
            if (op == 0x32)
            {
                auto named = service_handles.find({pid, gpr[0]});
                uint32_t hdr = read32(tls + 0x80);
                PendingReply pr{named != service_handles.end() ? named->second : "?", hdr, 0, 0, 0};
                //cfg GetConfigInfoBlk2 / GetConfigInfoBlk8 (cfg:s) / GetConfigInfoBlk8 (cfg:i):
                //size, block ID, buffer descriptor, buffer pointer
                if ((hdr == 0x00010082 || hdr == 0x04010082 || hdr == 0x08010082)
                    && pr.service.rfind("cfg:", 0) == 0)
                {
                    pr.size = read32(tls + 0x84);
                    pr.block = read32(tls + 0x88);
                    pr.ptr = read32(tls + 0x90);
                    if (skip_setup_enabled())
                    {
                        cfg_requests.push_back({pr.block, pr.size});
                        if (cfg_requests.size() > 64)
                            cfg_requests.pop_front();
                    }
                }
                pending_reply[{pid, tls}] = pr;
                if (hdr == 0x00010800)
                {
                    //err:f ThrowFatalError: type, result code, PC, PID, title IDs
                    uint32_t w[9];
                    for (int i = 0; i < 9; i++)
                        w[i] = read32(tls + 0x84 + i * 4);
                    fprintf(stderr, "[ERRF] PID%d[%s] type %d result $%08X pc $%08X pid $%X "
                            "title %08X %08X app %08X %08X\n", pid, procname.c_str(), w[0] & 0xFF,
                            w[1], w[2], w[3], w[5], w[4], w[7], w[6]);
                }
            }
            //The patch is applied on cfg's side, when it replies (svcReplyAndReceive): the block
            //data is in the mapped buffer, which is still mapped in cfg, and the requester hasn't
            //run yet. The reply only gives the size, so match it with the oldest request of that size.
            if (op == 0x4F && skip_setup_enabled() && procname.rfind("cfg", 0) == 0 && gpr[3])
            {
                uint32_t hdr = read32(tls + 0x80);
                if (hdr == 0x00010042 || hdr == 0x04010042 || hdr == 0x08010042)
                {
                    uint32_t result = read32(tls + 0x84);
                    uint32_t size = read32(tls + 0x88) >> 4;
                    uint32_t ptr = read32(tls + 0x8C);
                    for (auto req = cfg_requests.begin(); req != cfg_requests.end(); ++req)
                    {
                        if (req->size != size)
                            continue;
                        uint32_t block = req->block;
                        cfg_requests.erase(req);
                        if (result != 0)
                            break;
                        try
                        {
                            if (block == 0x00110000 && size >= 1)
                            {
                                write8(ptr, 1);
                                fprintf(stderr, "[SKIP_SETUP] cfg block 0x00110000 -> setup done\n");
                            }
                            else if (block == 0x000D0000 && size >= 2)
                            {
                                write8(ptr, 0x7F);
                                write8(ptr + 1, 0x7F);
                                fprintf(stderr, "[SKIP_SETUP] cfg block 0x000D0000 -> EULA 7F.7F\n");
                            }
                        }
                        catch (const EmuException::ARMDataAbort&)
                        {
                        }
                        break;
                    }
                }
            }
            if (op != 0x32)
            {
                char what[16];
                snprintf(what, sizeof(what), "svc%02X", op);
                count_svc(pid, procname, what);
            }
        }

        if (op == 0x32)
        {
            uint32_t tls = cp15->mrc(0, 0xD, 0x3, 0x0);
            uint32_t header = read32(tls + 0x80);

            int pid;
            std::string procname;
            get_hos_process_info(*this, pid, procname);

            auto named = service_handles.find({pid, gpr[0]});
            {
                char what[64];
                snprintf(what, sizeof(what), "%s:%08X",
                         named != service_handles.end() ? named->second.c_str() : "?", header);
                count_svc(pid, procname, what);
            }
            if (trace_svc_lines())
                printf("(PID%d[%s]) SendSyncRequest: $%08X (handle $%08X%s%s)\n", pid, procname.c_str(), header,
                   gpr[0], named != service_handles.end() ? " = " : "",
                   named != service_handles.end() ? named->second.c_str() : "");

            //srv: GetServiceHandle (0x00050100) and RegisterService (0x00030100) carry the service
            //name in words 1-2. Other services can reuse these headers, so only print printable names.
            if (header == 0x00050100 || header == 0x00030100)
            {
                char name[9] = {};
                uint32_t w1 = read32(tls + 0x84), w2 = read32(tls + 0x88);
                memcpy(name, &w1, 4);
                memcpy(name + 4, &w2, 4);
                bool printable = name[0] != 0;
                for (int i = 0; i < 8 && name[i]; i++)
                    printable &= name[i] >= 0x20 && name[i] < 0x7F;
                if (printable)
                {
                    if (trace_svc_lines())
                        printf("(PID%d[%s]) srv %s \"%s\"\n", pid, procname.c_str(),
                           header == 0x00050100 ? "GetServiceHandle" : "RegisterService", name);
                    if (header == 0x00050100)
                        pending_service[{pid, tls}] = name;
                }
            }
        }
    }
    if (op == 0x3C)
    {
        EmuException::die("[ARM%d] svcBreak called!", id);
    }
    //print_state();

    can_disassemble = false;

    uint32_t value = CPSR.get();
    SPSR[PSR_SUPERVISOR].set(value);

    LR_svc = gpr[15];
    LR_svc -= (!CPSR.thumb) ? 4 : 2;

    update_reg_mode(PSR_SUPERVISOR);
    CPSR.mode = PSR_SUPERVISOR;
    CPSR.irq_disable = true;

    if (cp15->has_high_exceptions())
        jp(0xFFFF0000 + 0x08, true);
    else
        jp(0x08, true);
}

void ARM_CPU::und()
{
    //can_disassemble = true;
    uint32_t value = CPSR.get();
    SPSR[PSR_UNDEFINED].set(value);

    LR_und = gpr[15] - 4;
    update_reg_mode(PSR_UNDEFINED);
    CPSR.mode = PSR_UNDEFINED;
    CPSR.irq_disable = true;

    if (cp15->has_high_exceptions())
        jp(0xFFFF0000 + 0x04, true);
    else
        jp(0x04, true);
}

void ARM_CPU::int_check()
{
    //printf("[ARM%d] Interrupt check\n", id);
#ifdef BLOSSOM_HAS_JIT
    if (in_jit)
    {
        //Registers live in the JIT right now; take the interrupt once Jit::Run returns
        jit->request_exit();
        return;
    }
#endif
    if (!CPSR.irq_disable)
    {
        if (trace_svc_lines())
            printf("[ARM%d] Interrupt!\n", id);
        uint32_t value = CPSR.get();
        SPSR[PSR_IRQ].set(value);

        //Update new CPSR
        LR_irq = gpr[15] + ((CPSR.thumb) ? 2 : 0);
        update_reg_mode(PSR_IRQ);
        CPSR.mode = PSR_IRQ;
        CPSR.irq_disable = true;

        can_disassemble = false;

        if (cp15->has_high_exceptions())
            jp(0xFFFF0000 + 0x18, true);
        else
            jp(0x18, true);
    }
}

void ARM_CPU::set_int_signal(bool pending)
{
    if (!int_pending && pending)
    {
        unhalt();
        int_check();
    }
    int_pending = pending;
    //printf("[ARM%d] Set int signal: %d\n", id, pending);
}

void ARM_CPU::halt()
{
    if (!int_pending)
    {
        //printf("[ARM%d] Halting... $%08X\n", id, CPSR.get());
        halted = true;
    }
}

void ARM_CPU::unhalt()
{
    halted = false;
}

void ARM_CPU::wfe()
{
    if (!event_pending)
    {
        printf("[ARM%d] WFE\n", id);
        waiting_for_event = true;
        halted = true;
    }
    else
        event_pending = false;
}

void ARM_CPU::sev()
{
    e->arm11_send_events(id);
}

void ARM_CPU::send_event(int id)
{
    if (this->id == id)
        return;
    if (waiting_for_event)
    {
        printf("[ARM%d] WFE cancelled\n", id);
        halted = false;
        waiting_for_event = false;
    }
    else
        event_pending = true;
}

void ARM_CPU::set_zero_neg_flags(uint32_t value)
{
    CPSR.negative = value & (1 << 31);
    CPSR.zero = !value;
}

//Code here from melonDS's ALU core (my original implementation was incorrect in many ways)
void ARM_CPU::set_CV_add_flags(uint32_t a, uint32_t b, uint32_t result)
{
    CPSR.carry = ((0xFFFFFFFF-a) < b);
    CPSR.overflow = ADD_OVERFLOW(a, b, result);
}

void ARM_CPU::set_CV_sub_flags(uint32_t a, uint32_t b, uint32_t result)
{
    CPSR.carry = a >= b;
    CPSR.overflow = SUB_OVERFLOW(a, b, result);
}

void ARM_CPU::update_reg_mode(PSR_MODE mode)
{
    if (mode != CPSR.mode)
    {
        switch (CPSR.mode)
        {
            case PSR_SYSTEM:
            case PSR_USER:
                break;
            case PSR_IRQ:
                std::swap(gpr[13], SP_irq);
                std::swap(gpr[14], LR_irq);
                break;
            case PSR_FIQ:
                std::swap(gpr[8], fiq_regs[0]);
                std::swap(gpr[9], fiq_regs[1]);
                std::swap(gpr[10], fiq_regs[2]);
                std::swap(gpr[11], fiq_regs[3]);
                std::swap(gpr[12], fiq_regs[4]);
                std::swap(gpr[13], SP_fiq);
                std::swap(gpr[14], LR_fiq);
                break;
            case PSR_SUPERVISOR:
                std::swap(gpr[13], SP_svc);
                std::swap(gpr[14], LR_svc);
                break;
            case PSR_ABORT:
                std::swap(gpr[13], SP_abt);
                std::swap(gpr[14], LR_abt);
                break;
            case PSR_UNDEFINED:
                std::swap(gpr[13], SP_und);
                std::swap(gpr[14], LR_und);
                break;
            default:
                EmuException::die("[ARM%d] Unrecognized old PSR mode %d\n", id, CPSR.mode);
        }

        switch (mode)
        {
            case PSR_SYSTEM:
            case PSR_USER:
                break;
            case PSR_IRQ:
                std::swap(gpr[13], SP_irq);
                std::swap(gpr[14], LR_irq);
                break;
            case PSR_FIQ:
                std::swap(gpr[8], fiq_regs[0]);
                std::swap(gpr[9], fiq_regs[1]);
                std::swap(gpr[10], fiq_regs[2]);
                std::swap(gpr[11], fiq_regs[3]);
                std::swap(gpr[12], fiq_regs[4]);
                std::swap(gpr[13], SP_fiq);
                std::swap(gpr[14], LR_fiq);
                break;
            case PSR_SUPERVISOR:
                std::swap(gpr[13], SP_svc);
                std::swap(gpr[14], LR_svc);
                break;
            case PSR_ABORT:
                std::swap(gpr[13], SP_abt);
                std::swap(gpr[14], LR_abt);
                break;
            case PSR_UNDEFINED:
                std::swap(gpr[13], SP_und);
                std::swap(gpr[14], LR_und);
                break;
            default:
                EmuException::die("[ARM%d] Unrecognized new PSR mode %d\n", id, mode);
        }
    }
}

void ARM_CPU::spsr_to_cpsr()
{
    uint32_t new_CPSR = SPSR[CPSR.mode].get();
    update_reg_mode((PSR_MODE)(new_CPSR & 0x1F));
    CPSR.set(new_CPSR);
}

bool ARM_CPU::meets_condition(int cond)
{
    switch (cond)
    {
        case 0x0:
            //EQ - equal
            return CPSR.zero;
        case 0x1:
            //NE - not equal
            return !CPSR.zero;
        case 0x2:
            //CS - unsigned higher or same
            return CPSR.carry;
        case 0x3:
            //CC - unsigned lower
            return !CPSR.carry;
        case 0x4:
            //MI - negative
            return CPSR.negative;
        case 0x5:
            //PL - positive or zero
            return !CPSR.negative;
        case 0x6:
            //VS - overflow
            return CPSR.overflow;
        case 0x7:
            //VC - no overflow
            return !CPSR.overflow;
        case 0x8:
            //HI - unsigned higher
            return CPSR.carry && !CPSR.zero;
        case 0x9:
            //LS - unsigned lower or same
            return !CPSR.carry || CPSR.zero;
        case 0xA:
            //GE - greater than or equal
            return (CPSR.negative == CPSR.overflow);
        case 0xB:
            //LT - less than
            return (CPSR.negative != CPSR.overflow);
        case 0xC:
            //GT - greater than
            return !CPSR.zero && (CPSR.negative == CPSR.overflow);
        case 0xD:
            //LE - less than or equal to
            return CPSR.zero || (CPSR.negative != CPSR.overflow);
        case 0xE:
            //AL - always
            return true;
        case 0xF:
            //Some instructions have the 0xF condition required, so let them pass
            return true;
        default:
            EmuException::die("[ARM_CPU] Unrecognized condition %d\n", cond);
            return false;
    }
}

void ARM_CPU::fetch_new_instr_ptr(uint32_t addr)
{
    uint64_t mem = (uint64_t)tlb_map[addr / 4096];
    if (!(mem & (1ULL << 60ULL)))
    {
        cp15->reload_tlb(addr);
        mem = (uint64_t)tlb_map[addr / 4096];
        if (!(mem & (1ULL << 60ULL)))
            prefetch_abort_occurred = true;
    }
    if (mem & (1ULL << 63ULL))
        EmuException::die("[ARM%d] PC points to MMIO $%08X", id, addr);
    mem &= ~(0xFULL << 60ULL);
    instr_ptr = (uint8_t*)mem;
}

uint8_t ARM_CPU::read8(uint32_t addr)
{
    uint64_t mem = (uint64_t)tlb_map[addr / 4096];
    if (!(mem & (1ULL << 62ULL)))
    {
        //TLB miss - reload and check the vaddr again
        cp15->reload_tlb(addr);
        mem = (uint64_t)tlb_map[addr / 4096];
        if (!(mem & (1ULL << 62ULL)))
            throw EmuException::ARMDataAbort(addr, false);
    }
    if (!(mem & (1ULL << 63ULL)))
    {
        mem &= ~(0xFULL << 60ULL);
        uint8_t* ptr = (uint8_t*)mem;
        return ptr[addr & 0xFFF];
    }
    else
        addr = (mem & 0xFFFFF000) + (addr & 0xFFF);
    uint8_t value = (id == 9) ? e->arm9_read8(addr) : e->arm11_read8(id - 11, addr);
    if (id != 9 && trace_mmio_enabled())
        trace_mmio(id, 'R', 8, addr, value);
    return value;
}

uint16_t ARM_CPU::read16(uint32_t addr)
{
    if (id == 9 && (addr & 0x1))
        EmuException::die("[ARM9] Unaligned read16 $%08X", addr);
    if ((addr & 0xFFF) > 0xFFE)
    {
        uint16_t value = read8(addr) | (read8(addr + 1) << 8);
        return value;
    }
    uint64_t mem = (uint64_t)tlb_map[addr / 4096];
    if (!(mem & (1ULL << 62ULL)))
    {
        //TLB miss - reload and check the vaddr again
        cp15->reload_tlb(addr);
        mem = (uint64_t)tlb_map[addr / 4096];
        if (!(mem & (1ULL << 62ULL)))
            throw EmuException::ARMDataAbort(addr, false);
    }
    if (!(mem & (1ULL << 63ULL)))
    {
        mem &= ~(0xFULL << 60ULL);
        uint8_t* ptr = (uint8_t*)mem;
        return *(uint16_t*)&ptr[addr & 0xFFF];
    }
    else
        addr = (mem & 0xFFFFF000) + (addr & 0xFFF);
    uint16_t value = (id == 9) ? e->arm9_read16(addr) : e->arm11_read16(id - 11, addr);
    if (id != 9 && trace_mmio_enabled())
        trace_mmio(id, 'R', 16, addr, value);
    return value;
}

uint32_t ARM_CPU::read32(uint32_t addr)
{
    if (id == 9 && (addr & 0x3))
        EmuException::die("[ARM9] Unaligned read32 $%08X", addr);
    if ((addr & 0xFFF) > 0xFFC)
    {
        //Handle unaligned reads on page boundaries by reading two words, as pages might be noncontiguous
        uint32_t word1 = read32(addr & ~0x3);
        uint32_t word2 = read32((addr + 4) & ~0x3);

        int low_bits = addr & 0x3;

        uint32_t word = word1 >> (low_bits * 8);
        word |= word2 << ((4 - low_bits) * 8);
        return word;
    }
    uint64_t mem = (uint64_t)tlb_map[addr / 4096];
    if (!(mem & (1ULL << 62ULL)))
    {
        //TLB miss - reload and check the vaddr again
        cp15->reload_tlb(addr);
        mem = (uint64_t)tlb_map[addr / 4096];
        if (!(mem & (1ULL << 62ULL)))
            throw EmuException::ARMDataAbort(addr, false);
    }
    if (!(mem & (1ULL << 63ULL)))
    {
        mem &= ~(0xFULL << 60ULL);
        uint8_t* ptr = (uint8_t*)mem;
        return *(uint32_t*)&ptr[addr & 0xFFF];
    }
    else
        addr = (mem & 0xFFFFF000) + (addr & 0xFFF);

    uint32_t value = (id == 9) ? e->arm9_read32(addr) : e->arm11_read32(id - 11, addr);
    if (id != 9 && trace_mmio_enabled())
        trace_mmio(id, 'R', 32, addr, value);
    return value;
}

uint64_t ARM_CPU::read64(uint32_t addr)
{
    uint64_t value = read32(addr + 4);
    value <<= 32ULL;
    return value | read32(addr);
}

void ARM_CPU::write8(uint32_t addr, uint8_t value)
{
    if (id != 9 && trace_writes_enabled())
        trace_mmio(id, 'w', 8, addr, value);
    uint64_t mem = (uint64_t)tlb_map[addr / 4096];
    if (!(mem & (1ULL << 61ULL)))
    {
        //TLB miss - reload and check the vaddr again
        cp15->reload_tlb(addr);
        mem = (uint64_t)tlb_map[addr / 4096];
        if (!(mem & (1ULL << 62ULL)))
            throw EmuException::ARMDataAbort(addr, true);
    }
    if (!(mem & (1ULL << 63ULL)))
    {
        mem &= ~(0xFULL << 60ULL);
        uint8_t* ptr = (uint8_t*)mem;
        ptr[addr & 0xFFF] = value;
        return;
    }
    else
        addr = (mem & 0xFFFFF000) + (addr & 0xFFF);
    if (id != 9 && trace_mmio_enabled())
        trace_mmio(id, 'W', 8, addr, value);
    if (id == 9)
        e->arm9_write8(addr, value);
    else
        e->arm11_write8(id - 11, addr, value);
}

void ARM_CPU::write16(uint32_t addr, uint16_t value)
{
    if (id != 9 && trace_writes_enabled())
        trace_mmio(id, 'w', 16, addr, value);
    if (id == 9 && (addr & 0x1))
        EmuException::die("[ARM9] Unaligned write16 $%08X: $%04X", addr, value);
    if ((addr & 0xFFF) > 0xFFE)
        EmuException::die("[ARM%d] Unaligned write16 on page boundary $%08X: $%08X", id, addr, value);
    uint64_t mem = (uint64_t)tlb_map[addr / 4096];
    if (!(mem & (1ULL << 61ULL)))
    {
        //TLB miss - reload and check the vaddr again
        cp15->reload_tlb(addr);
        mem = (uint64_t)tlb_map[addr / 4096];
        if (!(mem & (1ULL << 62ULL)))
            throw EmuException::ARMDataAbort(addr, true);
    }
    if (!(mem & (1ULL << 63ULL)))
    {
        mem &= ~(0xFULL << 60ULL);
        uint8_t* ptr = (uint8_t*)mem;
        *(uint16_t*)&ptr[addr & 0xFFF] = value;
        return;
    }
    else
        addr = (mem & 0xFFFFF000) + (addr & 0xFFF);
    if (id != 9 && trace_mmio_enabled())
        trace_mmio(id, 'W', 16, addr, value);
    if (id == 9)
        e->arm9_write16(addr, value);
    else
        e->arm11_write16(id - 11, addr, value);
}

void ARM_CPU::write32(uint32_t addr, uint32_t value)
{
    if (id != 9 && trace_writes_enabled())
        trace_mmio(id, 'w', 32, addr, value);
    if (id == 9 && (addr & 0x3))
        EmuException::die("[ARM9] Unaligned write32 $%08X: $%08X", addr, value);
    if ((addr & 0xFFF) > 0xFFC)
    {
        printf("[ARM%d] Unaligned write32 $%08X: $%08X\n", id, addr, value);
        const static uint32_t mask[] = {0xFF, 0xFFFF, 0xFFFFFF, 0xFFFFFFFF};
        const static uint32_t read_mask[] = {0, 0xFF, 0xFFFF, 0xFFFFFF};
        int low_bits = addr & 0x3;
        uint32_t word1 = value & mask[3 - low_bits];
        uint32_t word2 = value & mask[low_bits];

        word1 <<= (low_bits * 8);
        word2 >>= ((4 - low_bits) * 8);

        printf("Word1: $%08X Word2: $%08X\n", word1, word2);

        word1 |= read32(addr & ~0x3) & read_mask[low_bits];
        word2 |= read32((addr & ~0x3) + 4) & read_mask[3 - low_bits];

        printf("New1: $%08X New2: $%08X\n", word1, word2);

        write32(addr & ~0x3, word1);
        write32((addr & ~0x3) + 4, word2);
        //EmuException::die("Unaligned write32 on page boundary");
    }
    uint64_t mem = (uint64_t)tlb_map[addr / 4096];
    if (!(mem & (1ULL << 61ULL)))
    {
        //TLB miss - reload and check the vaddr again
        if (id == 9 && (addr & 0xF0000000) == 0xC0000000)
            return;
        cp15->reload_tlb(addr);
        mem = (uint64_t)tlb_map[addr / 4096];
        if (!(mem & (1ULL << 61ULL)))
            throw EmuException::ARMDataAbort(addr, true);
    }
    if (!(mem & (1ULL << 63ULL)))
    {
        mem &= ~(0xFULL << 60ULL);
        uint8_t* ptr = (uint8_t*)mem;
        *(uint32_t*)&ptr[addr & 0xFFF] = value;
        return;
    }
    else
        addr = (mem & 0xFFFFF000) + (addr & 0xFFF);
    if (id != 9 && trace_mmio_enabled())
        trace_mmio(id, 'W', 32, addr, value);
    if (id == 9)
        e->arm9_write32(addr, value);
    else
        e->arm11_write32(id - 11, addr, value);
}

void ARM_CPU::write64(uint32_t addr, uint64_t value)
{
    write32(addr, value & 0xFFFFFFFF);
    write32(addr + 4, value >> 32);
}

bool ARM_CPU::has_exclusive(uint32_t addr)
{
    uint64_t paddr = (uint64_t)tlb_map[addr / 4096] + (addr & 0xFFF);
    paddr &= ~(0xFFULL << 56ULL);

    if (paddr < local_exclusive_start || paddr > local_exclusive_end)
        return false;

    int core = id - 11;

    if (paddr < global_exclusive_start[core] || paddr > global_exclusive_end[core])
        return false;

    return true;
}

void ARM_CPU::set_exclusive(uint32_t addr, uint32_t size)
{
    uint64_t paddr = (uint64_t)tlb_map[addr / 4096] + (addr & 0xFFF);

    paddr &= ~(0xFFULL << 56ULL);
    local_exclusive_start = paddr;
    local_exclusive_end = paddr + size;

    global_exclusive_start[id - 11] = local_exclusive_start;
    global_exclusive_end[id - 11] = local_exclusive_end;
}

void ARM_CPU::clear_global_exclusives(uint32_t addr)
{
    if (id < 11)
        return;
    uint64_t paddr = (uint64_t)tlb_map[addr / 4096] + (addr & 0xFFF);

    paddr &= ~(0xFFULL << 56ULL);

    for (int i = 0; i < 4; i++)
    {
        if (global_exclusive_start[i] <= paddr && paddr < global_exclusive_end[i])
        {
            global_exclusive_start[i] = 0;
            global_exclusive_end[i] = 0;
        }
    }
}

void ARM_CPU::clear_exclusive()
{
    local_exclusive_start = 0;
    local_exclusive_end = 0;
    global_exclusive_start[id - 11] = 0;
    global_exclusive_end[id - 11] = 0;
}

void ARM_CPU::andd(int destination, int source, int operand, bool set_condition_codes)
{
    uint32_t result = source & operand;
    set_register(destination, result);

    if (set_condition_codes)
        set_zero_neg_flags(result);
}

void ARM_CPU::orr(int destination, int source, int operand, bool set_condition_codes)
{
    uint32_t result = source | operand;
    set_register(destination, result);

    if (set_condition_codes)
        set_zero_neg_flags(result);
}

void ARM_CPU::eor(int destination, int source, int operand, bool set_condition_codes)
{
    uint32_t result = source ^ operand;
    set_register(destination, result);

    if (set_condition_codes)
        set_zero_neg_flags(result);
}

void ARM_CPU::add(uint32_t destination, uint32_t source, uint32_t operand, bool set_condition_codes)
{
    uint64_t unsigned_result = source + operand;
    if (destination == REG_PC)
    {
        if (set_condition_codes)
            EmuException::die("adds pc, operand unsupported");
        else
            jp(unsigned_result & 0xFFFFFFFF, true);
    }
    else
    {
        set_register(destination, unsigned_result & 0xFFFFFFFF);

        if (set_condition_codes)
            cmn(source, operand);
    }
}

void ARM_CPU::sub(uint32_t destination, uint32_t source, uint32_t operand, bool set_condition_codes)
{
    uint64_t unsigned_result = source - operand;
    if (destination == REG_PC)
    {
        if (set_condition_codes)
        {
            int index = static_cast<int>(CPSR.mode);
            update_reg_mode(SPSR[index].mode);
            CPSR.set(SPSR[index].get());
            jp(unsigned_result & 0xFFFFFFFF, false);
        }
        else
            jp(unsigned_result & 0xFFFFFFFF, true);
    }
    else
    {
        set_register(destination, unsigned_result & 0xFFFFFFFF);
        if (set_condition_codes)
            cmp(source, operand);
    }
}

void ARM_CPU::adc(uint32_t destination, uint32_t source, uint32_t operand, bool set_condition_codes)
{
    uint8_t carry = (CPSR.carry) ? 1 : 0;
    add(destination, source + carry, operand, set_condition_codes);
    if (set_condition_codes)
    {
        uint32_t temp = source + operand;
        uint32_t res = temp + carry;
        CPSR.carry = CARRY_ADD(source, operand) | CARRY_ADD(temp, carry);
        CPSR.overflow = ADD_OVERFLOW(source, operand, temp) | ADD_OVERFLOW(temp, carry, res);
    }
}

void ARM_CPU::sbc(uint32_t destination, uint32_t source, uint32_t operand, bool set_condition_codes)
{
    unsigned int borrow = (CPSR.carry) ? 0 : 1;
    sub(destination, source, operand + borrow, set_condition_codes);
    if (set_condition_codes)
    {
        uint32_t temp = source - operand;
        uint32_t res = temp - borrow;
        CPSR.carry = CARRY_SUB(source, operand) & CARRY_SUB(temp, borrow);
        CPSR.overflow = SUB_OVERFLOW(source, operand, temp) | SUB_OVERFLOW(temp, borrow, res);
    }
}

void ARM_CPU::tst(uint32_t x, uint32_t y)
{
    set_zero_neg_flags(x & y);
}

void ARM_CPU::teq(uint32_t x, uint32_t y)
{
    set_zero_neg_flags(x ^ y);
}

void ARM_CPU::cmn(uint32_t x, uint32_t y)
{
    uint32_t result = x + y;
    set_zero_neg_flags(result);
    set_CV_add_flags(x, y, result);
}

void ARM_CPU::cmp(uint32_t x, uint32_t y)
{
    uint32_t result = x - y;
    set_zero_neg_flags(result);
    set_CV_sub_flags(x, y, result);
}

void ARM_CPU::mov(uint32_t destination, uint32_t operand, bool alter_flags)
{
    if (destination == REG_PC)
    {
        if (alter_flags)
        {
            int index = static_cast<int>(CPSR.mode);
            update_reg_mode(SPSR[index].mode);
            CPSR.set(SPSR[index].get());
            jp(operand, false);
        }
        else
            jp(operand, true);
    }
    else
    {
        set_register(destination, operand);
        if (alter_flags)
            set_zero_neg_flags(operand);
    }
}

void ARM_CPU::mul(uint32_t destination, uint32_t source, uint32_t operand, bool set_condition_codes)
{
    uint64_t result = source * operand;
    uint32_t truncated = result & 0xFFFFFFFF;
    set_register(destination, truncated);

    if (set_condition_codes)
        set_zero_neg_flags(truncated);
}

void ARM_CPU::bic(uint32_t destination, uint32_t source, uint32_t operand, bool alter_flags)
{
    uint32_t result = source & ~operand;
    set_register(destination, result);
    if (alter_flags)
        set_zero_neg_flags(result);
}

void ARM_CPU::mvn(uint32_t destination, uint32_t operand, bool alter_flags)
{
    set_register(destination, ~operand);
    if (alter_flags)
        set_zero_neg_flags(~operand);
}

void ARM_CPU::mrs(uint32_t instr)
{
    bool using_CPSR = (instr & (1 << 22)) == 0;
    uint32_t destination = (instr >> 12) & 0xF;

    if (using_CPSR)
    {
        set_register(destination, CPSR.get());
    }
    else
    {
        set_register(destination, SPSR[static_cast<int>(CPSR.mode)].get());
    }
}

void ARM_CPU::msr(uint32_t instr)
{
    bool is_imm = (instr & (1 << 25));
    bool using_CPSR = (instr & (1 << 22)) == 0;

    PSR_Flags* PSR;
    if (using_CPSR)
        PSR = &CPSR;
    else
        PSR = &SPSR[static_cast<int>(CPSR.mode)];

    uint32_t value = PSR->get();
    uint32_t source;
    if (is_imm)
    {
        source = instr & 0xFF;
        int shift = (instr & 0xF00) >> 7;
        source = rotr32(source, shift, false);
    }
    else
        source = get_register(instr & 0xF);

    uint32_t bitmask = 0;
    if (instr & (1 << 19))
        bitmask |= 0xFF000000;

    if (instr & (1 << 16))
        bitmask |= 0xFF;

    if (CPSR.mode == PSR_USER)
        bitmask &= 0xFFFFFF00; //prevent changes to control field

    if (using_CPSR)
        bitmask &= 0xFFFFFFDF; //prevent changes to thumb state

    value &= ~bitmask;
    value |= (source & bitmask);

    if (using_CPSR)
    {
        PSR_MODE new_mode = static_cast<PSR_MODE>(value & 0x1F);
        update_reg_mode(new_mode);
    }
    PSR->set(value);
}

void ARM_CPU::cps(uint32_t instr)
{
    if (CPSR.mode == PSR_USER)
        return;
    PSR_MODE psr_mode = (PSR_MODE)(instr & 0x1F);
    bool f = (instr >> 6) & 0x1;
    bool i = (instr >> 7) & 0x1;
    bool a = (instr >> 8) & 0x1;
    bool mmod = (instr >> 17) & 0x1;
    int imod = (instr >> 18) & 0x3;

    if (mmod)
    {
        update_reg_mode(psr_mode);
        CPSR.mode = psr_mode;
    }

    if (imod == 2)
    {
        //TODO: data abort
        CPSR.fiq_disable = CPSR.fiq_disable && !f;
        CPSR.irq_disable = CPSR.irq_disable && !i;
    }
    else if (imod == 3)
    {
        CPSR.fiq_disable |= f;
        CPSR.irq_disable |= i;
    }
}

void ARM_CPU::srs(uint32_t instr)
{
    bool is_writing_back = instr & (1 << 21);
    bool is_adding_offset = instr & (1 << 23);
    bool is_preindexing = instr & (1 << 24);

    PSR_MODE mode = (PSR_MODE)(instr & 0x1F);

    uint32_t saved_LR = gpr[REG_LR];
    uint32_t saved_PSR = SPSR[CPSR.mode].get();

    PSR_MODE old_mode = CPSR.mode;
    update_reg_mode(mode);
    CPSR.mode = mode;

    uint32_t banked_addr = get_register(REG_SP);

    int offset;
    if (is_adding_offset)
    {
        offset = 4;

        if (is_preindexing)
        {
            write32(banked_addr + offset, saved_LR);
            write32(banked_addr + (offset * 2), saved_PSR);
        }
        else
        {
            write32(banked_addr, saved_LR);
            write32(banked_addr + offset, saved_PSR);
        }
    }
    else
    {
        offset = -4;

        if (is_preindexing)
        {
            write32(banked_addr + offset, saved_PSR);
            write32(banked_addr + (offset * 2), saved_LR);
        }
        else
        {
            write32(banked_addr, saved_PSR);
            write32(banked_addr + offset, saved_LR);
        }
    }

    if (is_writing_back)
        set_register(REG_SP, banked_addr + (offset * 2));

    //Restore previous state
    update_reg_mode(old_mode);
    CPSR.mode = old_mode;
}

void ARM_CPU::rfe(uint32_t instr)
{
    bool is_writing_back = instr & (1 << 21);
    bool is_adding_offset = instr & (1 << 23);
    bool is_preindexing = instr & (1 << 24);

    int base = (instr >> 16) & 0xF;

    uint32_t addr = get_register(base);
    uint32_t PC, PSR;
    int offset;
    if (is_adding_offset)
    {
        offset = 4;

        if (is_preindexing)
        {
            PC = read32(addr + offset);
            PSR = read32(addr + (offset * 2));
        }
        else
        {
            PC = read32(addr);
            PSR = read32(addr + offset);
        }
    }
    else
    {
        offset = -4;

        if (is_preindexing)
        {
            PSR = read32(addr + offset);
            PC = read32(addr + (offset * 2));
        }
        else
        {
            PSR = read32(addr);
            PC = read32(addr + offset);
        }
    }

    if (is_writing_back)
        set_register(base, addr + (offset * 2));

    update_reg_mode((PSR_MODE)(PSR & 0x1F));
    CPSR.set(PSR);

    //If PC is less than this value, we're returning to userland
    if (PC < 0x40000000)
    {
        //can_disassemble = true;
        std::string procname;
        int pid;
        get_hos_process_info(*this, pid, procname);
        printf("Jumping to PID%d[%s] (addr: $%08X)\n", pid, procname.c_str(), PC);
        if (addr & 0xFFFFF)
        {
            uint32_t prev_instr = read32(addr - 4);
            if (prev_instr == 0xEF000032)
            {
                uint32_t error = read32(cp15->mrc(0, 0xD, 3, 0) + 0x84);
                if (error & (1 << 31))
                    printf("Error: $%08X\n", error);
            }
            else if ((prev_instr & 0x0F000000) == 0x0F000000 && (prev_instr & 0xFF) != 0x28)
            {
                if (gpr[0] & (1 << 31))
                   printf("Error: $%08X\n", gpr[0]);
            }
        }
        if (pid == 7)
        {
            //can_disassemble = true;

        }
        else
            can_disassemble = false;
    }

    jp(PC, false);
}

void ARM_CPU::vfp_single_transfer(uint32_t instr)
{
    ARM_Interpreter::vfp_single_transfer(*this, *vfp, instr);
}

void ARM_CPU::vfp_load_store(uint32_t instr)
{
    ARM_Interpreter::vfp_load_store(*this, *vfp, instr);
}

void ARM_CPU::vfp_data_processing(uint32_t instr)
{
    ARM_Interpreter::vfp_data_processing(*this, *vfp, instr);
}

uint32_t ARM_CPU::mrc(int coprocessor_id, int operation_mode, int CP_reg,
                  int coprocessor_info, int coprocessor_operand)
{
    switch (coprocessor_id)
    {
        case 15:
            if (!cp15)
                return 0;
            return cp15->mrc(operation_mode, CP_reg, coprocessor_info, coprocessor_operand);
        default:
            return 0;
    }
}

void ARM_CPU::mcr(int coprocessor_id, int operation_mode,
                  int CP_reg, int coprocessor_info, int coprocessor_operand, uint32_t value)
{
    switch (coprocessor_id)
    {
        case 15:
        {
            cp15->mcr(operation_mode, CP_reg, coprocessor_info, coprocessor_operand, value);

            uint8_t** old_map = tlb_map;
            tlb_map = cp15->get_tlb_mapping();
#ifdef BLOSSOM_HAS_JIT
            if (jit)
                jit->on_cp15_write((CP_reg << 8) | (coprocessor_operand << 4) | coprocessor_info, value,
                                   old_map != tlb_map);
#else
            (void)old_map;
#endif
            break;
        }
    }
}

uint32_t ARM_CPU::lsl(uint32_t value, int shift, bool alter_flags)
{
    if (!shift)
    {
        if (alter_flags)
            set_zero_neg_flags(value);
        return value;
    }
    if (shift > 32)
    {
        if (alter_flags)
        {
            set_zero_neg_flags(0);
            CPSR.carry = false;
        }
        return 0;
    }
    if (shift > 31)
    {
        if (alter_flags)
        {
            set_zero_neg_flags(0);
            CPSR.carry = value & (1 << 0);
        }
        return 0;
    }
    uint32_t result = value << shift;
    if (alter_flags)
    {
        set_zero_neg_flags(result);
        CPSR.carry = value & (1 << (32 - shift));
    }
    return value << shift;
}

uint32_t ARM_CPU::lsr(uint32_t value, int shift, bool alter_flags)
{
    if (shift > 32)
    {
        if (alter_flags)
        {
            set_zero_neg_flags(0);
            CPSR.carry = false;
        }
        return 0;
    }
    if (shift > 31)
        return lsr_32(value, alter_flags);
    uint32_t result = value >> shift;
    if (alter_flags)
    {
        set_zero_neg_flags(result);
        if (shift)
            CPSR.carry = value & (1 << (shift - 1));
    }
    return result;
}

uint32_t ARM_CPU::lsr_32(uint32_t value, bool alter_flags)
{
    if (alter_flags)
    {
        set_zero_neg_flags(0);
        CPSR.carry = value & (1 << 31);
    }
    return 0;
}

uint32_t ARM_CPU::asr(uint32_t value, int shift, bool alter_flags)
{
    if (shift > 31)
        return asr_32(value, alter_flags);
    uint32_t result = static_cast<int32_t>(value) >> shift;
    if (alter_flags)
    {
        set_zero_neg_flags(result);
        if (shift)
            CPSR.carry = value & (1 << (shift - 1));
    }
    return result;
}

uint32_t ARM_CPU::asr_32(uint32_t value, bool alter_flags)
{
    uint32_t result = static_cast<int32_t>(value) >> 31;
    if (alter_flags)
    {
        set_zero_neg_flags(result);
        CPSR.carry = value & (1 << 31);
    }
    return result;
}

uint32_t ARM_CPU::rrx(uint32_t value, bool alter_flags)
{
    uint32_t result = value;
    result >>= 1;
    result |= (CPSR.carry) ? (1 << 31) : 0;
    if (alter_flags)
    {
        set_zero_neg_flags(result);
        CPSR.carry = value & 0x1;
    }
    return result;
}

//Thanks to Stack Overflow for providing a safe rotate function
uint32_t ARM_CPU::rotr32(uint32_t n, unsigned int c, bool alter_flags)
{
    const unsigned int mask = 0x1F;
    if (alter_flags && c)
    {
        if (c & 0x1F)
            CPSR.carry = n & (1 << (c - 1));
        else
            CPSR.carry = n & (1 << 31);
    }
    c &= mask;

    uint32_t result = (n>>c) | (n<<( (-c)&mask ));

    if (alter_flags)
    {
        set_zero_neg_flags(result);
    }

    return result;
}
