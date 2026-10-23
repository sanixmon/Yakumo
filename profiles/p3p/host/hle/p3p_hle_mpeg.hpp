#pragma once

#include <cstdint>

namespace psprecomp {
class Runtime;
struct AllegrexContext;
}  // namespace psprecomp

namespace p3p {

class P3PKernel;

// sceMpeg library HLE (Gate 6 follow-up, multimedia package): the
// bring-up surface the opening-movie path uses -- library init,
// handle/ringbuffer sizing and construction, stream registration --
// modelled on PPSSPP's sceMpeg. There is no MPEG demuxer/decoder
// behind it yet: the AU getters honestly report NO_DATA. Returns true
// when the NID was handled.
bool hle_mpeg_dispatch(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                       P3PKernel *kernel, std::uint32_t nid);

// sceMpegRingbufferPut with a registered callback detours the
// calling thread into the guest callback and only completes when
// the callback returns to this trampoline address (an unmapped PC
// the runtime's missing-function hook routes here). Returns true
// when the PC was the trampoline and the pending Put was resumed.
bool hle_mpeg_trampoline(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                         std::uint32_t pc);

// Cumulative data-movement counters for the liveness detector:
// packets the guest enqueued into ringbuffers, and AUs handed out.
struct MpegProgress {
    std::uint64_t packets_put = 0;
    std::uint64_t au_video = 0;
    std::uint64_t au_audio = 0;
};
MpegProgress hle_mpeg_progress();

}  // namespace p3p
