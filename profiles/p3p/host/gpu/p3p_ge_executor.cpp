// P3P profile GE display-list executor (Gate 6, WP1). See the header.

#include "gpu/p3p_ge_executor.hpp"

namespace p3p::ge {
namespace {

struct CallFrame {
    std::uint32_t return_pc;
    std::uint32_t base_extended;
    std::uint32_t offset_address;
};

bool is_known_state_opcode(std::uint32_t op) noexcept {
    // Register-file commands the executor stores without interpretation.
    // Ranges follow PPSSPP GPU/ge_constants.h: enables (0x17-0x28),
    // matrices/lights (0x2A-0x9B), texture/framebuffer (0x9C-0xCB),
    // test/blend/transfer setup (0xD0-0xEE), immediate vertex (0xF0-0xF9).
    return (op >= 0x15u && op <= 0x28u) || (op >= 0x2Au && op <= 0x9Bu) ||
           (op >= 0x9Cu && op <= 0xCBu) || (op >= 0xCCu && op <= 0xEEu) ||
           (op >= 0xF0u && op <= 0xF9u) || op == kCmdVertexType;
}

}  // namespace

GeRunResult ge_execute(psprecomp::GuestMemory &memory, std::uint32_t pc,
                       std::uint32_t stall_address, GeState &state,
                       std::vector<GeDrawCall> &draws, GeExecStats &stats,
                       const GeSignalHandler &on_signal) {
    std::array<CallFrame, 32> call_stack{};
    std::size_t call_depth = 0;

    for (std::uint32_t step = 0; step < kMaxCommandsPerRun; ++step) {
        if (stall_address != 0u && pc == stall_address)
            return {GeRunStatus::Stalled, pc};
        if (!memory.contains(pc, 4u))
            return {GeRunStatus::Error, pc};
        const std::uint32_t word = memory.load32(pc);
        const std::uint32_t command_pc = pc;
        pc += 4u;
        const std::uint32_t op = word >> 24;
        const std::uint32_t arg = word & 0x00FFFFFFu;
        state.regs[op] = arg;
        ++stats.commands;

        switch (op) {
            case kCmdNop:
                break;
            case kCmdVaddr:
                state.vertex_address = state.relative(arg);
                break;
            case kCmdIaddr:
                state.index_address = state.relative(arg);
                break;
            case kCmdBase:
                state.base_extended = (arg & 0x000F0000u) << 8u;
                break;
            case kCmdOffsetAddr:
                state.offset_address = arg << 8u;
                break;
            case kCmdOrigin:
                // Subsequent relative addresses anchor at this command.
                state.offset_address = command_pc;
                break;
            case kCmdPrim: {
                GeDrawCall draw;
                draw.prim_type = (arg >> 16) & 7u;
                draw.vertex_count = arg & 0xFFFFu;
                draw.vertex_address = state.vertex_address;
                draw.index_address = state.index_address;
                draw.vertex_type = state.vertex_type();
                draw.regs = state.regs;
                draws.push_back(draw);
                ++stats.draws;
                break;
            }
            case kCmdTransferStart: {
                GeDrawCall draw;
                draw.is_transfer = true;
                draw.regs = state.regs;
                draws.push_back(draw);
                ++stats.transfers;
                break;
            }
            case kCmdBezier:
            case kCmdSpline:
            case kCmdBoundingBox:
                ++stats.unsupported_draws;
                break;
            case kCmdJump:
            case kCmdBJump:
                // BJUMP is gated on a bounding-box visibility test the
                // software back end always passes at this stage.
                pc = state.relative(arg & 0x00FFFFFCu);
                break;
            case kCmdCall: {
                if (call_depth >= call_stack.size())
                    return {GeRunStatus::Error, command_pc};
                call_stack[call_depth++] = {pc, state.base_extended, state.offset_address};
                pc = state.relative(arg & 0x00FFFFFCu);
                ++stats.calls;
                break;
            }
            case kCmdRet: {
                if (call_depth == 0)
                    return {GeRunStatus::Done, pc};
                const CallFrame frame = call_stack[--call_depth];
                pc = frame.return_pc;
                state.base_extended = frame.base_extended;
                state.offset_address = frame.offset_address;
                break;
            }
            case kCmdEnd: {
                if (call_depth == 0)
                    return {GeRunStatus::Done, pc};
                const CallFrame frame = call_stack[--call_depth];
                pc = frame.return_pc;
                state.base_extended = frame.base_extended;
                state.offset_address = frame.offset_address;
                break;
            }
            case kCmdSignal: {
                ++stats.signals;
                if (on_signal)
                    on_signal((arg >> 16) & 0xFFu, arg & 0xFFFFu);
                break;
            }
            case kCmdFinish:
                ++stats.finishes;
                break;
            default:
                if (!is_known_state_opcode(op))
                    ++stats.unknown_opcodes;
                break;
        }
    }
    return {GeRunStatus::Error, pc};
}

}  // namespace p3p::ge
