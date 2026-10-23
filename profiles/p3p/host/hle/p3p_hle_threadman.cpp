#include "p3p_hle_threadman.hpp"

#include "kernel/p3p_kernel.hpp"
#include "psprecomp/guest_memory.hpp"

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
    // Stack-passed arguments (PSP ABI): attr at $sp+16, option at $sp+20.
    std::uint32_t attr = 0;
    const std::uint32_t sp = ctx.gpr[29];
    if (memory.contains(sp + 16u, 4u))
        attr = memory.load32(sp + 16u);

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

}  // namespace p3p
