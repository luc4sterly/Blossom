/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <set>
#include <unordered_map>

#include <boost/icl/interval_map.hpp>
#include <boost/icl/interval_set.hpp>
#include <tsl/robin_set.h>

#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend {

template<typename ProgramCounterType>
class BlockRangeInformation {
public:
    void AddRange(boost::icl::discrete_interval<ProgramCounterType> range, IR::LocationDescriptor location);
    void ClearCache();
    tsl::robin_set<IR::LocationDescriptor> InvalidateRanges(const boost::icl::interval_set<ProgramCounterType>& ranges);

private:
    boost::icl::interval_map<ProgramCounterType, std::set<IR::LocationDescriptor>> block_ranges;
    // Blossom: the range each live block was added with, so invalidated blocks can be taken out of
    // block_ranges. Upstream never removes them: every segment kept the set of every block that ever
    // covered it, and with code at the same addresses recompiled for many contexts (each 3DS process
    // has .text at $00100000) the sets, copied on every AddRange, grew by GBs a second.
    std::unordered_map<IR::LocationDescriptor, boost::icl::discrete_interval<ProgramCounterType>> location_ranges;
};

}  // namespace Dynarmic::Backend
