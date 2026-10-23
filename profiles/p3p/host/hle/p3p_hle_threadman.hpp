#pragma once

// ThreadManForUser HLE bindings for the P3P profile (Gate 4B scope:
// sceKernelCreateThread + sceKernelStartThread; Gate 5B adds callbacks,
// event flags, thread attributes, and the fake-clock time/delay calls).

#include "psprecomp/runtime.hpp"

namespace p3p {

class P3PKernel;

inline constexpr std::uint32_t kNidCreateThread = 0x446D8DE6u;
inline constexpr std::uint32_t kNidStartThread = 0xF475845Du;
inline constexpr std::uint32_t kNidCreateCallback = 0xE81CAF8Fu;
inline constexpr std::uint32_t kNidDeleteCallback = 0xEDBA5844u;
inline constexpr std::uint32_t kNidCheckCallback = 0x349D6D6Cu;
inline constexpr std::uint32_t kNidCreateEventFlag = 0x55C20A00u;
inline constexpr std::uint32_t kNidCreateMutex = 0xB7D098C6u;
inline constexpr std::uint32_t kNidDeleteMutex = 0xF8170FBEu;
inline constexpr std::uint32_t kNidLockMutex = 0xB011B11Fu;
inline constexpr std::uint32_t kNidLockMutexCB = 0x5BF4DD27u;
inline constexpr std::uint32_t kNidUnlockMutex = 0x6B30100Fu;
inline constexpr std::uint32_t kNidSetEventFlag = 0x1FB15A32u;
inline constexpr std::uint32_t kNidClearEventFlag = 0x812346E4u;
inline constexpr std::uint32_t kNidDeleteEventFlag = 0xEF9E4C70u;
inline constexpr std::uint32_t kNidWaitEventFlag = 0x402FCF22u;
inline constexpr std::uint32_t kNidWaitEventFlagCB = 0x328C546Au;
inline constexpr std::uint32_t kNidChangeCurrentThreadAttr = 0xEA748E31u;
inline constexpr std::uint32_t kNidGetSystemTimeLow = 0x369ED59Du;
inline constexpr std::uint32_t kNidDelayThread = 0xCEADEB47u;
inline constexpr std::uint32_t kNidDelayThreadCB = 0x68DA9E36u;
inline constexpr std::uint32_t kNidExitThread = 0xAA73C935u;
inline constexpr const char *kThreadManLibrary = "ThreadManForUser";

// HLE cores, separated from the registration lambdas so unit tests can call
// them directly. Each writes its return value to $v0 and also returns it.
std::int32_t hle_create_thread(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                               P3PKernel &kernel);
std::int32_t hle_start_thread(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                              P3PKernel &kernel);

// Registers the ThreadMan bindings on the runtime, replacing any stubs.
void register_threadman(psprecomp::Runtime &runtime, P3PKernel &kernel);

// Gate 5B ThreadMan surface (callbacks, event flags, attributes, fake
// clock). Returns true when the NID was handled (result in $v0).
bool hle_threadman_extra_dispatch(psprecomp::Runtime &runtime,
                                  psprecomp::AllegrexContext &ctx, P3PKernel &kernel,
                                  std::uint32_t nid);

}  // namespace p3p
