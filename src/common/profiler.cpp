#include <thread>
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
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return (long)(pmc.WorkingSetSize >> 20);
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
    "slices", "short-slices"
};

const char* names[BUCKET_COUNT] =
{
    "other", "arm11", "arm9", "gpu-cmd", "gpu-xfer", "gpu-fill", "gpu-disp", "gpu-vsh", "gpu-raster", "dsp", "wifi", "dma+timers", "events",
    "jit-mem", "jit-interp", "jit-codecheck", "jit-glue", "loop", "frontend"
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

void charge(Clock::time_point now, int next = -1)
{
    Bucket cur = depth ? stack[depth - 1] : OTHER;
    double ms = std::chrono::duration<double, std::milli>(now - last_switch).count();
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
    last_switch = frame_start = Clock::now();
}

void enter(Bucket b)
{
    if (foreign())
        return;
    charge(Clock::now(), b);
    if (depth < MAX_DEPTH)
        stack[depth] = b;
    depth++;
}

void leave()
{
    if (foreign())
        return;
    charge(Clock::now());
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
    auto now = Clock::now();
    charge(now, OTHER); //a gap since the last bucket shows as "...>other"
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
