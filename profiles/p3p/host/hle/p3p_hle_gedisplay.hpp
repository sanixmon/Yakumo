// sceGe_user + sceDisplay HLE for the P3P profile (Gate 5B).
//
// Headless display model: there is no GPU or scan-out; the state recorded
// here is the smallest model that makes init's queries truthful — the EDRAM
// address/size are the real hardware constants, display lists complete
// immediately (ListSync/DrawSync report done), the frame buffer record
// echoes what the game set, and the vblank counter advances on every wait
// so guest frame loops progress.
#pragma once

#include <cstdint>
#include <string>

namespace psprecomp {
class Runtime;
class GuestMemory;
struct AllegrexContext;
}  // namespace psprecomp

namespace p3p {

inline constexpr const char *kGeLibrary = "sceGe_user";
inline constexpr const char *kDisplayLibrary = "sceDisplay";

class P3PKernel;

void gedisplay_reset_state();

// Gate 6 WP2: dumps the current display framebuffer as a PNG.
bool gedisplay_dump_frame(psprecomp::GuestMemory &memory, const std::string &path);

// Registers a path for periodic mid-run dumps (every 240 vblanks).
void gedisplay_set_frame_dump_path(const std::string &path);

// Returns true when the NID was handled (result in $v0 / $f0). `kernel`
// may be null (trace runner). With a kernel, vblank waits are scheduling
// points (Gate 5C): the call completes and the CPU yields, so `ctx` may
// hold another thread's context on return.
bool hle_gedisplay_dispatch(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                            P3PKernel *kernel, const std::string &library, std::uint32_t nid);

// Cumulative rasterized pixels across all GE lists so far (the
// liveness detector diffs this across its windows).
std::uint32_t hle_gedisplay_pixels_total();

}  // namespace p3p
