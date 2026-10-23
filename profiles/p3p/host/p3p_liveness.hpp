#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace p3p {

class P3PKernel;

// Liveness / stall detection (Gate 6 follow-up). "The guest produced
// no error" is not evidence the guest is making progress: a player
// polling for data that never arrives is indistinguishable from a
// healthy loop unless the host measures it. This unit samples the
// outer-dispatch PC from the runtime heartbeat hook and correlates
// it over fixed dispatch windows with what the guest actually did:
//
//   - did any NEW (library, NID) syscall appear in the window?
//   - how wide is the set of dispatch PCs (a tight poll loop is
//     narrow; real work roams)?
//   - did anything move data (ringbuffer packets in, AUs out, GE
//     pixels, kernel fake clock)?
//
// A window with no new syscall and a narrow PC set prints one
// [p3p-liveness] evidence line; when a single repeated import
// dominates the window the line calls it what it is -- polling.
// Windows that show new syscalls or a wide PC spread stay silent.

// Testable window accumulator. Production feeds it from the
// heartbeat hook + dispatch_import; tests feed it synthetically.
struct LivenessWindow {
    static constexpr std::size_t kPcSetCap = 4096;

    void reset(std::uint64_t dispatch) {
        start_dispatch = dispatch;
        pcs.clear();
        pc_wide = false;
        import_counts.clear();
        imports_total = 0;
        new_syscalls = 0;
    }
    void note_pc(std::uint32_t pc) {
        if (pcs.size() < kPcSetCap)
            pcs.insert(pc);
        else
            pc_wide = true;
    }
    // Returns true when this (library, nid) pair is seen for the
    // first time EVER (the caller keeps the all-time set).
    void note_import(std::uint64_t key, bool first_ever) {
        ++import_counts[key];
        ++imports_total;
        if (first_ever)
            ++new_syscalls;
    }

    std::uint64_t start_dispatch = 0;
    std::unordered_set<std::uint32_t> pcs;
    bool pc_wide = false;
    std::unordered_map<std::uint64_t, std::uint32_t> import_counts;
    std::uint64_t imports_total = 0;
    std::uint32_t new_syscalls = 0;
};

// Subsystem progress snapshot, diffed across a window.
struct LivenessProgress {
    std::uint64_t mpeg_packets_put = 0;
    std::uint64_t mpeg_au_video = 0;
    std::uint64_t mpeg_au_audio = 0;
    std::uint32_t ge_pixels = 0;
    std::uint64_t kernel_now_us = 0;
};

// Installs the heartbeat hook and binds the kernel used for clock
// snapshots. Called once from the profile's init.
void liveness_install(P3PKernel *kernel);
// Feed point for every dispatched import (profile dispatch_import).
void liveness_note_import(const std::string &library, std::uint32_t nid);
// Human-readable name for a syscall key, for the evidence lines.
std::string liveness_import_name(std::uint64_t key);

}  // namespace p3p
