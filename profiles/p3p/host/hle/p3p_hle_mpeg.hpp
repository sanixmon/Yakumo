#pragma once

#include <cstdint>

namespace psprecomp {
class Runtime;
struct AllegrexContext;
}  // namespace psprecomp

namespace p3p {

// sceMpeg library HLE (Gate 6 follow-up, multimedia package): the
// bring-up surface the opening-movie path uses -- library init,
// handle/ringbuffer sizing and construction, stream registration --
// modelled on PPSSPP's sceMpeg. There is no MPEG demuxer/decoder
// behind it yet: the AU getters honestly report NO_DATA. Returns true
// when the NID was handled.
bool hle_mpeg_dispatch(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                       std::uint32_t nid);

}  // namespace p3p
