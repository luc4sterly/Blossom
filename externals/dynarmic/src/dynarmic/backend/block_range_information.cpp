/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/block_range_information.h"

#include <boost/icl/interval_map.hpp>
#include <boost/icl/interval_set.hpp>
#include <mcl/stdint.hpp>
#include <tsl/robin_set.h>

namespace Dynarmic::Backend {

template<typename ProgramCounterType>
void BlockRangeInformation<ProgramCounterType>::AddRange(boost::icl::discrete_interval<ProgramCounterType> range, IR::LocationDescriptor location) {
    const auto [it, inserted] = location_ranges.try_emplace(location, range);
    if (!inserted) {
        block_ranges.subtract(std::make_pair(it->second, std::set<IR::LocationDescriptor>{location}));
        it->second = range;
    }
    block_ranges.add(std::make_pair(range, std::set<IR::LocationDescriptor>{location}));
}

template<typename ProgramCounterType>
void BlockRangeInformation<ProgramCounterType>::ClearCache() {
    block_ranges.clear();
    location_ranges.clear();
}

template<typename ProgramCounterType>
tsl::robin_set<IR::LocationDescriptor> BlockRangeInformation<ProgramCounterType>::InvalidateRanges(const boost::icl::interval_set<ProgramCounterType>& ranges) {
    tsl::robin_set<IR::LocationDescriptor> erase_locations;
    for (auto invalidate_interval : ranges) {
        auto pair = block_ranges.equal_range(invalidate_interval);
        for (auto it = pair.first; it != pair.second; ++it) {
            for (const auto& descriptor : it->second) {
                erase_locations.insert(descriptor);
            }
        }
    }
    for (const auto& descriptor : erase_locations) {
        const auto it = location_ranges.find(descriptor);
        if (it == location_ranges.end()) {
            continue;
        }
        block_ranges.subtract(std::make_pair(it->second, std::set<IR::LocationDescriptor>{descriptor}));
        location_ranges.erase(it);
    }
    return erase_locations;
}

template class BlockRangeInformation<u32>;
template class BlockRangeInformation<u64>;

}  // namespace Dynarmic::Backend
