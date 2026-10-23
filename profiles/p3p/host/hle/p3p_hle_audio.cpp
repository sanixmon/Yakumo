// P3P profile HLE: sceAudio (Gate 5C). See the header for the policy.

#include "hle/p3p_hle_audio.hpp"

namespace p3p {
namespace {

void output_blocking(psprecomp::AllegrexContext &ctx, P3PKernel &kernel) {
    // The call counts as finished (samples drain headless), but like the
    // reference kernel the caller still blocks for the drain: preset the
    // result and resume address, then sleep to the next frame boundary on
    // the kernel's fake clock (Gate 5C). Sleeping rather than yielding is
    // what keeps a high-priority audio thread from starving the workers
    // queued behind it.
    ctx.set_gpr(2, 0u);
    ctx.pc = ctx.gpr[31];
    constexpr std::uint64_t kFrameUs = 16667u;  // 59.94 Hz frame
    const std::uint64_t now = kernel.now_us();
    kernel.sleep_current(ctx, kFrameUs - (now % kFrameUs));
}

}  // namespace

bool hle_audio_dispatch(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                        P3PKernel &kernel, std::uint32_t nid) {
    (void)runtime;
    const std::int32_t a0 = static_cast<std::int32_t>(ctx.gpr[4]);
    switch (nid) {
        case kNidAudioChReserve:
            // sceAudioChReserve(chan, sampleCount, format): a specific
            // channel, or -1 for any free one. Returns the channel index.
            ctx.set_gpr(2, static_cast<std::uint32_t>(a0 < 0 ? 0 : a0));
            return true;
        case kNidAudioChangeChannelConfig:
        case kNidAudioSetChannelDataLen:
        case kNidAudioOutput2Reserve:
        case kNidAudioOutput2ChangeLength:
        case kNidAudioOutput2Release:
            ctx.set_gpr(2, 0u);
            return true;
        case kNidAudioOutput2GetRestSample:
            ctx.set_gpr(2, 0u);  // nothing left: instant drain
            return true;
        case kNidAudioOutputBlocking:
        case kNidAudioOutputPannedBlocking:
        case kNidAudioOutput2OutputBlocking:
        case kNidAudioSRCOutputBlocking:
            output_blocking(ctx, kernel);
            return true;
        default:
            return false;
    }
}

}  // namespace p3p
