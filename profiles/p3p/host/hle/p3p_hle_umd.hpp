// P3P profile HLE: sceUmdUser (Gate 5C follow-up).
//
// The host serves the user's own disc dump through the profile game
// root, so the drive model is fixed: a disc is inserted, and once the
// game activates it, it is ready and readable. The values follow the
// reference (PPSSPP Core/HLE/sceUmd.cpp): sceUmdCheckMedium returns 1
// with a disc in — the Gate 5 stub era returned 0 for every UMD call,
// which told the game there was NO disc and it never queued a read.

#pragma once

#include "psprecomp/runtime.hpp"

#include <cstdint>

namespace p3p {

// sceUmdUser NIDs (PPSSPP Core/HLE/sceUmd.cpp).
inline constexpr std::uint32_t kNidUmdGetErrorStat = 0x20628E6Fu;
inline constexpr std::uint32_t kNidUmdCheckMedium = 0x46EBB729u;
inline constexpr std::uint32_t kNidUmdWaitDriveStatCB = 0x4A9E5E29u;
inline constexpr std::uint32_t kNidUmdCancelWaitDriveStat = 0x6AF9B50Au;
inline constexpr std::uint32_t kNidUmdGetDriveStat = 0x6B4A146Cu;
inline constexpr std::uint32_t kNidUmdWaitDriveStat = 0x8EF08FCEu;
inline constexpr std::uint32_t kNidUmdRegisterUMDCallBack = 0xAEE7404Du;
inline constexpr std::uint32_t kNidUmdUnRegisterUMDCallBack = 0xBD2BDE07u;
inline constexpr std::uint32_t kNidUmdActivate = 0xC6183D47u;

void umd_reset_state();

// Handles one sceUmdUser call. Returns true when the NID belongs to this
// module (result in $v0).
bool hle_umd_dispatch(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                      std::uint32_t nid);

}  // namespace p3p
