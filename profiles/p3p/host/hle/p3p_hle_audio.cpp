// P3P profile HLE: sceAudio (Gate 5C). See the header for the policy.

#include "hle/p3p_hle_audio.hpp"

#include <algorithm>
#include <cstdio>

namespace p3p {
namespace {

// One output channel's drain state: the configured buffer length in
// samples and the fake-clock instant at which everything queued so far
// has finished playing. Samples drain at the hardware rate.
struct ChanState {
    std::uint32_t sample_count = 0;
    std::uint64_t drain_end_us = 0;
    bool reserved = false;
};

constexpr std::uint32_t kSampleRate = 44100u;
constexpr std::uint64_t kFrameUs = 16667u;  // 59.94 Hz frame

ChanState g_chans[8];
ChanState g_output2;
ChanState g_src;

std::uint32_t rest_samples(const ChanState &st, const P3PKernel &kernel) {
    const std::uint64_t now = kernel.now_us();
    if (st.drain_end_us <= now)
        return 0u;
    const std::uint64_t remaining_us = st.drain_end_us - now;
    return static_cast<std::uint32_t>((remaining_us * kSampleRate + 999999u) / 1000000u);
}

// Queues one buffer on the channel and parks the caller until it has
// played out. The result and resume address are preset before sleeping
// (the Gate 5C blocking-call treatment). When no length was ever
// configured for the channel, fall back to the historical behaviour of
// sleeping to the next frame boundary.
void output_blocking(ChanState &st, psprecomp::AllegrexContext &ctx, P3PKernel &kernel,
                     const char *what, std::int32_t chan) {
    (void)what;
    (void)chan;
    ctx.set_gpr(2, 0u);
    ctx.pc = ctx.gpr[31];
    const std::uint64_t now = kernel.now_us();
    if (st.sample_count == 0u) {
        kernel.sleep_current(ctx, kFrameUs - (now % kFrameUs));
        return;
    }
    const std::uint64_t start = std::max(now, st.drain_end_us);
    const std::uint64_t duration_us =
        static_cast<std::uint64_t>(st.sample_count) * 1000000u / kSampleRate;
    st.drain_end_us = start + duration_us;
    kernel.sleep_current(ctx, st.drain_end_us - now);
}

}  // namespace


bool hle_audio_dispatch(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                        P3PKernel &kernel, std::uint32_t nid) {
    (void)runtime;
    const std::int32_t a0 = static_cast<std::int32_t>(ctx.gpr[4]);
    const std::uint32_t a1 = ctx.gpr[5];
    switch (nid) {
        case kNidAudioChReserve: {
            // sceAudioChReserve(chan, sampleCount, format): reserve a
            // specific channel, or with chan == -1 the first free one.
            // Distinct subsystems (the SAS renderer, the CRI wave
            // output, a movie player's wave library) each hold their
            // own channel and drain in parallel; handing every caller
            // channel 0 would serialize all of them onto one queue and
            // stretch each subsystem's per-buffer wait by the number of
            // sharers -- enough to desync a media player whose video
            // side is vblank-paced.
            constexpr std::uint32_t kErrorInvalidChannel = 0x80260003u;
            std::int32_t chan = a0;
            if (chan < 0) {
                chan = -1;
                for (std::int32_t i = 0; i < 8; ++i) {
                    if (!g_chans[i].reserved) {
                        chan = i;
                        break;
                    }
                }
                if (chan < 0) {
                    ctx.set_gpr(2, kErrorInvalidChannel);
                    return true;
                }
            }
            if (chan >= 8 || g_chans[chan].reserved) {
                ctx.set_gpr(2, kErrorInvalidChannel);
                return true;
            }
            g_chans[chan].reserved = true;
            g_chans[chan].sample_count = a1;
            g_chans[chan].drain_end_us = kernel.now_us();
            ctx.set_gpr(2, static_cast<std::uint32_t>(chan));
            return true;
        }
        case kNidAudioChRelease:
            if (a0 >= 0 && a0 < 8 && g_chans[a0].reserved) {
                g_chans[a0].reserved = false;
                g_chans[a0].sample_count = 0;
                g_chans[a0].drain_end_us = kernel.now_us();
                ctx.set_gpr(2, 0u);
            } else {
                ctx.set_gpr(2, 0x80260003u);
            }
            return true;
        case kNidAudioSetChannelDataLen:
            if (a0 >= 0 && a0 < 8)
                g_chans[a0].sample_count = a1;
            ctx.set_gpr(2, 0u);
            return true;
        case kNidAudioGetChannelRestLength:
            ctx.set_gpr(2, (a0 >= 0 && a0 < 8) ? rest_samples(g_chans[a0], kernel) : 0u);
            return true;
        case kNidAudioChangeChannelConfig:
            ctx.set_gpr(2, 0u);
            return true;
        case kNidAudioOutputBlocking:
        case kNidAudioOutputPannedBlocking: {
            ChanState &st = (a0 >= 0 && a0 < 8) ? g_chans[a0] : g_output2;
            output_blocking(st, ctx, kernel,
                            nid == kNidAudioOutputBlocking ? "OutputBlocking"
                                                             : "OutputPannedBlocking",
                            a0);
            return true;
        }
        case kNidAudioOutput2Reserve:
            // sceAudioOutput2Reserve(sampleCount): the single Output2
            // channel's buffer length.
            g_output2.sample_count = static_cast<std::uint32_t>(a0);
            g_output2.drain_end_us = kernel.now_us();
            ctx.set_gpr(2, 0u);
            return true;
        case kNidAudioOutput2ChangeLength:
            g_output2.sample_count = static_cast<std::uint32_t>(a0);
            ctx.set_gpr(2, 0u);
            return true;
        case kNidAudioOutput2Release:
            g_output2 = ChanState{};
            ctx.set_gpr(2, 0u);
            return true;
        case kNidAudioOutput2GetRestSample:
            ctx.set_gpr(2, rest_samples(g_output2, kernel));
            return true;
        case kNidAudioOutput2OutputBlocking:
            output_blocking(g_output2, ctx, kernel, "Output2OutputBlocking", -2);
            return true;
        case kNidAudioSRCOutputBlocking:
            output_blocking(g_src, ctx, kernel, "SRCOutputBlocking", -3);
            return true;
        default:
            return false;
    }
}

}  // namespace p3p
