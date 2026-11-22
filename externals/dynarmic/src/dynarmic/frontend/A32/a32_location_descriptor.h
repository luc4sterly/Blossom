/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <functional>
#include <string>
#include <tuple>

#include <fmt/format.h>
#include <mcl/stdint.hpp>

#include "dynarmic/frontend/A32/FPSCR.h"
#include "dynarmic/frontend/A32/ITState.h"
#include "dynarmic/frontend/A32/PSR.h"
#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::A32 {

/**
 * LocationDescriptor describes the location of a basic block.
 * The location is not solely based on the PC because other flags influence the way
 * instructions should be translated. The CPSR.T flag is most notable since it
 * tells us if the processor is in Thumb or Arm mode.
 */
class LocationDescriptor {
public:
    // Indicates bits that should be preserved within descriptors.
    static constexpr u32 CPSR_MODE_MASK = 0x0600FE20;
    static constexpr u32 FPSCR_MODE_MASK = 0x07F70000;
    // Blossom: bits of the upper half of the descriptor that dynarmic does not use itself. The
    // embedder stores an opaque context there (address space id, VFP enable) so that code compiled
    // for one context is never reused for another. See Jit::SetContext.
    static constexpr u32 CONTEXT_MASK = 0x780000F8;
    // Blossom: context bit meaning "VFP is disabled (FPEXC.EN = 0)"; VFP instructions then go to
    // the interpreter fallback, which raises the Undefined Instruction exception.
    static constexpr u32 CONTEXT_VFP_DISABLED = 0x00000008;

    LocationDescriptor(u32 arm_pc, PSR cpsr, FPSCR fpscr, bool single_stepping = false, u32 context = 0)
            : arm_pc(arm_pc)
            , cpsr(cpsr.Value() & CPSR_MODE_MASK)
            , fpscr(fpscr.Value() & FPSCR_MODE_MASK)
            , single_stepping(single_stepping)
            , context(context & CONTEXT_MASK) {}

    explicit LocationDescriptor(const IR::LocationDescriptor& o) {
        arm_pc = static_cast<u32>(o.Value());
        cpsr.T((o.Value() >> 32) & 1);
        cpsr.E((o.Value() >> 32) & 2);
        fpscr = (o.Value() >> 32) & FPSCR_MODE_MASK;
        cpsr.IT(ITState{static_cast<u8>(o.Value() >> 40)});
        single_stepping = (o.Value() >> 32) & 4;
        context = static_cast<u32>(o.Value() >> 32) & CONTEXT_MASK;
    }

    u32 PC() const { return arm_pc; }
    bool TFlag() const { return cpsr.T(); }
    bool EFlag() const { return cpsr.E(); }
    ITState IT() const { return cpsr.IT(); }

    A32::PSR CPSR() const { return cpsr; }
    A32::FPSCR FPSCR() const { return fpscr; }

    bool SingleStepping() const { return single_stepping; }
    u32 Context() const { return context; }
    bool VFPDisabled() const { return (context & CONTEXT_VFP_DISABLED) != 0; }

    bool operator==(const LocationDescriptor& o) const {
        return std::tie(arm_pc, cpsr, fpscr, single_stepping, context) == std::tie(o.arm_pc, o.cpsr, o.fpscr, o.single_stepping, o.context);
    }

    bool operator!=(const LocationDescriptor& o) const {
        return !operator==(o);
    }

    LocationDescriptor SetPC(u32 new_arm_pc) const {
        return LocationDescriptor(new_arm_pc, cpsr, fpscr, single_stepping, context);
    }

    LocationDescriptor AdvancePC(int amount) const {
        return LocationDescriptor(static_cast<u32>(arm_pc + amount), cpsr, fpscr, single_stepping, context);
    }

    LocationDescriptor SetTFlag(bool new_tflag) const {
        PSR new_cpsr = cpsr;
        new_cpsr.T(new_tflag);

        return LocationDescriptor(arm_pc, new_cpsr, fpscr, single_stepping, context);
    }

    LocationDescriptor SetEFlag(bool new_eflag) const {
        PSR new_cpsr = cpsr;
        new_cpsr.E(new_eflag);

        return LocationDescriptor(arm_pc, new_cpsr, fpscr, single_stepping, context);
    }

    LocationDescriptor SetFPSCR(u32 new_fpscr) const {
        return LocationDescriptor(arm_pc, cpsr, A32::FPSCR{new_fpscr & FPSCR_MODE_MASK}, single_stepping, context);
    }

    LocationDescriptor SetIT(ITState new_it) const {
        PSR new_cpsr = cpsr;
        new_cpsr.IT(new_it);

        return LocationDescriptor(arm_pc, new_cpsr, fpscr, single_stepping, context);
    }

    LocationDescriptor AdvanceIT() const {
        return SetIT(IT().Advance());
    }

    LocationDescriptor SetSingleStepping(bool new_single_stepping) const {
        return LocationDescriptor(arm_pc, cpsr, fpscr, new_single_stepping, context);
    }

    u64 UniqueHash() const noexcept {
        // This value MUST BE UNIQUE.
        // This calculation has to match up with EmitX64::EmitTerminalPopRSBHint
        const u64 pc_u64 = arm_pc;
        const u64 fpscr_u64 = fpscr.Value();
        const u64 t_u64 = cpsr.T() ? 1 : 0;
        const u64 e_u64 = cpsr.E() ? 2 : 0;
        const u64 single_stepping_u64 = single_stepping ? 4 : 0;
        const u64 it_u64 = u64(cpsr.IT().Value()) << 8;
        const u64 context_u64 = context;
        const u64 upper = (fpscr_u64 | t_u64 | e_u64 | single_stepping_u64 | it_u64 | context_u64) << 32;
        return pc_u64 | upper;
    }

    operator IR::LocationDescriptor() const {
        return IR::LocationDescriptor{UniqueHash()};
    }

private:
    u32 arm_pc;        ///< Current program counter value.
    PSR cpsr;          ///< Current program status register.
    A32::FPSCR fpscr;  ///< Floating point status control register.
    bool single_stepping;
    u32 context;  ///< Blossom: opaque embedder context (CONTEXT_MASK bits).
};

/**
 * Provides a string representation of a LocationDescriptor.
 *
 * @param descriptor The descriptor to get a string representation of
 */
std::string ToString(const LocationDescriptor& descriptor);

}  // namespace Dynarmic::A32

namespace std {
template<>
struct less<Dynarmic::A32::LocationDescriptor> {
    bool operator()(const Dynarmic::A32::LocationDescriptor& x, const Dynarmic::A32::LocationDescriptor& y) const noexcept {
        return x.UniqueHash() < y.UniqueHash();
    }
};
template<>
struct hash<Dynarmic::A32::LocationDescriptor> {
    size_t operator()(const Dynarmic::A32::LocationDescriptor& x) const noexcept {
        return std::hash<Dynarmic::IR::LocationDescriptor>()(Dynarmic::IR::LocationDescriptor{x.UniqueHash()});
    }
};
}  // namespace std

template<>
struct fmt::formatter<Dynarmic::A32::LocationDescriptor> : fmt::formatter<std::string> {
    template<typename FormatContext>
    auto format(Dynarmic::A32::LocationDescriptor descriptor, FormatContext& ctx) const {
        return formatter<std::string>::format(Dynarmic::A32::ToString(descriptor), ctx);
    }
};
