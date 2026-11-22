/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <functional>
#include <string>

#include <fmt/format.h>
#include <mcl/stdint.hpp>

namespace Dynarmic::IR {

class LocationDescriptor {
public:
    explicit LocationDescriptor(u64 value)
            : value(value) {}

    bool operator==(const LocationDescriptor& o) const {
        return value == o.Value();
    }

    bool operator!=(const LocationDescriptor& o) const {
        return !operator==(o);
    }

    u64 Value() const { return value; }

private:
    u64 value;
};

std::string ToString(const LocationDescriptor& descriptor);

inline bool operator<(const LocationDescriptor& x, const LocationDescriptor& y) noexcept {
    return x.Value() < y.Value();
}

}  // namespace Dynarmic::IR

namespace std {
template<>
struct less<Dynarmic::IR::LocationDescriptor> {
    bool operator()(const Dynarmic::IR::LocationDescriptor& x, const Dynarmic::IR::LocationDescriptor& y) const noexcept {
        return x < y;
    }
};
template<>
struct hash<Dynarmic::IR::LocationDescriptor> {
    size_t operator()(const Dynarmic::IR::LocationDescriptor& x) const noexcept {
        // Blossom: mix the bits (splitmix64 finalizer). std::hash<u64> is the identity on libstdc++, and
        // tsl::robin_map's power-of-two policy indexes with the low bits, which are just the PC: the
        // same PC in many contexts (every 3DS process has .text at $00100000) all landed in one bucket
        // run, and rehash_on_extreme_load kept doubling patch_information until it took GBs.
        u64 z = x.Value();
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return static_cast<size_t>(z ^ (z >> 31));
    }
};
}  // namespace std

template<>
struct fmt::formatter<Dynarmic::IR::LocationDescriptor> : fmt::formatter<std::string> {
    template<typename FormatContext>
    auto format(Dynarmic::IR::LocationDescriptor descriptor, FormatContext& ctx) const {
        return formatter<std::string>::format(ToString(descriptor), ctx);
    }
};
