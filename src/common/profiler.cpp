#include <cstdio>
#include <cstdlib>
#include "common/profiler.hpp"
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

namespace Profiler
{

//Resident memory of the process in MB, or -1 if unknown
long resident_mb()
{
#ifdef _WIN32
    //Private bytes (committed memory): the working set can stay small while Windows pages the
    //process out, so it missed a leak that grew to GBs
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
        return (long)(pmc.PrivateUsage >> 20);
    return -1;
#else
    long pages = 0, resident = 0;
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f)
        return -1;
    int n = std::fscanf(f, "%ld %ld", &pages, &resident);
    std::fclose(f);
    if (n != 2)
        return -1;
    return resident * sysconf(_SC_PAGESIZE) / (1024 * 1024);
#endif
}

}

namespace Profiler
{

bool enabled = false;
uint64_t counters[COUNTER_COUNT];

namespace
{

using Clock = std::chrono::steady_clock;

constexpr int MAX_DEPTH = 32;
Bucket stack[MAX_DEPTH];
int depth = 0;
Clock::time_point last_switch;
Clock::time_point frame_start;
double totals[BUCKET_COUNT];

const char* counter_names[COUNTER_COUNT] =
{
    "jit-icache-invals", "jit-range-invals", "jit-fallbacks", "jit-new-contexts",
    "jit-translated-instrs", "jit-slow-reads", "jit-slow-writes", "jit-changed-chunks",
    "slices", "jit-compiles", "short-slices"
};

const char* names[BUCKET_COUNT] =
{
    "other", "arm11", "arm9", "gpu-cmd", "gpu-xfer", "gpu-fill", "gpu-disp", "gpu-vsh", "gpu-raster", "dsp", "wifi", "dma+timers", "events",
    "jit-mem", "jit-interp", "jit-codecheck", "jit-glue", "jit-compile", "gpu-vtx", "gpu-prim"
};

void charge(Clock::time_point now)
{
    Bucket cur = depth ? stack[depth - 1] : OTHER;
    totals[cur] += std::chrono::duration<double, std::milli>(now - last_switch).count();
    last_switch = now;
}

}

void init()
{
    const char* env = std::getenv("BLOSSOM_PROFILE");
    enabled = env && env[0] && env[0] != '0';
    depth = 0;
    for (double& t : totals)
        t = 0;
    for (uint64_t& c : counters)
        c = 0;
    last_switch = frame_start = Clock::now();
}

void enter(Bucket b)
{
    charge(Clock::now());
    if (depth < MAX_DEPTH)
        stack[depth] = b;
    depth++;
}

void leave()
{
    charge(Clock::now());
    if (depth > 0)
        depth--;
}

void end_frame(int frame)
{
    if (!enabled)
        return;
    auto now = Clock::now();
    charge(now);
    double total = std::chrono::duration<double, std::milli>(now - frame_start).count();
    std::fprintf(stderr, "[PROFILE] frame %d: %.1f ms |", frame, total);
    for (int i = 0; i < BUCKET_COUNT; i++)
    {
        if (totals[i] >= 0.05)
            std::fprintf(stderr, " %s %.1f (%.0f%%)", names[i], totals[i], total > 0 ? totals[i] * 100.0 / total : 0.0);
        totals[i] = 0;
    }
    for (int i = 0; i < COUNTER_COUNT; i++)
    {
        if (counters[i])
            std::fprintf(stderr, " | %s %llu", counter_names[i], (unsigned long long)counters[i]);
        counters[i] = 0;
    }
    std::fprintf(stderr, " | ram %ld MB\n", resident_mb());
    frame_start = now;
}

const char* bucket_name(int bucket)
{
    return bucket >= 0 && bucket < BUCKET_COUNT ? names[bucket] : "?";
}

int stack_snapshot(int* out, int max)
{
    int d = depth;
    if (d > MAX_DEPTH)
        d = MAX_DEPTH;
    int n = 0;
    for (int i = 0; i < d && n < max; i++)
        out[n++] = stack[i];
    return n;
}

}
