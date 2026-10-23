// P3P profile HLE: sceAudio (Gate 5C).
//
// The channel/output calls the boot path uses. Headless policy: there is
// no audio device, so output completes instantly — but the *Blocking
// output calls are still scheduling points, exactly like the reference
// kernel: a thread that outputs audio blocks until its samples drain,
// which is what lets lower-priority threads (e.g. the CRI file thread)
// run. Each blocking output therefore presets its result and sleeps on
// the kernel's fake clock (the Gate 5C DelayThread treatment).

#pragma once

#include "kernel/p3p_kernel.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdint>

namespace p3p {

// sceAudio NIDs (PPSSPP Core/HLE/sceAudio.cpp).
inline constexpr std::uint32_t kNidAudioChReserve = 0x5EC81C55u;
inline constexpr std::uint32_t kNidAudioChangeChannelConfig = 0x95FD0C2Du;
inline constexpr std::uint32_t kNidAudioSetChannelDataLen = 0xCB2E439Eu;
inline constexpr std::uint32_t kNidAudioOutputBlocking = 0x136CAF51u;
inline constexpr std::uint32_t kNidAudioOutputPannedBlocking = 0x13F592BCu;
inline constexpr std::uint32_t kNidAudioOutput2Reserve = 0x01562BA3u;
inline constexpr std::uint32_t kNidAudioOutput2OutputBlocking = 0x2D53F36Eu;
inline constexpr std::uint32_t kNidAudioOutput2ChangeLength = 0x63F2889Cu;
inline constexpr std::uint32_t kNidAudioOutput2GetRestSample = 0x647CEF33u;
inline constexpr std::uint32_t kNidAudioOutput2Release = 0x43196845u;
inline constexpr std::uint32_t kNidAudioSRCOutputBlocking = 0xE0727056u;

// Handles one sceAudio call. Returns true when the NID belongs to this
// module. After a blocking output, `ctx` may hold another thread's
// context (a yield happened); callers must not touch $v0/$pc afterwards.
bool hle_audio_dispatch(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                        P3PKernel &kernel, std::uint32_t nid);

}  // namespace p3p
