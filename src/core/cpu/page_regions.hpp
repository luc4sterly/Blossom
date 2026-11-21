#ifndef PAGE_REGIONS_HPP
#define PAGE_REGIONS_HPP
#include <cstdint>
#include <cstring>
#include <vector>

//Remembers which 1 MB regions (256 pages of 4 KB) of a 4 GB page table hold any entries, so that
//flushing the table only touches the regions that were filled instead of all 1M entries.
class PageRegions
{
    private:
        bool marked[4096];
        std::vector<uint16_t> list;
    public:
        static constexpr uint32_t PAGES_PER_REGION = 256;

        PageRegions() { std::memset(marked, 0, sizeof(marked)); }

        inline void mark(uint32_t page)
        {
            uint32_t region = page >> 8;
            if (!marked[region])
            {
                marked[region] = true;
                list.push_back((uint16_t)region);
            }
        }

        //Calls f(first_page) for every marked region that lies in [start_page, start_page + page_count)
        //and forgets those regions. Ranges are expected to be region-aligned.
        template <typename F>
        void flush(uint32_t start_page, uint32_t page_count, F f)
        {
            uint64_t end = (uint64_t)start_page + page_count;
            size_t kept = 0;
            for (size_t i = 0; i < list.size(); i++)
            {
                uint32_t first = (uint32_t)list[i] * PAGES_PER_REGION;
                if (first >= start_page && first < end)
                {
                    marked[list[i]] = false;
                    f(first);
                }
                else
                    list[kept++] = list[i];
            }
            list.resize(kept);
        }

        template <typename F>
        void flush_all(F f)
        {
            for (uint16_t region : list)
            {
                marked[region] = false;
                f((uint32_t)region * PAGES_PER_REGION);
            }
            list.clear();
        }

        void reset()
        {
            std::memset(marked, 0, sizeof(marked));
            list.clear();
        }
};

//Page table in the format dynarmic reads directly: a host pointer to the start of each 4 KB page,
//or nullptr to go through the memory callbacks. Bit 0 set means the page is read-only (writes take
//the callback path). Entries are filled lazily by the JIT's memory callbacks and dropped by the MMU
//whenever the matching TLB entry changes.
class JitPageTable
{
    private:
        PageRegions regions;
    public:
        static constexpr uint32_t PAGE_COUNT = 1024 * 1024;
        uint8_t* entries[PAGE_COUNT];

        JitPageTable() { std::memset(entries, 0, sizeof(entries)); }

        inline void set(uint32_t page, uint8_t* host_page, bool read_only)
        {
            //The low two bits carry flags, so only 4-byte aligned host pages can be used directly
            if ((uintptr_t)host_page & 0x3)
                return;
            regions.mark(page);
            entries[page] = (uint8_t*)((uintptr_t)host_page | (read_only ? 1 : 0));
        }

        inline void invalidate_page(uint32_t page)
        {
            entries[page] = nullptr;
        }

        void invalidate_range(uint32_t start_page, uint32_t page_count)
        {
            regions.flush(start_page, page_count, [this](uint32_t first)
            {
                std::memset(&entries[first], 0, PageRegions::PAGES_PER_REGION * sizeof(uint8_t*));
            });
        }

        void invalidate_all()
        {
            regions.flush_all([this](uint32_t first)
            {
                std::memset(&entries[first], 0, PageRegions::PAGES_PER_REGION * sizeof(uint8_t*));
            });
        }
};

#endif // PAGE_REGIONS_HPP
