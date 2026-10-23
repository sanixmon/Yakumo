// P3P profile PRX module loader (Gate 5D).
//
// Parses a PSP relocatable module (ET_SCE_RELEXE), places its segments
// into a fresh partition block, applies its relocations, parses the
// module info and syslib export (for module_start), and registers the
// module with the kernel. The image is NOT executed by this gate:
// StartModule records intent, and whether PRX code must run is decided
// from boot evidence (GATE5D_PLAN.md).
//
// Format references: PPSSPP Core/ELF/ElfReader.cpp and PSPElfTypes.h —
// SHT_PRXREL (0x700000A0) sections hold 8-byte {r_offset, r_info}
// entries; all relocations are against the module's own load bias
// (r_info's symbol field is unused for these modules).

#pragma once

#include "psprecomp/guest_memory.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace p3p {

class P3PKernel;

struct PrxLoadResult {
    bool ok = false;
    std::string error;
    std::int32_t uid = 0;
    std::string name;
    std::uint32_t base = 0;
    std::uint32_t size = 0;
    std::uint32_t gp = 0;
    std::uint32_t module_start = 0;  // absolute guest address, 0 = none
    std::uint32_t relocations_applied = 0;
    std::uint32_t relocations_skipped = 0;
};

PrxLoadResult load_prx_module(psprecomp::GuestMemory &memory, P3PKernel &kernel,
                              const std::vector<std::uint8_t> &bytes);

}  // namespace p3p
