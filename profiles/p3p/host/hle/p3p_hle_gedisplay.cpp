#include "hle/p3p_hle_gedisplay.hpp"

#include "gpu/p3p_frame_dump.hpp"
#include "gpu/p3p_ge_executor.hpp"
#include "gpu/p3p_rasterizer.hpp"
#include "kernel/p3p_kernel.hpp"

#include <map>
#include <vector>

#include "psprecomp/guest_memory.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdio>

namespace p3p {
namespace {

// sceGe_user NIDs.
constexpr std::uint32_t kNidGeListEnQueue = 0xAB49E76Au;
constexpr std::uint32_t kNidGeListEnQueueHead = 0x1C0D95A6u;
constexpr std::uint32_t kNidGeListSync = 0x03444EB4u;
constexpr std::uint32_t kNidGeDrawSync = 0xB287BD61u;
constexpr std::uint32_t kNidGeBreak = 0xB448EC0Du;
constexpr std::uint32_t kNidGeContinue = 0x4C06E472u;
constexpr std::uint32_t kNidGeSetCallback = 0xA4FC06A4u;
constexpr std::uint32_t kNidGeUnsetCallback = 0x05DB22CEu;
constexpr std::uint32_t kNidGeEdramGetAddr = 0xE47E40E4u;
constexpr std::uint32_t kNidGeEdramGetSize = 0x1F6752ADu;
constexpr std::uint32_t kNidGeGetCmd = 0xDC93CFEFu;
constexpr std::uint32_t kNidGeListUpdateStallAddr = 0xE0D68148u;

// sceDisplay NIDs.
constexpr std::uint32_t kNidDisplaySetMode = 0x0E20F177u;
constexpr std::uint32_t kNidDisplaySetFrameBuf = 0x289D82FEu;
constexpr std::uint32_t kNidDisplayGetFrameBuf = 0xEEDA2E54u;
constexpr std::uint32_t kNidDisplayWaitVblankStart = 0x984C27E7u;
constexpr std::uint32_t kNidDisplayWaitVblankStartCB = 0x46F186C3u;
constexpr std::uint32_t kNidDisplayWaitVblankStartMultiCB = 0x77ED8B3Au;
constexpr std::uint32_t kNidDisplayGetVcount = 0x9C6EAAD7u;
constexpr std::uint32_t kNidDisplayGetFramePerSec = 0xDBA6C4C4u;

struct GeListRecord {
    std::uint32_t stall = 0;
    std::uint32_t pc = 0;
    ge::GeRunStatus status = ge::GeRunStatus::Done;
    std::vector<ge::GeDrawCall> draws;
    ge::GeExecStats stats;
    std::size_t rasterized = 0;  // draws already fed to the rasterizer
};

struct GeDisplayState {
    std::uint32_t next_list_id = 0;
    std::uint32_t next_callback_id = 0;
    // Gate 6 WP1: real display-list execution.
    ge::GeState ge_machine;
    std::map<std::uint32_t, GeListRecord> ge_lists;
    std::uint32_t ge_signal_cb = 0;
    std::uint32_t ge_finish_cb = 0;
    std::uint32_t ge_log_budget = 12;
    ge::RasterStats raster_stats;
    // --dump-frame: when set, the display framebuffer is written to
    // this path every kDumpEveryFrames vblanks (the guest's frame loop
    // is the only reliable mid-run cadence in a headless boot).
    std::string dump_path;
    std::uint32_t dump_counter = 0;
    std::uint32_t vcount = 0;
    std::uint32_t mode = 0;
    std::uint32_t width = 480;
    std::uint32_t height = 272;
    std::uint32_t fb_topaddr = 0x04000000u;
    std::uint32_t fb_bufferwidth = 512;
    std::uint32_t fb_pixelformat = 3;  // 8888
};

GeDisplayState g_state;

}  // namespace

void gedisplay_reset_state() { g_state = GeDisplayState{}; }

void gedisplay_set_frame_dump_path(const std::string &path) { g_state.dump_path = path; }

bool gedisplay_dump_frame(psprecomp::GuestMemory &memory, const std::string &path) {
    {
        std::uint32_t nz_a = 0, nz_b = 0;
        for (std::uint32_t a = 0x04000000u; a < 0x04088000u; a += 4u)
            if (memory.load32(a) != 0u) ++nz_a;
        for (std::uint32_t a = 0x04088000u; a < 0x04110000u; a += 4u)
            if (memory.load32(a) != 0u) ++nz_b;
        std::fprintf(stderr,
                     "[p3p-ge] dump: display fb %08X, VRAM nonzero words A %u B %u, raster "
                     "sprites %u tris %u px %u\n",
                     g_state.fb_topaddr, nz_a, nz_b, g_state.raster_stats.sprites,
                     g_state.raster_stats.triangles, g_state.raster_stats.pixels_written);
    }
    const std::uint32_t height = g_state.height != 0 ? g_state.height : 272u;
    const std::uint32_t width =
        std::min(g_state.fb_bufferwidth, g_state.width != 0 ? g_state.width : 480u);
    // In movie mode the player takes the display over and the recorded
    // framebuffer pointer reads 0; fall back to VRAM bank A so the
    // visual check keeps working instead of failing the dump.
    std::uint32_t fb = g_state.fb_topaddr;
    if (fb < 0x04000000u || fb >= 0x04200000u) {
        std::fprintf(stderr, "[p3p-ge] dump: fb %08X unusable, falling back to VRAM bank A\n", fb);
        fb = 0x04000000u;
    }
    return write_framebuffer_png(memory, fb, g_state.fb_bufferwidth, width, height,
                                 g_state.fb_pixelformat, path);
}

namespace {

const char *ge_status_name(ge::GeRunStatus status) {
    switch (status) {
        case ge::GeRunStatus::Done: return "done";
        case ge::GeRunStatus::Stalled: return "stalled";
        case ge::GeRunStatus::Error: return "error";
    }
    return "?";
}

void ge_accumulate(ge::GeExecStats &into, const ge::GeExecStats &add) {
    into.commands += add.commands;
    into.draws += add.draws;
    into.transfers += add.transfers;
    into.unsupported_draws += add.unsupported_draws;
    into.signals += add.signals;
    into.calls += add.calls;
    into.unknown_opcodes += add.unknown_opcodes;
    into.finishes += add.finishes;
}

// Runs (or resumes) one list on the shared GE machine and folds the
// outcome into its record. Signals go to the registered GE callback.
void ge_run_list(psprecomp::GuestMemory &memory, P3PKernel *kernel, std::uint32_t id,
                 GeListRecord &rec, std::uint32_t pc, const char *how) {
    ge::GeExecStats stats;
    const ge::GeRunResult res = ge::ge_execute(
        memory, pc, rec.stall, g_state.ge_machine, rec.draws, stats,
        [kernel](std::uint32_t behavior, std::uint32_t data) {
            (void)behavior;
            if (kernel != nullptr && g_state.ge_signal_cb != 0u)
                kernel->notify_callback(static_cast<std::int32_t>(g_state.ge_signal_cb),
                                        static_cast<std::int32_t>(data));
        });
    ge_accumulate(rec.stats, stats);
    rec.status = res.status;
    rec.pc = res.pc;
    // Gate 6 WP2: rasterize whatever this run produced. Draw records
    // carry guest-controlled addresses; a wild one must not kill the
    // host -- log it and skip the rest of this list instead.
    const std::uint32_t pixels_before = g_state.raster_stats.pixels_written;
    try {
        ge::rasterize_draws(memory, rec.draws, rec.rasterized, g_state.fb_pixelformat,
                            g_state.height, g_state.raster_stats);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[p3p-ge] rasterize aborted (list %u): %s\n", id, e.what());
    }
    rec.rasterized = rec.draws.size();
    const std::uint32_t pixels_new = g_state.raster_stats.pixels_written - pixels_before;
    if (pixels_new > 0u && g_state.ge_log_budget > 0u)
        std::fprintf(stderr,
                     "[p3p-ge] rasterized +%u px (totals: %u sprites, %u tris, %u px)\n",
                     pixels_new, g_state.raster_stats.sprites, g_state.raster_stats.triangles,
                     g_state.raster_stats.pixels_written);
    if (res.status == ge::GeRunStatus::Done && stats.finishes > 0u && kernel != nullptr &&
        g_state.ge_finish_cb != 0u)
        kernel->notify_callback(static_cast<std::int32_t>(g_state.ge_finish_cb),
                                static_cast<std::int32_t>(id));
    if (g_state.ge_log_budget > 0u) {
        --g_state.ge_log_budget;
        std::fprintf(stderr,
                     "[p3p-ge] list %u %s @%08X: +%u cmds +%u draws +%u xfers -> %s\n", id,
                     how, pc, stats.commands, stats.draws, stats.transfers,
                     ge_status_name(res.status));
    }
}

}  // namespace

// A vblank wait is a frame-pacing block in the reference kernel: the
// caller sleeps until the next frame boundary, and that sleep is what lets
// worker threads progress (the CRI ADX audio thread's main loop is
// WaitVblankStart + decode pump; the CRI file thread only gets the CPU
// while it waits). Gate 5C: with a kernel, complete the call (result
// preset, resume at $ra) and sleep to the next frame boundary on the
// kernel's fake clock — sleeping, not just yielding, so the waiter is
// not Ready again until its frame arrives. Without a kernel, the
// caller's counter advance is the whole effect.
void wait_vblank(psprecomp::AllegrexContext &ctx, P3PKernel *kernel) {
    ctx.set_gpr(2, 0u);
    if (kernel != nullptr) {
        ctx.pc = ctx.gpr[31];
        constexpr std::uint64_t kFrameUs = 16667u;  // 59.94 Hz frame
        const std::uint64_t now = kernel->now_us();
        kernel->sleep_current(ctx, kFrameUs - (now % kFrameUs));
    }
}

std::uint32_t hle_gedisplay_pixels_total() { return g_state.raster_stats.pixels_written; }

bool hle_gedisplay_dispatch(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                            P3PKernel *kernel, const std::string &library, std::uint32_t nid) {
    psprecomp::GuestMemory &memory = runtime.memory();
    const auto set_v0 = [&ctx](std::uint32_t value) { ctx.set_gpr(2, value); };

    if (library == kGeLibrary) {
        switch (nid) {
            case kNidGeEdramGetAddr:
                set_v0(0x04000000u);  // EDRAM base (GuestMemory::kVramPhysicalBase)
                return true;
            case kNidGeEdramGetSize:
                set_v0(0x00200000u);  // 2 MiB EDRAM
                return true;
            case kNidGeSetCallback: {
                const std::uint32_t id = g_state.next_callback_id++;
                g_state.ge_finish_cb = ctx.gpr[4];
                g_state.ge_signal_cb = ctx.gpr[5];
                set_v0(id);
                return true;
            }
            case kNidGeUnsetCallback:
                g_state.ge_finish_cb = 0;
                g_state.ge_signal_cb = 0;
                set_v0(0);
                return true;
            case kNidGeListEnQueue:
            case kNidGeListEnQueueHead: {
                const std::uint32_t id = g_state.next_list_id++;
                // Bound retained history: drop completed records.
                if (g_state.ge_lists.size() >= 16u) {
                    for (auto it = g_state.ge_lists.begin(); it != g_state.ge_lists.end();) {
                        if (it->second.status == ge::GeRunStatus::Done)
                            it = g_state.ge_lists.erase(it);
                        else
                            ++it;
                    }
                }
                GeListRecord rec;
                rec.stall = ctx.gpr[5];
                GeListRecord &slot = g_state.ge_lists[id] = std::move(rec);
                ge_run_list(memory, kernel, id, slot, ctx.gpr[4],
                            nid == kNidGeListEnQueue ? "enqueue" : "enqueue-head");
                set_v0(id);
                return true;
            }
            case kNidGeListSync: {
                const auto it = g_state.ge_lists.find(ctx.gpr[4]);
                if (it == g_state.ge_lists.end() || it->second.status == ge::GeRunStatus::Done)
                    set_v0(0);
                else if (it->second.status == ge::GeRunStatus::Stalled)
                    set_v0(3u);  // PSP_GE_DL_STATE_STALL
                else
                    set_v0(2u);  // PSP_GE_DL_STATE_DRAWING (error/other)
                return true;
            }
            case kNidGeDrawSync: {
                bool busy = false;
                for (const auto &entry : g_state.ge_lists)
                    if (entry.second.status == ge::GeRunStatus::Stalled)
                        busy = true;
                set_v0(busy ? 1u : 0u);
                return true;
            }
            case kNidGeListUpdateStallAddr: {
                const auto it = g_state.ge_lists.find(ctx.gpr[4]);
                if (it != g_state.ge_lists.end()) {
                    it->second.stall = ctx.gpr[5];
                    if (it->second.status == ge::GeRunStatus::Stalled)
                        ge_run_list(memory, kernel, ctx.gpr[4], it->second, it->second.pc,
                                    "stall-update");
                }
                set_v0(0);
                return true;
            }
            case kNidGeBreak:
            case kNidGeContinue:
            case kNidGeGetCmd:
                set_v0(0);
                return true;
            default:
                return false;
        }
    }

    if (library == kDisplayLibrary) {
        switch (nid) {
            case kNidDisplaySetMode:
                g_state.mode = ctx.gpr[4];
                g_state.width = ctx.gpr[5];
                g_state.height = ctx.gpr[6];
                set_v0(0);
                return true;
            case kNidDisplaySetFrameBuf:
                g_state.fb_topaddr = ctx.gpr[4];
                g_state.fb_bufferwidth = ctx.gpr[5];
                g_state.fb_pixelformat = ctx.gpr[6];
                set_v0(0);
                return true;
            case kNidDisplayGetFrameBuf:
                if (ctx.gpr[4] != 0u) memory.store32(ctx.gpr[4], g_state.fb_topaddr);
                if (ctx.gpr[5] != 0u) memory.store32(ctx.gpr[5], g_state.fb_bufferwidth);
                if (ctx.gpr[6] != 0u) memory.store32(ctx.gpr[6], g_state.fb_pixelformat);
                set_v0(g_state.fb_topaddr);
                return true;
            case kNidDisplayWaitVblankStart:
            case kNidDisplayWaitVblankStartCB:
                g_state.vcount += 1u;
                wait_vblank(ctx, kernel);
                if (!g_state.dump_path.empty() &&
                    ++g_state.dump_counter % 240u == 0u)
                    gedisplay_dump_frame(memory, g_state.dump_path);
                return true;
            case kNidDisplayWaitVblankStartMultiCB:
                g_state.vcount += ctx.gpr[4] != 0u ? ctx.gpr[4] : 1u;
                wait_vblank(ctx, kernel);
                return true;
            case kNidDisplayGetVcount:
                set_v0(g_state.vcount);
                return true;
            case kNidDisplayGetFramePerSec:
                ctx.fpr[0] = 59.94f;
                set_v0(ctx.fpr_bits(0));
                return true;
            default:
                return false;
        }
    }

    return false;
}

}  // namespace p3p
