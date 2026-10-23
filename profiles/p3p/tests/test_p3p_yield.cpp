// Unit tests for the P3P Gate 5C cooperative scheduler: yield on
// DelayThread, blocking WaitEventFlag with wake + re-dispatch, and the
// poll (zero-timeout) form. Guest code is never executed — the tests drive
// the HLE dispatch with fabricated live contexts and inspect the context
// swaps the kernel performs.

#include "hle/p3p_hle_audio.hpp"
#include "hle/p3p_hle_gedisplay.hpp"
#include "hle/p3p_hle_threadman.hpp"
#include "kernel/p3p_kernel.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

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

constexpr std::uint32_t kStrAddr = 0x09000100u;
constexpr std::uint32_t kResultAddr = 0x09200000u;
constexpr std::uint32_t kFakeStub = 0x08B70000u;

void put_str(psprecomp::Runtime &runtime, std::uint32_t addr, const char *s) {
    for (std::uint32_t i = 0; i <= std::strlen(s); ++i)
        runtime.memory().store8(addr + i, static_cast<std::uint8_t>(s[i]));
}

}  // namespace

int main() {
    using namespace p3p;

    // --- DelayThread yields to a Ready thread and resumes after the call ---
    {
        psprecomp::Runtime runtime(32u * 1024u * 1024u);
        P3PKernel kernel;
        const std::int32_t loader = kernel.bootstrap_loader("loader", 0x08804108u, 0x20, 0x08C42A50u, 0x10000u);
        const std::int32_t worker = kernel.create_thread("worker", 0x08805000u, 0x30, 0x10000u, 0u, 0x08C42A50u, runtime.memory());
        check(worker > 0, "worker created");
        check(kernel.start_thread(worker, 0u, 0u, runtime.memory()) == 0, "worker started");
        const P3PThread *worker_rec = kernel.find_thread(worker);
        const std::uint32_t worker_sp = worker_rec->context.gpr[29];
        const std::uint32_t worker_pc = worker_rec->context.pc;

        psprecomp::AllegrexContext live{};
        live.pc = kFakeStub;
        live.gpr[31] = 0x08809999u;  // $ra: continuation after the delay call
        live.gpr[4] = 5000u;         // delay 5000 us
        check(hle_threadman_extra_dispatch(runtime, live, kernel, kNidDelayThread),
              "delay dispatch handled");
        check(kernel.current_uid() == worker, "delay switched to the worker");
        check(live.pc == worker_pc && live.gpr[29] == worker_sp,
              "live context is the worker's saved context");
        const P3PThread *loader_rec = kernel.find_thread(loader);
        check(loader_rec->status == ThreadStatus::Waiting, "loader sleeps after DelayThread");
        check(loader_rec->context.pc == 0x08809999u,
              "loader resumes after the delay call ($ra)");
        check(loader_rec->context.gpr[2] == 0u, "loader's delay return value is 0");

        // Worker exits: hand-off resumes the loader at its continuation.
        check(kernel.exit_current_and_schedule_next(live), "exit hand-off happened");
        check(kernel.current_uid() == loader, "loader running again");
        check(live.pc == 0x08809999u, "loader context loaded at continuation");
    }

    // --- DelayThread with nobody else Ready is a plain return ---
    {
        psprecomp::Runtime runtime(32u * 1024u * 1024u);
        P3PKernel kernel;
        const std::int32_t loader = kernel.bootstrap_loader("solo", 0x08804108u, 0x20, 0u, 0x10000u);
        psprecomp::AllegrexContext live{};
        live.pc = kFakeStub;
        live.gpr[31] = 0x08801111u;
        live.gpr[4] = 100u;
        check(hle_threadman_extra_dispatch(runtime, live, kernel, kNidDelayThread),
              "solo delay handled");
        check(kernel.current_uid() == loader, "solo delay keeps the CPU");
        check(kernel.find_thread(loader)->status == ThreadStatus::Running,
              "solo thread still Running");
        check(live.pc == 0x08801111u && live.gpr[2] == 0u,
              "solo delay returns normally at $ra");
    }

    // --- WaitEventFlag blocks, wakes on SetEventFlag, completes on re-dispatch ---
    {
        psprecomp::Runtime runtime(32u * 1024u * 1024u);
        P3PKernel kernel;
        const std::int32_t loader = kernel.bootstrap_loader("loader", 0x08804108u, 0x20, 0x08C42A50u, 0x10000u);

        put_str(runtime, kStrAddr, "flag");
        psprecomp::AllegrexContext mk{};
        mk.gpr[4] = kStrAddr;
        mk.gpr[5] = 0u;  // attr
        mk.gpr[6] = 0u;  // init pattern 0
        check(hle_threadman_extra_dispatch(runtime, mk, kernel, kNidCreateEventFlag),
              "create event flag handled");
        const std::int32_t evf = static_cast<std::int32_t>(mk.gpr[2]);
        check(evf > 0, "event flag uid valid");

        const std::int32_t worker = kernel.create_thread("worker", 0x08806000u, 0x30, 0x10000u, 0u, 0x08C42A50u, runtime.memory());
        check(kernel.start_thread(worker, 0u, 0u, runtime.memory()) == 0, "worker started");

        runtime.memory().store32(kResultAddr, 0xDEADBEEFu);  // sentinel
        psprecomp::AllegrexContext live{};
        live.pc = kFakeStub + 4u;
        live.gpr[31] = 0x08808888u;
        live.gpr[4] = static_cast<std::uint32_t>(evf);
        live.gpr[5] = 0x1u;          // wait for bit 0
        live.gpr[6] = 0u;            // WAITAND, no clear
        live.gpr[7] = kResultAddr;   // result pattern out
        live.gpr[8] = 0u;            // no timeout pointer: blocking wait
        check(hle_threadman_extra_dispatch(runtime, live, kernel, kNidWaitEventFlag),
              "wait dispatch handled");
        check(kernel.current_uid() == worker, "blocking wait switched to the worker");
        const P3PThread *loader_rec = kernel.find_thread(loader);
        check(loader_rec->status == ThreadStatus::Waiting, "loader is Waiting");
        check(loader_rec->wait_kind == P3PThread::WaitKind::EventFlag,
              "wait recorded as EventFlag");
        check(loader_rec->context.pc == kFakeStub + 4u,
              "blocked context still points at the wait call (re-dispatch)");
        check(runtime.memory().load32(kResultAddr) == 0xDEADBEEFu,
              "result pattern untouched while blocked");

        // Worker sets the flag: the waiter becomes Ready.
        psprecomp::AllegrexContext set_ctx{};
        set_ctx.gpr[4] = static_cast<std::uint32_t>(evf);
        set_ctx.gpr[5] = 0x1u;
        check(hle_threadman_extra_dispatch(runtime, set_ctx, kernel, kNidSetEventFlag) &&
                  set_ctx.gpr[2] == 0u,
              "set event flag ok");
        check(kernel.find_thread(loader)->status == ThreadStatus::Ready,
              "waiter woken by set");

        // Worker exits; hand-off resumes the loader at the wait call, whose
        // re-dispatch now completes it.
        check(kernel.exit_current_and_schedule_next(live), "hand-off back to loader");
        check(kernel.current_uid() == loader, "loader running again");
        check(live.pc == kFakeStub + 4u, "resumed at the wait call");
        check(hle_threadman_extra_dispatch(runtime, live, kernel, kNidWaitEventFlag),
              "re-dispatched wait handled");
        check(live.gpr[2] == 0u, "re-dispatched wait returns 0");
        check(runtime.memory().load32(kResultAddr) == 0x1u,
              "result pattern written on completion");
        check(kernel.find_thread(loader)->wait_kind == P3PThread::WaitKind::None,
              "wait record cleared after completion");

        // Poll form: zero timeout on an unsatisfied flag returns EVF_COND
        // without blocking.
        put_str(runtime, kStrAddr, "flag2");
        psprecomp::AllegrexContext mk2{};
        mk2.gpr[4] = kStrAddr;
        mk2.gpr[5] = 0u;
        mk2.gpr[6] = 0u;
        hle_threadman_extra_dispatch(runtime, mk2, kernel, kNidCreateEventFlag);
        const std::int32_t evf2 = static_cast<std::int32_t>(mk2.gpr[2]);
        runtime.memory().store32(kResultAddr, 0u);  // zero timeout lives here
        psprecomp::AllegrexContext poll{};
        poll.pc = kFakeStub + 8u;
        poll.gpr[4] = static_cast<std::uint32_t>(evf2);
        poll.gpr[5] = 0x1u;
        poll.gpr[6] = 0u;
        poll.gpr[7] = 0u;
        poll.gpr[8] = kResultAddr;  // timeout pointer -> *timeout == 0 -> poll
        check(hle_threadman_extra_dispatch(runtime, poll, kernel, kNidWaitEventFlag),
              "poll wait handled");
        check(poll.gpr[2] == kErrorEvfCond, "poll wait returns EVF_COND");
        check(kernel.find_thread(loader)->status == ThreadStatus::Running,
              "poll wait does not block");
    }

    // --- Blocking audio output is a scheduling point (Gate 5C) ---
    {
        psprecomp::Runtime runtime(32u * 1024u * 1024u);
        P3PKernel kernel;
        const std::int32_t loader = kernel.bootstrap_loader("loader", 0x08804108u, 0x20, 0u, 0x10000u);
        const std::int32_t worker = kernel.create_thread("worker", 0x08807000u, 0x30, 0x10000u, 0u, 0u, runtime.memory());
        check(kernel.start_thread(worker, 0u, 0u, runtime.memory()) == 0, "audio-test worker started");

        // ChReserve: specific channel echoes back, -1 gets channel 0.
        psprecomp::AllegrexContext ch{};
        ch.gpr[4] = 3u;
        check(hle_audio_dispatch(runtime, ch, kernel, kNidAudioChReserve) && ch.gpr[2] == 3u,
              "ChReserve returns the requested channel");
        ch.gpr[4] = 0xFFFFFFFFu;
        hle_audio_dispatch(runtime, ch, kernel, kNidAudioChReserve);
        check(ch.gpr[2] == 0u, "ChReserve(-1) returns channel 0");
        check(hle_audio_dispatch(runtime, ch, kernel, kNidAudioOutput2GetRestSample) &&
                  ch.gpr[2] == 0u,
              "Output2 rest sample is 0 (instant drain)");

        psprecomp::AllegrexContext live{};
        live.pc = kFakeStub;
        live.gpr[31] = 0x08807777u;
        check(hle_audio_dispatch(runtime, live, kernel, kNidAudioOutputPannedBlocking),
              "panned blocking output handled");
        check(kernel.current_uid() == worker, "blocking output yielded to the worker");
        const P3PThread *loader_rec = kernel.find_thread(loader);
        check(loader_rec->status == ThreadStatus::Waiting &&
                  loader_rec->context.pc == 0x08807777u && loader_rec->context.gpr[2] == 0u,
              "output caller resumes after the call with result 0");
    }

    // --- Audio channels: distinct allocation + sample-rate drain ---
    {
        psprecomp::Runtime runtime(32u * 1024u * 1024u);
        P3PKernel kernel;
        const std::int32_t loader = kernel.bootstrap_loader("loader", 0x08804108u, 0x20, 0u, 0x10000u);
        const std::int32_t worker = kernel.create_thread("worker", 0x08807000u, 0x30, 0x10000u, 0u, 0u, runtime.memory());
        check(kernel.start_thread(worker, 0u, 0u, runtime.memory()) == 0, "audio-alloc worker started");

        // Channel state is process-global in the HLE; start from a
        // clean slate so earlier blocks' reservations don't leak in.
        {
            psprecomp::AllegrexContext rel{};
            for (std::uint32_t c = 0; c < 8u; ++c) {
                rel.gpr[4] = c;
                hle_audio_dispatch(runtime, rel, kernel, kNidAudioChRelease);
            }
        }

        // Each reserve(-1) hands out the next free channel; the game
        // relies on distinct channels per audio subsystem.
        psprecomp::AllegrexContext ch{};
        ch.gpr[4] = 0xFFFFFFFFu;  // -1: any free channel
        ch.gpr[5] = 4410u;        // sample count: 100 ms at 44.1 kHz
        hle_audio_dispatch(runtime, ch, kernel, kNidAudioChReserve);
        check(ch.gpr[2] == 0u, "first reserve(-1) gets channel 0");
        hle_audio_dispatch(runtime, ch, kernel, kNidAudioChReserve);
        check(ch.gpr[2] == 1u, "second reserve(-1) gets channel 1, not 0 again");
        // Releasing makes the channel the first free one again.
        ch.gpr[4] = 0u;
        check(hle_audio_dispatch(runtime, ch, kernel, kNidAudioChRelease) && ch.gpr[2] == 0u,
              "release channel 0");
        ch.gpr[4] = 0xFFFFFFFFu;
        hle_audio_dispatch(runtime, ch, kernel, kNidAudioChReserve);
        check(ch.gpr[2] == 0u, "reserve(-1) after release reuses channel 0");

        // A blocking output on the channel parks the caller for the
        // buffer's drain duration: 4410 samples at 44.1 kHz = 100 ms.
        psprecomp::AllegrexContext live{};
        live.pc = kFakeStub;
        live.gpr[31] = 0x08807777u;
        live.gpr[4] = 0u;  // channel 0
        check(hle_audio_dispatch(runtime, live, kernel, kNidAudioOutputBlocking),
              "blocking output handled");
        check(kernel.current_uid() == worker, "blocking output yielded to the worker");
        const P3PThread *loader_rec = kernel.find_thread(loader);
        check(loader_rec->status == ThreadStatus::Waiting && loader_rec->timed_wait &&
                  loader_rec->wake_at_us == 100000u,
              "output caller sleeps exactly the drain duration");

        // The worker observes the channel's rest length while it drains.
        psprecomp::AllegrexContext rest{};
        rest.gpr[4] = 0u;
        check(hle_audio_dispatch(runtime, rest, kernel, kNidAudioGetChannelRestLength) &&
                  rest.gpr[2] == 4410u,
              "rest length reports the queued samples");

        // Leave no reservations behind for later blocks.
        psprecomp::AllegrexContext rel{};
        rel.gpr[4] = 0u;
        hle_audio_dispatch(runtime, rel, kernel, kNidAudioChRelease);
        rel.gpr[4] = 1u;
        hle_audio_dispatch(runtime, rel, kernel, kNidAudioChRelease);
    }

    // --- WaitVblankStart is a frame-pacing scheduling point (Gate 5C) ---
    {
        psprecomp::Runtime runtime(32u * 1024u * 1024u);
        P3PKernel kernel;
        const std::int32_t loader = kernel.bootstrap_loader("loader", 0x08804108u, 0x20, 0u, 0x10000u);
        const std::int32_t worker = kernel.create_thread("worker", 0x08808000u, 0x30, 0x10000u, 0u, 0u, runtime.memory());
        check(kernel.start_thread(worker, 0u, 0u, runtime.memory()) == 0, "vblank-test worker started");
        gedisplay_reset_state();
        psprecomp::AllegrexContext live{};
        live.pc = kFakeStub;
        live.gpr[31] = 0x08806666u;
        check(hle_gedisplay_dispatch(runtime, live, &kernel, "sceDisplay", 0x984C27E7u),
              "wait vblank start handled");
        check(kernel.current_uid() == worker, "vblank wait yielded to the worker");
        const P3PThread *loader_rec = kernel.find_thread(loader);
        check(loader_rec->status == ThreadStatus::Waiting &&
                  loader_rec->context.pc == 0x08806666u && loader_rec->context.gpr[2] == 0u,
              "vblank waiter resumes after the call with result 0");
        psprecomp::AllegrexContext q{};
        check(hle_gedisplay_dispatch(runtime, q, &kernel, "sceDisplay", 0x9C6EAAD7u) &&
                  q.gpr[2] == 1u,
              "vcount advanced by the wait");
    }

    // --- Timed sleeps end priority starvation (the CRI thread layout) ---
    {
        psprecomp::Runtime runtime(32u * 1024u * 1024u);
        P3PKernel kernel;
        const std::int32_t a = kernel.bootstrap_loader("A", 0x08804108u, 22, 0u, 0x10000u);
        const std::int32_t w = kernel.create_thread("W", 0x08809000u, 16, 0x10000u, 0u, 0u, runtime.memory());
        const std::int32_t f = kernel.create_thread("F", 0x0880A000u, 24, 0x10000u, 0u, 0u, runtime.memory());
        check(kernel.start_thread(w, 0u, 0u, runtime.memory()) == 0, "starvation-test W started");
        check(kernel.start_thread(f, 0u, 0u, runtime.memory()) == 0, "starvation-test F started");

        // A (prio 22, the ADX-audio role) waits for vblank: it sleeps, so
        // W (16, wave-out role) runs next.
        psprecomp::AllegrexContext live{};
        live.pc = kFakeStub;
        live.gpr[31] = 0x08801000u;
        check(hle_gedisplay_dispatch(runtime, live, &kernel, "sceDisplay", 0x984C27E7u),
              "starvation-test vblank handled");
        check(kernel.current_uid() == w, "W runs while A vblank-sleeps");
        check(kernel.find_thread(a)->status == ThreadStatus::Waiting, "A is asleep, not Ready");

        // W blocks on audio output: it sleeps too, so F (24, file role)
        // gets the CPU even though A (22) outranks it — A is not Ready.
        live.gpr[31] = 0x08802000u;
        check(hle_audio_dispatch(runtime, live, kernel, kNidAudioOutputPannedBlocking),
              "starvation-test audio block handled");
        check(kernel.current_uid() == f, "F runs while A and W sleep (no starvation)");

        // F exits: the idle time skip wakes the frame sleepers, and the
        // higher-priority W resumes first.
        check(kernel.exit_current_and_schedule_next(live), "exit hand-off after time skip");
        check(kernel.current_uid() == w, "earliest frame wake resumes W first");
    }

    // --- Mutexes: ownership, blocking, hand-off (Gate 5C follow-up) ---
    {
        psprecomp::Runtime runtime(32u * 1024u * 1024u);
        P3PKernel kernel;
        const std::int32_t loader = kernel.bootstrap_loader("loader", 0x08804108u, 0x20, 0u, 0x10000u);
        const std::int32_t worker = kernel.create_thread("worker", 0x0880B000u, 0x30, 0x10000u, 0u, 0u, runtime.memory());
        check(kernel.start_thread(worker, 0u, 0u, runtime.memory()) == 0, "mutex-test worker started");

        put_str(runtime, kStrAddr, "mtx");
        psprecomp::AllegrexContext mk{};
        mk.gpr[4] = kStrAddr;
        mk.gpr[5] = 0u;  // attr
        mk.gpr[6] = 0u;  // initial count 0: unlocked
        check(hle_threadman_extra_dispatch(runtime, mk, kernel, kNidCreateMutex),
              "create mutex handled");
        const std::int32_t mtx = static_cast<std::int32_t>(mk.gpr[2]);
        check(mtx > 0, "mutex uid valid");

        psprecomp::AllegrexContext lk{};
        lk.gpr[4] = static_cast<std::uint32_t>(mtx);
        lk.gpr[5] = 1u;
        lk.gpr[6] = 0u;
        check(hle_threadman_extra_dispatch(runtime, lk, kernel, kNidLockMutex) && lk.gpr[2] == 0u,
              "lock of a free mutex acquires");
        check(hle_threadman_extra_dispatch(runtime, lk, kernel, kNidLockMutex) &&
                  lk.gpr[2] == 0x800201C8u,
              "non-recursive owner relock is ALREADY_LOCKED");

        // A mutex created with initial count 1 is owned by its creator.
        mk.gpr[6] = 1u;
        check(hle_threadman_extra_dispatch(runtime, mk, kernel, kNidCreateMutex),
              "create mutex (initial 1) handled");
        const std::int32_t mtx2 = static_cast<std::int32_t>(mk.gpr[2]);
        check(mtx2 > 0, "mutex2 uid valid");
        psprecomp::AllegrexContext un{};
        un.gpr[4] = static_cast<std::uint32_t>(mtx2);
        un.gpr[5] = 1u;
        check(hle_threadman_extra_dispatch(runtime, un, kernel, kNidUnlockMutex) && un.gpr[2] == 0u,
              "creator unlocks an initially-locked mutex");
        check(hle_threadman_extra_dispatch(runtime, un, kernel, kNidUnlockMutex) &&
                  un.gpr[2] == 0x800201C5u,
              "unlock of a free mutex is NOT_LOCKED");
        check(hle_threadman_extra_dispatch(runtime, un, kernel, kNidDeleteMutex) && un.gpr[2] == 0u,
              "delete mutex");
        psprecomp::AllegrexContext lk3{};
        lk3.gpr[4] = static_cast<std::uint32_t>(mtx2);
        lk3.gpr[5] = 1u;
        lk3.gpr[6] = 0u;
        check(hle_threadman_extra_dispatch(runtime, lk3, kernel, kNidLockMutex) &&
                  lk3.gpr[2] == 0x800200CBu,
              "lock of a deleted mutex is UNKNOWN_UID");

        // The loader still owns `mtx`. Delay so the worker becomes
        // current; its poll lock times out, its blocking lock waits.
        psprecomp::AllegrexContext live{};
        live.pc = kFakeStub;
        live.gpr[31] = 0x08805555u;
        live.gpr[4] = 100000u;
        hle_threadman_extra_dispatch(runtime, live, kernel, kNidDelayThread);
        check(kernel.current_uid() == worker, "worker current after loader delay");

        runtime.memory().store32(kResultAddr, 0u);  // zero timeout: poll
        live.pc = kFakeStub;
        live.gpr[31] = 0x08804444u;
        live.gpr[4] = static_cast<std::uint32_t>(mtx);
        live.gpr[5] = 1u;
        live.gpr[6] = kResultAddr;
        check(hle_threadman_extra_dispatch(runtime, live, kernel, kNidLockMutex) &&
                  live.gpr[2] == 0x800201A8u,
              "poll lock on an owned mutex is WAIT_TIMEOUT");
        check(kernel.find_thread(worker)->status == ThreadStatus::Running,
              "poll lock does not block");

        live.gpr[6] = 0u;  // no timeout: block
        check(hle_threadman_extra_dispatch(runtime, live, kernel, kNidLockMutex),
              "blocking lock handled");
        const P3PThread *wrec = kernel.find_thread(worker);
        check(wrec->status == ThreadStatus::Waiting &&
                  wrec->wait_kind == P3PThread::WaitKind::Mutex,
              "worker waits on the mutex");
        check(kernel.current_uid() == loader,
              "blocked worker hands the CPU back (loader's delay expires)");

        // Unlock hands ownership to the waiting worker.
        psprecomp::AllegrexContext un2{};
        un2.gpr[4] = static_cast<std::uint32_t>(mtx);
        un2.gpr[5] = 1u;
        check(hle_threadman_extra_dispatch(runtime, un2, kernel, kNidUnlockMutex) && un2.gpr[2] == 0u,
              "owner unlock succeeds");
        check(kernel.find_thread(worker)->status == ThreadStatus::Ready,
              "worker readied by the hand-off");

        // The loader's own lock attempt now blocks; the worker resumes
        // and its re-dispatched lock completes through the hand-off.
        psprecomp::AllegrexContext lk2{};
        lk2.pc = kFakeStub;
        lk2.gpr[31] = 0x08803333u;
        lk2.gpr[4] = static_cast<std::uint32_t>(mtx);
        lk2.gpr[5] = 1u;
        lk2.gpr[6] = 0u;
        check(hle_threadman_extra_dispatch(runtime, lk2, kernel, kNidLockMutex),
              "loader lock attempt handled");
        check(kernel.current_uid() == worker, "loader blocked, worker resumed");
        check(hle_threadman_extra_dispatch(runtime, lk2, kernel, kNidLockMutex) && lk2.gpr[2] == 0u,
              "handed-off lock completes on re-dispatch");

        // The worker unlocks (hand-off back to the loader) and exits; the
        // loader's re-dispatched lock acquires.
        check(hle_threadman_extra_dispatch(runtime, un2, kernel, kNidUnlockMutex) && un2.gpr[2] == 0u,
              "worker unlock hands the mutex back");
        check(kernel.exit_current_and_schedule_next(lk2), "exit hand-off to the loader");
        check(kernel.current_uid() == loader, "loader resumed at its lock call");
        check(hle_threadman_extra_dispatch(runtime, lk2, kernel, kNidLockMutex) && lk2.gpr[2] == 0u,
              "loader lock completes after the hand-off");
    }

    // --- Semaphores: counts, poll rules, overflow, blocking hand-off ---
    {
        psprecomp::Runtime runtime(32u * 1024u * 1024u);
        P3PKernel kernel;
        const std::int32_t loader = kernel.bootstrap_loader("loader", 0x08804108u, 0x20, 0u, 0x10000u);
        const std::int32_t worker = kernel.create_thread("worker", 0x0880C000u, 0x30, 0x10000u, 0u, 0u, runtime.memory());
        check(kernel.start_thread(worker, 0u, 0u, runtime.memory()) == 0, "sema-test worker started");

        put_str(runtime, kStrAddr, "sem");
        psprecomp::AllegrexContext mk{};
        mk.gpr[4] = kStrAddr;  // name
        mk.gpr[5] = 0u;        // attr
        mk.gpr[6] = 0u;        // initial count
        mk.gpr[7] = 5u;        // max count
        check(hle_threadman_extra_dispatch(runtime, mk, kernel, kNidCreateSema),
              "create sema handled");
        const std::int32_t sem = static_cast<std::int32_t>(mk.gpr[2]);
        check(sem > 0, "sema uid valid");

        // The loader waits for 3: it blocks, the worker runs.
        psprecomp::AllegrexContext wt{};
        wt.pc = kFakeStub;
        wt.gpr[31] = 0x08804444u;
        wt.gpr[4] = static_cast<std::uint32_t>(sem);
        wt.gpr[5] = 3u;
        wt.gpr[6] = 0u;
        check(hle_threadman_extra_dispatch(runtime, wt, kernel, kNidWaitSema),
              "loader wait handled");
        check(kernel.current_uid() == worker, "worker runs while the loader waits on the sema");
        check(kernel.find_thread(loader)->status == ThreadStatus::Waiting, "loader blocked on sema");

        // Signal 2: not enough for the waiter's 3, nobody wakes. A poll
        // by the worker still fails -- a poll may not jump the queue.
        psprecomp::AllegrexContext sg{};
        sg.gpr[4] = static_cast<std::uint32_t>(sem);
        sg.gpr[5] = 2u;
        check(hle_threadman_extra_dispatch(runtime, sg, kernel, kNidSignalSema) && sg.gpr[2] == 0u,
              "signal 2 accepted");
        check(kernel.find_thread(loader)->status == ThreadStatus::Waiting,
              "loader still waiting after a partial signal");
        psprecomp::AllegrexContext pol{};
        pol.gpr[4] = static_cast<std::uint32_t>(sem);
        pol.gpr[5] = 1u;
        check(hle_threadman_extra_dispatch(runtime, pol, kernel, kNidPollSema) &&
                  pol.gpr[2] == 0x800201ADu,
              "poll refuses to jump a waiting queue (SEMA_ZERO)");

        // Signal 1 more: the wait is satisfied, deducted at signal
        // time, and handed to the loader.
        sg.gpr[5] = 1u;
        check(hle_threadman_extra_dispatch(runtime, sg, kernel, kNidSignalSema) && sg.gpr[2] == 0u,
              "signal 1 completes the wait");
        check(kernel.find_thread(loader)->status == ThreadStatus::Ready, "loader readied by the signal");
        check(hle_threadman_extra_dispatch(runtime, pol, kernel, kNidPollSema) &&
                  pol.gpr[2] == 0x800201ADu,
              "handed-off counts were already deducted");

        // The worker exits; the loader's re-dispatched wait completes
        // through the hand-off.
        check(kernel.exit_current_and_schedule_next(wt), "worker exit hands over to the loader");
        check(kernel.current_uid() == loader, "loader resumed at its wait call");
        check(hle_threadman_extra_dispatch(runtime, wt, kernel, kNidWaitSema) && wt.gpr[2] == 0u,
              "handed-off wait completes on re-dispatch");

        // Overflow: 0 + 6 > max 5 changes nothing; 5 fits exactly.
        sg.gpr[5] = 6u;
        check(hle_threadman_extra_dispatch(runtime, sg, kernel, kNidSignalSema) &&
                  sg.gpr[2] == 0x800201AEu,
              "overflowing signal is SEMA_OVF");
        sg.gpr[5] = 5u;
        check(hle_threadman_extra_dispatch(runtime, sg, kernel, kNidSignalSema) && sg.gpr[2] == 0u,
              "signal to exactly max accepted");
        wt.gpr[5] = 5u;
        check(hle_threadman_extra_dispatch(runtime, wt, kernel, kNidWaitSema) && wt.gpr[2] == 0u,
              "wait for the full count acquires immediately");
        check(hle_threadman_extra_dispatch(runtime, pol, kernel, kNidPollSema) &&
                  pol.gpr[2] == 0x800201ADu,
              "poll on an empty sema is SEMA_ZERO");

        // Illegal counts and deletion.
        wt.gpr[5] = 0u;
        check(hle_threadman_extra_dispatch(runtime, wt, kernel, kNidWaitSema) &&
                  wt.gpr[2] == 0x800201BDu,
              "wait with count 0 is ILLEGAL_COUNT");
        psprecomp::AllegrexContext del{};
        del.gpr[4] = static_cast<std::uint32_t>(sem);
        check(hle_threadman_extra_dispatch(runtime, del, kernel, kNidDeleteSema) && del.gpr[2] == 0u,
              "delete sema");
        check(hle_threadman_extra_dispatch(runtime, pol, kernel, kNidPollSema) &&
                  pol.gpr[2] == 0x800200CBu,
              "poll on a deleted sema is an unknown object");
    }

    std::fprintf(stderr, "test_p3p_yield: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
