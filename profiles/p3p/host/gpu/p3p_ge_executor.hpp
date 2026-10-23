// P3P profile GE display-list executor (Gate 6, WP1).
//
// Walks a submitted display list in guest memory and maintains the GE
// register state, mirroring the reference semantics (PPSSPP GPU/
// ge_constants.h + GPUCommon; address convention per the working
// in-repo implementation in profiles/mhp3rd/host/gpu/ge_state.cpp):
//
//   BASE       -> base_extended = (data & 0x000F0000) << 8  (address bits 24-27)
//   OFFSETADDR -> offset_address = data << 8
//   relative(arg) = base_extended + offset_address + arg
//
// The executor produces decoded draw calls (primitive + vertex/index
// source + a full register snapshot) for the rasterizer back end
// (WP2), and honours the list mechanics the HLE must answer honestly:
// stall addresses, CALL/RET/JUMP control flow, SIGNAL delivery, and
// FINISH/END completion. It renders nothing itself.

#pragma once

#include "psprecomp/guest_memory.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace p3p::ge {

// GE opcodes interpreted by the executor (PPSSPP GPU/ge_constants.h).
inline constexpr std::uint32_t kCmdNop = 0x00u;
inline constexpr std::uint32_t kCmdVaddr = 0x01u;
inline constexpr std::uint32_t kCmdIaddr = 0x02u;
inline constexpr std::uint32_t kCmdPrim = 0x04u;
inline constexpr std::uint32_t kCmdBezier = 0x05u;
inline constexpr std::uint32_t kCmdSpline = 0x06u;
inline constexpr std::uint32_t kCmdBoundingBox = 0x07u;
inline constexpr std::uint32_t kCmdJump = 0x08u;
inline constexpr std::uint32_t kCmdBJump = 0x09u;
inline constexpr std::uint32_t kCmdCall = 0x0Au;
inline constexpr std::uint32_t kCmdRet = 0x0Bu;
inline constexpr std::uint32_t kCmdEnd = 0x0Cu;
inline constexpr std::uint32_t kCmdSignal = 0x0Eu;
inline constexpr std::uint32_t kCmdFinish = 0x0Fu;
inline constexpr std::uint32_t kCmdBase = 0x10u;
inline constexpr std::uint32_t kCmdVertexType = 0x12u;
inline constexpr std::uint32_t kCmdOffsetAddr = 0x13u;
inline constexpr std::uint32_t kCmdOrigin = 0x14u;
inline constexpr std::uint32_t kCmdTransferStart = 0xEAu;

// Primitive types (GE_PRIM_*), PRIM arg bits 16-18.
inline constexpr std::uint32_t kPrimPoints = 0u;
inline constexpr std::uint32_t kPrimLines = 1u;
inline constexpr std::uint32_t kPrimLineStrip = 2u;
inline constexpr std::uint32_t kPrimTriangles = 3u;
inline constexpr std::uint32_t kPrimTriangleStrip = 4u;
inline constexpr std::uint32_t kPrimTriangleFan = 5u;
inline constexpr std::uint32_t kPrimSprites = 6u;

// One decoded draw: a PRIM (or a VRAM transfer) plus the register state
// as it stood at that point in the list.
struct GeDrawCall {
    bool is_transfer = false;
    std::uint32_t prim_type = 0;      // kPrim* (unused for transfers)
    std::uint32_t vertex_count = 0;   // PRIM arg low 16 bits
    std::uint32_t vertex_address = 0;
    std::uint32_t index_address = 0;
    std::uint32_t vertex_type = 0;
    std::array<std::uint32_t, 256> regs{};
};

// The GE register machine. `regs` holds the raw 24-bit argument of the
// most recent command for every opcode (the hardware register file);
// the executor additionally tracks the resolved address registers.
struct GeState {
    std::array<std::uint32_t, 256> regs{};
    std::uint32_t base_extended = 0;
    std::uint32_t offset_address = 0;
    std::uint32_t vertex_address = 0;
    std::uint32_t index_address = 0;

    [[nodiscard]] std::uint32_t relative(std::uint32_t arg) const noexcept {
        return base_extended + offset_address + arg;
    }
    [[nodiscard]] std::uint32_t vertex_type() const noexcept { return regs[kCmdVertexType]; }
};

struct GeExecStats {
    std::uint32_t commands = 0;
    std::uint32_t draws = 0;
    std::uint32_t transfers = 0;
    std::uint32_t unsupported_draws = 0;  // BEZIER/SPLINE/BOUNDINGBOX
    std::uint32_t signals = 0;
    std::uint32_t calls = 0;
    std::uint32_t unknown_opcodes = 0;
    std::uint32_t finishes = 0;
};

enum class GeRunStatus : std::uint32_t { Done = 0, Stalled = 1, Error = 2 };

struct GeRunResult {
    GeRunStatus status = GeRunStatus::Done;
    std::uint32_t pc = 0;  // resume address when Stalled
};

// Invoked for every SIGNAL command: (behavior, data16). The HLE wires
// this to the registered GE callback.
using GeSignalHandler =
    std::function<void(std::uint32_t behavior, std::uint32_t data)>;

// Executes a display list starting at `pc` until END, the stall
// address, or an error. `state` carries across calls (one GE, many
// lists; a stalled list resumes with the same state). Draws append to
// `draws`. A run is capped (kMaxCommands) so a corrupt list cannot
// loop forever.
inline constexpr std::uint32_t kMaxCommandsPerRun = 1u << 20;

GeRunResult ge_execute(psprecomp::GuestMemory &memory, std::uint32_t pc,
                       std::uint32_t stall_address, GeState &state,
                       std::vector<GeDrawCall> &draws, GeExecStats &stats,
                       const GeSignalHandler &on_signal);

}  // namespace p3p::ge
