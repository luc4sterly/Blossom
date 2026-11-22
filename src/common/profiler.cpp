#include <chrono>
#include <thread>
#if defined(__x86_64__) || defined(_M_X64)
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#define BLOSSOM_PROFILER_TSC 1
#endif
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

//Committed private memory in MB on Windows, -1 elsewhere
long private_mb()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)))
        return (long)(pmc.PrivateUsage >> 20);
    return -1;
#else
    return -1;
#endif
}

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
//Bucket switches happen ~1M times a frame, so they are timed with the TSC (a few ns) instead of
//steady_clock, which costs about a microsecond on some Windows machines and was itself showing
//up as "other". Ticks are converted to ms once a frame against steady_clock.
using Tick = uint64_t;
inline Tick ticks()
{
#ifdef BLOSSOM_PROFILER_TSC
    return __rdtsc();
#else
    return (Tick)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
Tick last_switch;
Tick frame_start;
Clock::time_point frame_start_clock;
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
    "jit-mem", "jit-interp", "jit-codecheck", "jit-glue", "jit-compile", "loop", "frontend", "report"
};

//Time spent with no bucket open ("other"), split by the bucket that was left last and the one
//entered next, so a gap in the instrumentation can be found: "other: arm9>dsp 120.0" means
//120 ms passed between leaving arm9 and entering dsp.
int last_left = OTHER;
//The profiler only follows the emulation thread; calls from other threads would corrupt the stack
std::thread::id owner;
bool have_owner = false;
uint64_t foreign_calls = 0;

}

void claim_thread()
{
    owner = std::this_thread::get_id();
    have_owner = true;
}

namespace
{

bool foreign()
{
    auto id = std::this_thread::get_id();
    if (!have_owner)
    {
        owner = id;
        have_owner = true;
    }
    if (id != owner)
    {
        foreign_calls++;
        return true;
    }
    return false;
}
double gaps[BUCKET_COUNT][BUCKET_COUNT];
double gap_max;
int gap_max_from, gap_max_to;

void charge(Tick now, int next = -1)
{
    Bucket cur = depth ? stack[depth - 1] : OTHER;
    double ms = (double)(now - last_switch); //in ticks until end_frame converts
    totals[cur] += ms;
    if (!depth && next >= 0)
    {
        gaps[last_left][next] += ms;
        if (ms > gap_max)
        {
            gap_max = ms;
            gap_max_from = last_left;
            gap_max_to = next;
        }
    }
    last_switch = now;
}

}

void init()
{
    const char* env = std::getenv("BLOSSOM_PROFILE");
    enabled = env && env[0] && env[0] != '0';
    depth = 0;
    have_owner = false;
    for (double& t : totals)
        t = 0;
    for (uint64_t& c : counters)
        c = 0;
    last_switch = frame_start = ticks();
    frame_start_clock = Clock::now();
    if (enabled)
    {
        //How much the old timer cost per call, for comparison with the old profiles
        auto t0 = Clock::now();
        for (int i = 0; i < 1000; i++)
            (void)Clock::now();
        double ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / 1000.0;
        std::fprintf(stderr, "[PROFILE] steady_clock::now() costs %.0f ns per call here\n", ns);
    }
}

void enter(Bucket b)
{
    if (foreign())
        return;
    charge(ticks(), b);
    if (depth < MAX_DEPTH)
        stack[depth] = b;
    depth++;
}

void leave()
{
    if (foreign())
        return;
    charge(ticks());
    if (depth > 0)
    {
        depth--;
        if (!depth)
            last_left = depth < MAX_DEPTH ? stack[0] : OTHER;
    }
}

void end_frame(int frame)
{
    if (!enabled)
        return;
    Tick now = ticks();
    auto now_clock = Clock::now();
    charge(now, OTHER); //a gap since the last bucket shows as "...>other"
    double total = std::chrono::duration<double, std::milli>(now_clock - frame_start_clock).count();
    //Convert the tick totals to ms
    double ms_per_tick = now > frame_start ? total / (double)(now - frame_start) : 0.0;
    for (double& t : totals)
        t *= ms_per_tick;
    for (auto& row : gaps)
        for (double& g : row)
            g *= ms_per_tick;
    gap_max *= ms_per_tick;
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
    long priv = private_mb();
    if (priv >= 0)
        std::fprintf(stderr, " | ram %ld MB (private %ld MB)\n", resident_mb(), priv);
    else
        std::fprintf(stderr, " | ram %ld MB\n", resident_mb());

    //Where "other" went: the three biggest gaps between buckets, and the longest single gap
    struct Gap { double ms; int from, to; } top[3] = {};
    for (int a = 0; a < BUCKET_COUNT; a++)
        for (int b = 0; b < BUCKET_COUNT; b++)
        {
            double ms = gaps[a][b];
            gaps[a][b] = 0;
            for (int k = 0; k < 3; k++)
                if (ms > top[k].ms)
                {
                    for (int m = 2; m > k; m--)
                        top[m] = top[m - 1];
                    top[k] = {ms, a, b};
                    break;
                }
        }
    std::fprintf(stderr, "[PROFILE] frame %d other:", frame);
    for (auto& g : top)
        if (g.ms >= 0.05)
            std::fprintf(stderr, " %s>%s %.1f", names[g.from], names[g.to], g.ms);
    std::fprintf(stderr, " | longest %.1f ms (%s>%s)", gap_max, names[gap_max_from], names[gap_max_to]);
    if (foreign_calls)
        std::fprintf(stderr, " | calls from other threads %llu", (unsigned long long)foreign_calls);
    std::fprintf(stderr, "\n");
    foreign_calls = 0;
    gap_max = 0;
    frame_start = now;
    frame_start_clock = now_clock;
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
