// SysMemUserForUser HLE for the P3P profile (Gate 5B).
//
// The semantically load-bearing entry point is sceKernelAllocPartitionMemory:
// init allocates memory blocks before it ever opens a file, and a return-0
// stub makes the game dereference a null block head. Block bookkeeping lives
// in P3PKernel (it owns the partition RangeAllocator); this unit translates.
#pragma once

#include <cstdint>

namespace psprecomp {
class Runtime;
struct AllegrexContext;
}  // namespace psprecomp

namespace p3p {

class P3PKernel;

inline constexpr const char *kSysMemLibrary = "SysMemUserForUser";
inline constexpr std::uint32_t kNidAllocPartitionMemory = 0x237DBD4Fu;
inline constexpr std::uint32_t kNidFreePartitionMemory = 0xB6D61D02u;
inline constexpr std::uint32_t kNidGetBlockHeadAddr = 0x9D9A5BA1u;
inline constexpr std::uint32_t kNidKernelPrintf = 0x13A5ABEFu;
inline constexpr std::uint32_t kNidSetCompiledSdkVersion = 0x35669D4Cu;
inline constexpr std::uint32_t kNidSetCompilerVersion = 0xF77D77CBu;

// Returns true when the NID was handled (result in $v0).
bool hle_sysmem_dispatch(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                         P3PKernel &kernel, std::uint32_t nid);

}  // namespace p3p
