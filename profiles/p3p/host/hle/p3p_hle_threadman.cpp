#include "p3p_hle_threadman.hpp"

#include "kernel/p3p_kernel.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/guest_memory.hpp"

#include <cstdio>
#include <set>
#include <utility>

#include <string>

namespace p3p {

namespace {

// Reads a NUL-terminated guest string, bounded to `max_length` bytes.
// Returns false when the pointer is not readable at all.
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

std::int32_t hle_create_thread(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                               P3PKernel &kernel) {
    psprecomp::GuestMemory &memory = runtime.memory();
    const std::uint32_t name_address = ctx.gpr[4];   // $a0
    const std::uint32_t entry = ctx.gpr[5];          // $a1
    const auto priority = static_cast<std::int32_t>(ctx.gpr[6]);  // $a2
    const std::uint32_t stack_size = ctx.gpr[7];     // $a3
    // EABI argument layout (PPSSPP Core/HLE/HLE.cpp): arguments 5-8 travel
    // in $t0-$t3, so attr is $t0 here (the option pointer would be $t1).
    const std::uint32_t attr = ctx.gpr[8];

    std::string name;
    std::int32_t result;
    if (!read_guest_string(memory, name_address, 32u, name)) {
        result = static_cast<std::int32_t>(kErrorIllegalAddr);
    } else {
        // A new thread inherits the creator's $gp (the module $gp for threads
        // created by module_start).
        result = kernel.create_thread(name, entry, priority, stack_size, attr, ctx.gpr[28], memory);
    }
    ctx.set_gpr(2, static_cast<std::uint32_t>(result));  // $v0
    return result;
}

std::int32_t hle_start_thread(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                              P3PKernel &kernel) {
    const auto thid = static_cast<std::int32_t>(ctx.gpr[4]);  // $a0
    const std::uint32_t arglen = ctx.gpr[5];                  // $a1
    const std::uint32_t argp = ctx.gpr[6];                    // $a2
    const std::int32_t result = kernel.start_thread(thid, arglen, argp, runtime.memory());
    ctx.set_gpr(2, static_cast<std::uint32_t>(result));  // $v0
    return result;
}

void register_threadman(psprecomp::Runtime &runtime, P3PKernel &kernel) {
    runtime.register_hle(kThreadManLibrary, kNidCreateThread,
                         [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                             hle_create_thread(rt, ctx, kernel);
                         });
    runtime.register_hle(kThreadManLibrary, kNidStartThread,
                         [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                             hle_start_thread(rt, ctx, kernel);
                         });
}

namespace {

// Fake clock for sceKernelGetSystemTimeLow / sceKernelDelayThread. Headless
// runs have no real timer thread; the clock advances by the requested delay
// and by a fixed step per read so guest timing loops always make progress.
std::uint32_t g_fake_time_us = 0;

void set_v0(psprecomp::AllegrexContext &ctx, std::int32_t result) {
    ctx.set_gpr(2, static_cast<std::uint32_t>(result));
}

}  // namespace

bool hle_threadman_extra_dispatch(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                                  P3PKernel &kernel, std::uint32_t nid) {
    psprecomp::GuestMemory &memory = runtime.memory();
    const auto a0 = static_cast<std::int32_t>(ctx.gpr[4]);
    switch (nid) {
        case kNidCreateCallback: {
            std::string name;
            if (!read_guest_string(memory, ctx.gpr[4], 32u, name)) {
                set_v0(ctx, static_cast<std::int32_t>(kErrorIllegalAddr));
                return true;
            }
            set_v0(ctx, kernel.create_callback(name, ctx.gpr[5], ctx.gpr[6]));
            return true;
        }
        case kNidDeleteCallback:
            set_v0(ctx, kernel.delete_callback(a0));
            return true;
        case kNidCheckCallback:
            set_v0(ctx, kernel.check_callbacks());
            return true;
        case kNidCreateEventFlag: {
            std::string name;
            if (!read_guest_string(memory, ctx.gpr[4], 32u, name)) {
                set_v0(ctx, static_cast<std::int32_t>(kErrorIllegalAddr));
                return true;
            }
            set_v0(ctx, kernel.create_event_flag(name, ctx.gpr[5], ctx.gpr[6]));
            return true;
        }
        case kNidSetEventFlag:
            set_v0(ctx, kernel.set_event_flag(a0, ctx.gpr[5]));
            return true;
        case kNidClearEventFlag:
            set_v0(ctx, kernel.clear_event_flag(a0, ctx.gpr[5]));
            return true;
        case kNidDeleteEventFlag:
            set_v0(ctx, kernel.delete_event_flag(a0));
            return true;
        case kNidWaitEventFlag:
        case kNidWaitEventFlagCB: {
            const std::uint32_t wait_pattern = ctx.gpr[5];
            const std::uint32_t mode = ctx.gpr[6];
            const std::uint32_t result_addr = ctx.gpr[7];
            std::uint32_t pattern = 0;
            if (!kernel.event_flag_pattern(a0, pattern)) {
                set_v0(ctx, static_cast<std::int32_t>(kErrorUnknownEvfId));
                return true;
            }
            const bool satisfied = (mode & 1u) != 0u ? (pattern & wait_pattern) != 0u
                                                     : (pattern & wait_pattern) == wait_pattern;
            if (result_addr != 0u)
                memory.store32(result_addr, pattern);
            if (satisfied) {
                if ((mode & 0x10u) != 0u)
                    kernel.clear_event_flag(a0, 0xFFFFFFFFu);      // WAITCLEARALL
                else if ((mode & 0x20u) != 0u)
                    kernel.clear_event_flag(a0, wait_pattern);     // WAITCLEARPATTERN
            } else {
                // Headless approximation: a blocking wait cannot be honoured
                // inside one dispatch; log it (once per uid+pattern) and
                // report success with the current pattern.
                static std::set<std::pair<std::int32_t, std::uint32_t>> logged;
                if (logged.emplace(a0, wait_pattern).second)
                    std::fprintf(stderr,
                                 "[p3p] wait event flag uid %d would block (pattern %s, "
                                 "wait %s, mode %s); returning success (headless)\n",
                                 a0, psprecomp::hex32(pattern).c_str(),
                                 psprecomp::hex32(wait_pattern).c_str(),
                                 psprecomp::hex32(mode).c_str());
            }
            set_v0(ctx, 0);
            return true;
        }
        case kNidChangeCurrentThreadAttr:
            set_v0(ctx, kernel.change_current_thread_attr(ctx.gpr[4], ctx.gpr[5]));
            return true;
        case kNidGetSystemTimeLow:
            g_fake_time_us += 100u;
            set_v0(ctx, static_cast<std::int32_t>(g_fake_time_us));
            return true;
        case kNidDelayThread:
        case kNidDelayThreadCB:
            g_fake_time_us += ctx.gpr[4];
            set_v0(ctx, 0);
            return true;
        case kNidExitThread:
            if (!kernel.exit_current_and_schedule_next(ctx))
                runtime.stop("p3p: sceKernelExitThread with no ready thread remaining");
            return true;
        default:
            return false;
    }
}

}  // namespace p3p

