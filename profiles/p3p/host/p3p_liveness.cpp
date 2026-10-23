
#include "p3p_liveness.hpp"

#include "hle/p3p_hle_gedisplay.hpp"
#include "hle/p3p_hle_mpeg.hpp"
#include "kernel/p3p_kernel.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdio>
#include <set>
#include <utility>

namespace p3p {

namespace {

// One evaluation window: ~2M outer dispatches between heartbeats of
// 250k. Long enough that healthy phases (loads, scene changes) show
// new syscalls or a wide PC spread; short enough that a stall is
// reported while there is still run left to observe.
inline constexpr std::uint64_t kHeartbeatInterval = 250'000u;
inline constexpr std::uint64_t kWindowDispatches = 2'000'000u;
// A window whose sampled outer-dispatch PCs fit in this set while
// millions of dispatches elapsed is a loop, not a workload.
inline constexpr std::size_t kNarrowPcThreshold = 256;

P3PKernel *g_kernel = nullptr;
LivenessWindow g_window;
std::set<std::uint64_t> g_seen_imports;
std::unordered_map<std::uint64_t, std::string> g_import_names;
LivenessProgress g_window_start_progress;
bool g_window_open = false;

std::uint64_t import_key(const std::string &library, std::uint32_t nid) {
    // FNV-1a over the library name, folded with the NID.
    std::uint64_t h = 1469598103934665603ull;
    for (const char c : library) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 1099511628211ull;
    }
    return h ^ (static_cast<std::uint64_t>(nid) + 0x9E3779B97F4A7C15ull);
}

LivenessProgress snapshot_progress() {
    LivenessProgress p;
    const MpegProgress mpeg = hle_mpeg_progress();
    p.mpeg_packets_put = mpeg.packets_put;
    p.mpeg_au_video = mpeg.au_video;
    p.mpeg_au_audio = mpeg.au_audio;
    p.ge_pixels = hle_gedisplay_pixels_total();
    p.kernel_now_us = g_kernel != nullptr ? g_kernel->now_us() : 0u;
    return p;
}

void evaluate_window(std::uint64_t dispatch) {
    const LivenessProgress now = snapshot_progress();
    const std::uint64_t d_put = now.mpeg_packets_put - g_window_start_progress.mpeg_packets_put;
    const std::uint64_t d_auv = now.mpeg_au_video - g_window_start_progress.mpeg_au_video;
    const std::uint64_t d_aua = now.mpeg_au_audio - g_window_start_progress.mpeg_au_audio;
    const std::uint32_t d_px = now.ge_pixels - g_window_start_progress.ge_pixels;
    const std::uint64_t d_clk = now.kernel_now_us - g_window_start_progress.kernel_now_us;

    // A trickle of first-ever syscalls (lazy one-off inits) must not
    // mask a stall: at ~800k imports per window, a couple of new
    // arrivals still means >99.7% repeats. The line reports the
    // count so the reader can judge.
    if (g_window.new_syscalls <= 2u && !g_window.pc_wide &&
        g_window.pcs.size() <= kNarrowPcThreshold) {
        if (g_window.imports_total == 0u) {
            std::fprintf(stderr,
                         "[p3p-liveness] @%llu: no syscalls in %llu dispatches, "
                         "%zu distinct pcs -- guest compute loop, no kernel contact "
                         "(progress: mpeg_put +%llu au +%llu/%llu ge +%u px clock +%llu us)\n",
                         static_cast<unsigned long long>(dispatch),
                         static_cast<unsigned long long>(dispatch - g_window.start_dispatch),
                         g_window.pcs.size(),
                         static_cast<unsigned long long>(d_put),
                         static_cast<unsigned long long>(d_auv),
                         static_cast<unsigned long long>(d_aua), d_px,
                         static_cast<unsigned long long>(d_clk));
        } else {
            // The dominant repeated import is the poll signature.
            std::uint64_t top_key = 0;
            std::uint32_t top_count = 0;
            for (const auto &[key, count] : g_window.import_counts) {
                if (count > top_count) {
                    top_count = count;
                    top_key = key;
                }
            }
            const std::uint32_t share =
                static_cast<std::uint32_t>(top_count * 100u / g_window.imports_total);
            std::fprintf(stderr,
                         "[p3p-liveness] @%llu: %u new syscalls in %llu dispatches, "
                         "%zu distinct pcs, top import %s x%u (%u%% of %llu)%s "
                         "(progress: mpeg_put +%llu au +%llu/%llu ge +%u px clock +%llu us)\n",
                         static_cast<unsigned long long>(dispatch), g_window.new_syscalls,
                         static_cast<unsigned long long>(dispatch - g_window.start_dispatch),
                         g_window.pcs.size(), liveness_import_name(top_key).c_str(), top_count,
                         share, static_cast<unsigned long long>(g_window.imports_total),
                         (g_window.new_syscalls == 0u && d_put == 0u && d_auv == 0u &&
                          d_aua == 0u && d_px == 0u && d_clk > 1'000'000u)
                             ? " -- POLLING SUSPECT (time advances, nothing moves)"
                             : "",
                         static_cast<unsigned long long>(d_put),
                         static_cast<unsigned long long>(d_auv),
                         static_cast<unsigned long long>(d_aua), d_px,
                         static_cast<unsigned long long>(d_clk));
            if (g_window.new_syscalls == 0u && d_put == 0u && d_auv == 0u &&
                d_aua == 0u && d_px == 0u && d_clk > 1'000'000u) {
                std::fprintf(stderr, "[p3p-liveness]   stalled pc set:");
                for (std::uint32_t pc : g_window.pcs)
                    std::fprintf(stderr, " %08X", pc);
                std::fprintf(stderr, "\n");
            }
        }
    }
    g_window.reset(dispatch);
    g_window_start_progress = now;
}

void heartbeat(std::uint64_t dispatch, std::uint32_t pc) {
    if (!g_window_open) {
        g_window.reset(dispatch);
        g_window_start_progress = snapshot_progress();
        g_window_open = true;
        return;
    }
    g_window.note_pc(pc);
    if (dispatch - g_window.start_dispatch >= kWindowDispatches)
        evaluate_window(dispatch);
}

}  // namespace

void liveness_install(P3PKernel *kernel) {
    g_kernel = kernel;
    psprecomp::set_runtime_heartbeat_hook(&heartbeat, kHeartbeatInterval);
}

void liveness_note_import(const std::string &library, std::uint32_t nid) {
    const std::uint64_t key = import_key(library, nid);
    const bool first_ever = g_seen_imports.insert(key).second;
    if (first_ever) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s::0x%08X", library.c_str(), nid);
        g_import_names.emplace(key, buf);
    }
    if (g_window_open)
        g_window.note_import(key, first_ever);
}

std::string liveness_import_name(std::uint64_t key) {
    const auto it = g_import_names.find(key);
    return it == g_import_names.end() ? std::string{"<unknown>"} : it->second;
}

}  // namespace p3p
