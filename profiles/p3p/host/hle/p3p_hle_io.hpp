// P3P IoFileMgrForUser HLE (Gate 5).
//
// File I/O for the P3P profile, registered with the profile dispatcher
// (p3p_profile.cpp) for every import of library "IoFileMgrForUser".
// Paths are resolved through Runtime::translate_path against the game root
// (Runtime::set_game_root), with a case-insensitive fallback walk because
// UMD/ISO9660 names and the game's path literals do not always agree on
// case while the host filesystem is case-sensitive.
//
// Design notes:
//  - disc0:/umd0: devices are read-only; ms0:/host0: are writable. The
//    writable area lives under the same single game root (ms0:/PSP/...
//    maps to <game_root>/PSP/...), matching translate_path's single-root
//    model.
//  - The CPK archives are parsed by the game's own CRI middleware running
//    as recompiled guest code; this module only serves bytes.
//  - The async family (7 entry points) completes synchronously: the
//    operation is performed immediately and its result is stored on the
//    handle, so sceIoPollAsync/sceIoWaitAsync always observe completion.
//    This matches PPSSPP's observable behaviour when an operation finishes
//    before the first poll and cannot spin a guest polling loop forever.
//  - Error codes follow PPSSPP (Core/HLE/ErrorCodes.h and its errno-style
//    mapping 0x80010000+errno for filesystem errors).
//  - State is file-scope (single-instance host), mirroring p3p_profile.cpp.
#pragma once

#include <cstdint>

namespace psprecomp {
class Runtime;
struct AllegrexContext;
}  // namespace psprecomp

namespace p3p {

inline constexpr const char *kIoFileMgrLibrary = "IoFileMgrForUser";

inline constexpr std::uint32_t kNidIoOpen = 0x109F50BCu;
inline constexpr std::uint32_t kNidIoRead = 0x6A638D83u;
inline constexpr std::uint32_t kNidIoWrite = 0x42EC03ACu;
inline constexpr std::uint32_t kNidIoLseek = 0x27EB27B8u;
inline constexpr std::uint32_t kNidIoClose = 0x810C4BC3u;
inline constexpr std::uint32_t kNidIoGetstat = 0xACE946E8u;
inline constexpr std::uint32_t kNidIoDopen = 0xB29DDF9Cu;
inline constexpr std::uint32_t kNidIoDread = 0xE3EB004Cu;
inline constexpr std::uint32_t kNidIoDclose = 0xEB092469u;
inline constexpr std::uint32_t kNidIoRename = 0x779103A0u;
inline constexpr std::uint32_t kNidIoDevctl = 0x54F5FB11u;
inline constexpr std::uint32_t kNidIoOpenAsync = 0x89AA9906u;
inline constexpr std::uint32_t kNidIoReadAsync = 0xA0B5A7C2u;
inline constexpr std::uint32_t kNidIoLseekAsync = 0x71B19E77u;
inline constexpr std::uint32_t kNidIoCloseAsync = 0xFF5940B6u;
inline constexpr std::uint32_t kNidIoPollAsync = 0x3251EA56u;
inline constexpr std::uint32_t kNidIoWaitAsync = 0xE23EEC33u;
inline constexpr std::uint32_t kNidIoWaitAsyncCB = 0x35DBD746u;
inline constexpr std::uint32_t kNidIoSync = 0xAB96437Fu;
inline constexpr std::uint32_t kNidIoChangeAsyncPriority = 0xB293727Fu;

// Clears all open file/dir handles and async results (host/test lifecycle).
void io_reset_state();

// Handles one IoFileMgrForUser import. Returns true when the NID is a known
// IoFileMgr entry point (result written to $v0/$v1), false to let the
// profile fall back to its logged return-0 stub.
bool hle_io_dispatch(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                     std::uint32_t nid);

}  // namespace p3p
