// Unit tests for the P3P Gate 6 WP1 GE display-list executor:
// state/address resolution, PRIM recording, control flow, stall/resume,
// SIGNAL delivery, transfers, and the runaway cap.

#include "gpu/p3p_ge_executor.hpp"

#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

std::uint32_t cmd(std::uint32_t op, std::uint32_t arg) { return (op << 24) | (arg & 0x00FFFFFFu); }

}  // namespace

int main() {
    using namespace p3p::ge;

    // --- State, address resolution, one sprite draw ---
    {
        psprecomp::Runtime runtime(32u * 1024u * 1024u);
        auto &mem = runtime.memory();
        const std::uint32_t list = 0x08810000u;
        mem.store32(list + 0, cmd(kCmdBase, 0x00080000u));        // base_ext = 0x08000000
        mem.store32(list + 4, cmd(kCmdOffsetAddr, 0x00000100u));  // offset = 0x10000
        mem.store32(list + 8, cmd(kCmdVaddr, 0x00002000u));       // vaddr = 0x08012000
        mem.store32(list + 12, cmd(kCmdVertexType, 0x00000012u));
        mem.store32(list + 16, cmd(kCmdPrim, (6u << 16) | 4u));   // sprites x4
        mem.store32(list + 20, cmd(kCmdFinish, 0u));
        mem.store32(list + 24, cmd(kCmdEnd, 0u));

        GeState state;
        std::vector<GeDrawCall> draws;
        GeExecStats stats;
        const GeRunResult res = ge_execute(mem, list, 0u, state, draws, stats, nullptr);
        check(res.status == GeRunStatus::Done, "simple list completes");
        check(stats.commands == 7u, "command count");
        check(stats.finishes == 1u && stats.draws == 1u, "finish + one draw");
        check(draws.size() == 1u && draws[0].prim_type == kPrimSprites &&
                  draws[0].vertex_count == 4u,
              "sprite draw decoded");
        check(draws[0].vertex_address == 0x08012000u, "vaddr = base + offset + arg");
        check(draws[0].vertex_type == 0x12u, "vertex type snapshot");
        check(draws[0].regs[kCmdPrim] == ((6u << 16) | 4u), "register snapshot holds PRIM arg");
    }

    // --- Control flow: JUMP skips, CALL/RET nests ---
    {
        psprecomp::Runtime runtime(32u * 1024u * 1024u);
        auto &mem = runtime.memory();
        const std::uint32_t list = 0x08810000u;
        const std::uint32_t sub = list + 0x40u;
        mem.store32(list + 0, cmd(kCmdBase, 0x00080000u));
        mem.store32(list + 4, cmd(kCmdJump, (list + 12u) & 0x00FFFFFCu));  // skip next
        mem.store32(list + 8, cmd(kCmdPrim, (3u << 16) | 3u));             // skipped
        mem.store32(list + 12, cmd(kCmdCall, sub & 0x00FFFFFCu));
        mem.store32(list + 16, cmd(kCmdPrim, (6u << 16) | 2u));            // after ret
        mem.store32(list + 20, cmd(kCmdEnd, 0u));
        mem.store32(sub + 0, cmd(kCmdPrim, (0u << 16) | 5u));              // in sublist
        mem.store32(sub + 4, cmd(kCmdRet, 0u));

        GeState state;
        std::vector<GeDrawCall> draws;
        GeExecStats stats;
        const GeRunResult res = ge_execute(mem, list, 0u, state, draws, stats, nullptr);
        check(res.status == GeRunStatus::Done, "control-flow list completes");
        check(stats.calls == 1u, "one call");
        check(draws.size() == 2u && draws[0].vertex_count == 5u && draws[1].vertex_count == 2u,
              "jump skipped a draw, call/ret ordered the rest");
    }

    // --- Stall and resume ---
    {
        psprecomp::Runtime runtime(32u * 1024u * 1024u);
        auto &mem = runtime.memory();
        const std::uint32_t list = 0x08810000u;
        mem.store32(list + 0, cmd(kCmdPrim, (6u << 16) | 1u));
        mem.store32(list + 4, cmd(kCmdPrim, (6u << 16) | 2u));  // <- stall here
        mem.store32(list + 8, cmd(kCmdPrim, (6u << 16) | 3u));
        mem.store32(list + 12, cmd(kCmdEnd, 0u));

        GeState state;
        std::vector<GeDrawCall> draws;
        GeExecStats stats;
        const GeRunResult res = ge_execute(mem, list, list + 4u, state, draws, stats, nullptr);
        check(res.status == GeRunStatus::Stalled && res.pc == list + 4u, "stalls at the stall address");
        check(draws.size() == 1u, "one draw before the stall");
        const GeRunResult res2 = ge_execute(mem, res.pc, 0u, state, draws, stats, nullptr);
        check(res2.status == GeRunStatus::Done && draws.size() == 3u, "resume drains the list");
    }

    // --- SIGNAL delivery + transfer recording ---
    {
        psprecomp::Runtime runtime(32u * 1024u * 1024u);
        auto &mem = runtime.memory();
        const std::uint32_t list = 0x08810000u;
        mem.store32(list + 0, cmd(kCmdSignal, (0x05u << 16) | 0x1234u));
        mem.store32(list + 4, cmd(kCmdTransferStart, 0u));
        mem.store32(list + 8, cmd(kCmdEnd, 0u));

        std::uint32_t got_behavior = 0, got_data = 0;
        GeState state;
        std::vector<GeDrawCall> draws;
        GeExecStats stats;
        const GeRunResult res = ge_execute(
            mem, list, 0u, state, draws, stats,
            [&](std::uint32_t behavior, std::uint32_t data) {
                got_behavior = behavior;
                got_data = data;
            });
        check(res.status == GeRunStatus::Done, "signal list completes");
        check(stats.signals == 1u && got_behavior == 0x05u && got_data == 0x1234u,
              "signal behavior + data delivered");
        check(stats.transfers == 1u && draws.size() == 1u && draws[0].is_transfer,
              "transfer recorded as a draw");
    }

    // --- ORIGIN anchoring, bare RET, runaway cap ---
    {
        psprecomp::Runtime runtime(32u * 1024u * 1024u);
        auto &mem = runtime.memory();
        const std::uint32_t list = 0x08810000u;
        mem.store32(list + 0, cmd(kCmdOrigin, 0u));
        mem.store32(list + 4, cmd(kCmdVaddr, 0x40u));
        mem.store32(list + 8, cmd(kCmdPrim, (6u << 16) | 1u));
        mem.store32(list + 12, cmd(kCmdRet, 0u));  // bare RET ends the list

        GeState state;
        std::vector<GeDrawCall> draws;
        GeExecStats stats;
        const GeRunResult res = ge_execute(mem, list, 0u, state, draws, stats, nullptr);
        check(res.status == GeRunStatus::Done, "bare RET completes the list");
        check(draws.size() == 1u && draws[0].vertex_address == list + 0x40u,
              "ORIGIN anchors relative addresses at the list");

        const std::uint32_t loop_list = 0x08820000u;
        mem.store32(loop_list + 0, cmd(kCmdBase, 0x00080000u));
        mem.store32(loop_list + 4, cmd(kCmdJump, (loop_list + 4u) & 0x00FFFFFCu));  // to self
        GeState state2;
        std::vector<GeDrawCall> draws2;
        GeExecStats stats2;
        const GeRunResult res2 = ge_execute(mem, loop_list, 0u, state2, draws2, stats2, nullptr);
        check(res2.status == GeRunStatus::Error && stats2.commands == kMaxCommandsPerRun,
              "runaway list hits the command cap");
    }

    std::fprintf(stderr, "test_p3p_ge: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
