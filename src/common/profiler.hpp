#ifndef PROFILER_HPP
#define PROFILER_HPP
#include <chrono>
#include <cstdint>

//Very small per-frame profiler. Enable it with the environment variable BLOSSOM_PROFILE=1.
//Time is "exclusive": while a nested scope runs, its time is not charged to the enclosing scope,
//so a GPU command list processed from inside an ARM11 MMIO write is counted as GPU, not ARM11.
namespace Profiler
{

enum Bucket
{
    OTHER,
    ARM11,
    ARM9,
    GPU_CMDLIST,
    GPU_TRANSFER,
    GPU_MEMFILL,
    GPU_DISPLAY,
    DSP,
    WIFI,
    DMA_TIMERS,
    EVENTS,
    BUCKET_COUNT
};

enum Counter
{
    JIT_CACHE_CLEARS,
    JIT_RANGE_INVALIDATIONS,
    JIT_FALLBACKS,
    JIT_CONTEXTS,
    COUNTER_COUNT
};

extern bool enabled;
//Per-frame event counters, printed (and reset) by end_frame. Cheap enough to bump unconditionally.
extern uint64_t counters[COUNTER_COUNT];

void init();
void enter(Bucket b);
void leave();
void end_frame(int frame);

class Scope
{
    private:
        bool active;
    public:
        explicit Scope(Bucket b) : active(enabled) { if (active) enter(b); }
        ~Scope() { if (active) leave(); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
};

}

#endif // PROFILER_HPP
