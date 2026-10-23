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
struct AllegrexContext;
}  // namespace psprecomp

namespace p3p {

inline constexpr const char *kGeLibrary = "sceGe_user";
inline constexpr const char *kDisplayLibrary = "sceDisplay";

void gedisplay_reset_state();

// Returns true when the NID was handled (result in $v0 / $f0).
bool hle_gedisplay_dispatch(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                            const std::string &library, std::uint32_t nid);

}  // namespace p3p
