#pragma once

// ThreadManForUser HLE bindings for the P3P profile (Gate 4B scope:
// sceKernelCreateThread + sceKernelStartThread only).

#include "psprecomp/runtime.hpp"

namespace p3p {

class P3PKernel;

inline constexpr std::uint32_t kNidCreateThread = 0x446D8DE6u;
inline constexpr std::uint32_t kNidStartThread = 0xF475845Du;
inline constexpr const char *kThreadManLibrary = "ThreadManForUser";

// HLE cores, separated from the registration lambdas so unit tests can call
// them directly. Each writes its return value to $v0 and also returns it.
std::int32_t hle_create_thread(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                               P3PKernel &kernel);
std::int32_t hle_start_thread(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                              P3PKernel &kernel);

// Registers the ThreadMan bindings on the runtime, replacing any stubs.
void register_threadman(psprecomp::Runtime &runtime, P3PKernel &kernel);

}  // namespace p3p
