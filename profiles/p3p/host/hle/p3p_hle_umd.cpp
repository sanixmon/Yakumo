// P3P profile HLE: sceUmdUser (Gate 5C follow-up). See the header.

#include "hle/p3p_hle_umd.hpp"

#include "psprecomp/guest_memory.hpp"

#include <cstdio>
#include <string>

namespace p3p {
namespace {

// Drive state bits (PPSSPP Core/HLE/sceUmd.h pspUmdState).
constexpr std::uint32_t kUmdStatAllowWait = 0x01u | 0x02u | 0x08u | 0x10u | 0x20u;
constexpr std::uint32_t kDriveState = 0x02u | 0x10u | 0x20u;  // PRESENT | READY | READABLE
constexpr std::int32_t kErrorInvalidArgument = static_cast<std::int32_t>(0x800200D2u);

bool g_activated = false;
std::int32_t g_callback_uid = 0;

bool read_name(psprecomp::GuestMemory &memory, std::uint32_t addr, std::string &out) {
    out.clear();
    for (std::uint32_t i = 0; i < 64u; ++i) {
        if (!memory.contains(addr + i, 1u))
            return false;
        const char c = static_cast<char>(memory.load8(addr + i));
        if (c == '\0')
            return true;
        out += c;
    }
    return false;
}

}  // namespace

void umd_reset_state() {
    g_activated = false;
    g_callback_uid = 0;
}

bool hle_umd_dispatch(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                      std::uint32_t nid) {
    psprecomp::GuestMemory &memory = runtime.memory();
    const std::uint32_t a0 = ctx.gpr[4];
    const std::uint32_t a1 = ctx.gpr[5];
    switch (nid) {
        case kNidUmdCheckMedium:
            ctx.set_gpr(2, 1u);  // non-zero: a disc is in the drive
            return true;
        case kNidUmdGetDriveStat:
            ctx.set_gpr(2, kDriveState);
            return true;
        case kNidUmdGetErrorStat:
            ctx.set_gpr(2, 0u);
            return true;
        case kNidUmdWaitDriveStat:
        case kNidUmdWaitDriveStatCB: {
            // The drive already holds the requested state (present and
            // ready), so the wait completes immediately, as in the
            // reference implementation.
            if ((a0 & kUmdStatAllowWait) == 0u) {
                ctx.set_gpr(2, static_cast<std::uint32_t>(kErrorInvalidArgument));
                return true;
            }
            ctx.set_gpr(2, 0u);
            return true;
        }
        case kNidUmdCancelWaitDriveStat:
            ctx.set_gpr(2, 0u);
            return true;
        case kNidUmdActivate: {
            std::string name;
            if (a0 < 1u || a0 > 2u || !read_name(memory, a1, name) ||
                name.rfind("disc0:", 0) != 0) {
                ctx.set_gpr(2, static_cast<std::uint32_t>(kErrorInvalidArgument));
                return true;
            }
            if (!g_activated) {
                g_activated = true;
                std::fprintf(stderr, "[p3p] umd activated (%s)\n", name.c_str());
            }
            ctx.set_gpr(2, 0u);
            return true;
        }
        case kNidUmdRegisterUMDCallBack:
            g_callback_uid = static_cast<std::int32_t>(a0);
            ctx.set_gpr(2, 0u);
            return true;
        case kNidUmdUnRegisterUMDCallBack:
            if (g_callback_uid == static_cast<std::int32_t>(a0))
                g_callback_uid = 0;
            ctx.set_gpr(2, 0u);
            return true;
        default:
            return false;
    }
}

}  // namespace p3p
