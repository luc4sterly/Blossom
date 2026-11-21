#ifndef HEAP_STATS_HPP
#define HEAP_STATS_HPP
#include <atomic>
#include <cstdint>

//Bytes and blocks currently allocated with operator new, and the largest single allocation, for
//the watchdog (heap_stats.cpp replaces the global operator new/delete)
extern std::atomic<int64_t> heap_bytes, heap_blocks, heap_largest;

#endif // HEAP_STATS_HPP
