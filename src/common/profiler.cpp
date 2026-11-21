#include <cstdio>
#include <cstdlib>
#include "common/profiler.hpp"

namespace Profiler
{

bool enabled = false;

namespace
{

using Clock = std::chrono::steady_clock;

constexpr int MAX_DEPTH = 32;
Bucket stack[MAX_DEPTH];
int depth = 0;
Clock::time_point last_switch;
Clock::time_point frame_start;
double totals[BUCKET_COUNT];

const char* names[BUCKET_COUNT] =
{
    "other", "arm11", "arm9", "gpu-cmd", "gpu-xfer", "gpu-fill", "gpu-disp", "dsp", "wifi", "dma+timers", "events"
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
    std::fprintf(stderr, "\n");
    frame_start = now;
}

}
