#include <cstdlib>
#include <new>
#include "common/heap_stats.hpp"

std::atomic<int64_t> heap_bytes{0}, heap_blocks{0}, heap_largest{0};

namespace
{

//Each block carries its size in a 16-byte header, which keeps the default 16-byte alignment
constexpr size_t HEADER = 16;

void* counted_alloc(size_t size)
{
    void* raw = std::malloc(size + HEADER);
    if (!raw)
        return nullptr;
    *(size_t*)raw = size;
    heap_bytes.fetch_add((int64_t)size, std::memory_order_relaxed);
    heap_blocks.fetch_add(1, std::memory_order_relaxed);
    int64_t largest = heap_largest.load(std::memory_order_relaxed);
    while ((int64_t)size > largest && !heap_largest.compare_exchange_weak(largest, (int64_t)size))
        ;
    return (char*)raw + HEADER;
}

void counted_free(void* ptr)
{
    if (!ptr)
        return;
    void* raw = (char*)ptr - HEADER;
    heap_bytes.fetch_sub((int64_t)*(size_t*)raw, std::memory_order_relaxed);
    heap_blocks.fetch_sub(1, std::memory_order_relaxed);
    std::free(raw);
}

void* counted_new(size_t size)
{
    void* p = counted_alloc(size ? size : 1);
    if (!p)
        throw std::bad_alloc();
    return p;
}

}

void* operator new(size_t size) { return counted_new(size); }
void* operator new[](size_t size) { return counted_new(size); }
void* operator new(size_t size, const std::nothrow_t&) noexcept { return counted_alloc(size ? size : 1); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { return counted_alloc(size ? size : 1); }
void operator delete(void* ptr) noexcept { counted_free(ptr); }
void operator delete[](void* ptr) noexcept { counted_free(ptr); }
void operator delete(void* ptr, size_t) noexcept { counted_free(ptr); }
void operator delete[](void* ptr, size_t) noexcept { counted_free(ptr); }
void operator delete(void* ptr, const std::nothrow_t&) noexcept { counted_free(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept { counted_free(ptr); }
