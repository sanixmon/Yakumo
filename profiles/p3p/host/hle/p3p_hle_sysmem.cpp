#include "hle/p3p_hle_sysmem.hpp"

#include "kernel/p3p_kernel.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdio>
#include <string>

namespace p3p {
namespace {

bool read_guest_string(psprecomp::GuestMemory &memory, std::uint32_t address,
                       std::size_t max_length, std::string &out) {
    if (address == 0u || !memory.contains(address, 1u))
        return false;
    out.clear();
    for (std::size_t i = 0; i < max_length; ++i) {
        if (!memory.contains(address + static_cast<std::uint32_t>(i), 1u))
            break;
        const char c = static_cast<char>(memory.load8(address + static_cast<std::uint32_t>(i)));
        if (c == '\0')
            return true;
        out.push_back(c);
    }
    return true;
}

}  // namespace

bool hle_sysmem_dispatch(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                         P3PKernel &kernel, std::uint32_t nid) {
    psprecomp::GuestMemory &memory = runtime.memory();
    const auto set_v0 = [&ctx](std::int32_t result) {
        ctx.set_gpr(2, static_cast<std::uint32_t>(result));
    };
    switch (nid) {
        case kNidAllocPartitionMemory: {
            std::string name;
            if (!read_guest_string(memory, ctx.gpr[5], 32u, name))
                name = "<unreadable>";
            // EABI argument layout (PPSSPP Core/HLE/HLE.cpp): arguments 5-8
            // travel in $t0-$t3, so the address/alignment is $t0 here.
            const std::uint32_t addr = ctx.gpr[8];
            const std::int32_t result = kernel.alloc_partition_memory(
                static_cast<std::int32_t>(ctx.gpr[4]), name,
                static_cast<std::int32_t>(ctx.gpr[6]), ctx.gpr[7], addr);
            if (result > 0)
                std::fprintf(stderr, "[p3p] alloc partition memory '%s' size %s -> uid %d head %s\n",
                             name.c_str(), psprecomp::hex32(ctx.gpr[7]).c_str(), result,
                             psprecomp::hex32(kernel.block_head_address(result)).c_str());
            else
                std::fprintf(stderr, "[p3p] alloc partition memory '%s' partition %s type %s size %s addr %s -> error %s\n",
                             name.c_str(), psprecomp::hex32(ctx.gpr[4]).c_str(),
                             psprecomp::hex32(ctx.gpr[6]).c_str(),
                             psprecomp::hex32(ctx.gpr[7]).c_str(),
                             psprecomp::hex32(addr).c_str(),
                             psprecomp::hex32(static_cast<std::uint32_t>(result)).c_str());
            set_v0(result);
            return true;
        }
        case kNidFreePartitionMemory:
            set_v0(kernel.free_partition_memory(static_cast<std::int32_t>(ctx.gpr[4])));
            return true;
        case kNidGetBlockHeadAddr:
            ctx.set_gpr(2, kernel.block_head_address(static_cast<std::int32_t>(ctx.gpr[4])));
            return true;
        case kNidKernelPrintf: {
            std::string format;
            if (read_guest_string(memory, ctx.gpr[4], 512u, format))
                std::fprintf(stderr, "[p3p] printf: %s (a1=%s a2=%s a3=%s)\n", format.c_str(),
                             psprecomp::hex32(ctx.gpr[5]).c_str(),
                             psprecomp::hex32(ctx.gpr[6]).c_str(),
                             psprecomp::hex32(ctx.gpr[7]).c_str());
            set_v0(0);
            return true;
        }
        case kNidSetCompiledSdkVersion:
        case kNidSetCompilerVersion:
            set_v0(0);
            return true;
        default:
            return false;
    }
}

}  // namespace p3p
