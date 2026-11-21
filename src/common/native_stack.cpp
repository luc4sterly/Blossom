#include "common/native_stack.hpp"
#include <cstdio>
#include <cstdint>
#include <cstring>

#if defined(_WIN32) && defined(_M_X64) || defined(_WIN32) && defined(__x86_64__)
#include <windows.h>
#include <thread>
#include <chrono>

namespace NativeStack
{

static DWORD target_tid = 0;

void set_target_thread()
{
    target_tid = GetCurrentThreadId();
}

//Walks the stack with the PE unwind tables (MinGW emits them for x64). Nothing in here may take
//the heap lock: the suspended thread could be holding it.
static int walk(HANDLE thread, uint64_t* frames, int max)
{
    CONTEXT ctx;
    std::memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &ctx))
        return 0;
    int n = 0;
    while (n < max && ctx.Rip)
    {
        frames[n++] = ctx.Rip;
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
        if (fn)
        {
            PVOID handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher,
                             nullptr);
        }
        else
        {
            //Leaf function (or JIT code without unwind info): the return address is on top
            if (!ctx.Rsp || (ctx.Rsp & 7))
                break;
            ctx.Rip = *(DWORD64*)ctx.Rsp;
            ctx.Rsp += 8;
        }
    }
    return n;
}

static void print_frame(int i, uint64_t addr, uint64_t* exe_vmas, int& exe_count)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &module) || !module)
    {
        fprintf(stderr, "[WATCHDOG]   #%-2d $%016llX  (no module: JIT code?)\n", i, (unsigned long long)addr);
        return;
    }
    char path[MAX_PATH] = {};
    GetModuleFileNameA(module, path, MAX_PATH);
    const char* name = std::strrchr(path, '\\');
    name = name ? name + 1 : path;
    const uint8_t* base = (const uint8_t*)module;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    //Address as addr2line sees it (preferred image base, before ASLR)
    uint64_t vma = addr - (uint64_t)base + nt->OptionalHeader.ImageBase;
    fprintf(stderr, "[WATCHDOG]   #%-2d 0x%llX  %s\n", i, (unsigned long long)vma, name);
    if (module == GetModuleHandleA(nullptr) && exe_count < 64)
        exe_vmas[exe_count++] = vma;
}

void report(int samples, int gap_ms)
{
    if (!target_tid)
        return;
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                               target_tid);
    if (!thread)
    {
        fprintf(stderr, "[WATCHDOG] native stack: cannot open the emulator thread\n");
        return;
    }
    for (int s = 0; s < samples; s++)
    {
        if (s)
            std::this_thread::sleep_for(std::chrono::milliseconds(gap_ms));
        uint64_t frames[48];
        int n = 0;
        if (SuspendThread(thread) != (DWORD)-1)
        {
            n = walk(thread, frames, 48);
            ResumeThread(thread);
        }
        fprintf(stderr, "[WATCHDOG] native stack of the emulator thread, sample %d/%d:\n", s + 1, samples);
        uint64_t exe_vmas[64];
        int exe_count = 0;
        for (int i = 0; i < n; i++)
            print_frame(i, frames[i], exe_vmas, exe_count);
        fprintf(stderr, "[WATCHDOG] addr2line -f -C -i -p -e blossom.exe");
        for (int i = 0; i < exe_count; i++)
            fprintf(stderr, " 0x%llX", (unsigned long long)exe_vmas[i]);
        fprintf(stderr, "\n");
    }
    fflush(stderr);
    CloseHandle(thread);
}

}

#else

namespace NativeStack
{

void set_target_thread() {}

void report(int, int)
{
    fprintf(stderr, "[WATCHDOG] native stack: only implemented on Windows x64\n");
}

}

#endif
