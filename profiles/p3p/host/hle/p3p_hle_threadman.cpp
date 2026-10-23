#include "p3p_hle_threadman.hpp"

#include "kernel/p3p_kernel.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/guest_memory.hpp"

#include <cstdio>
#include <set>
#include <utility>

#include <string>

namespace p3p {

namespace {

// Reads a NUL-terminated guest string, bounded to `max_length` bytes.
// Returns false when the pointer is not readable at all.
bool read_guest_string(psprecomp::GuestMemory &memory, std::uint32_t address,
                       std::size_t max_length, std::string &out) {
    if (address == 0u || !memory.contains(address, 1u))
        return false;
    out.clear();
    for (std::size_t i = 0; i < max_length; ++i) {
        if (!memory.contains(address + static_cast<std::uint32_t>(i), 1u))
            break;
        const char c = static_cast<char>(memory.load8(address + static_cast<std::uint32_t>(i)));
        if (c == '\0')
            return true;
        out.push_back(c);
    }
    return true;
}

}  // namespace

std::int32_t hle_create_thread(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                               P3PKernel &kernel) {
    psprecomp::GuestMemory &memory = runtime.memory();
    const std::uint32_t name_address = ctx.gpr[4];   // $a0
    const std::uint32_t entry = ctx.gpr[5];          // $a1
    const auto priority = static_cast<std::int32_t>(ctx.gpr[6]);  // $a2
    const std::uint32_t stack_size = ctx.gpr[7];     // $a3
    // EABI argument layout (PPSSPP Core/HLE/HLE.cpp): arguments 5-8 travel
    // in $t0-$t3, so attr is $t0 here (the option pointer would be $t1).
    const std::uint32_t attr = ctx.gpr[8];

    std::string name;
    std::int32_t result;
    if (!read_guest_string(memory, name_address, 32u, name)) {
        result = static_cast<std::int32_t>(kErrorIllegalAddr);
    } else {
        // A new thread inherits the creator's $gp (the module $gp for threads
        // created by module_start).
        result = kernel.create_thread(name, entry, priority, stack_size, attr, ctx.gpr[28], memory);
        if (result > 0)
            std::fprintf(stderr, "[p3p] create thread '%s' prio %d entry %s -> uid %d\n",
                         name.c_str(), priority, psprecomp::hex32(entry).c_str(), result);
    }
    ctx.set_gpr(2, static_cast<std::uint32_t>(result));  // $v0
    return result;
}

std::int32_t hle_start_thread(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                              P3PKernel &kernel) {
    const auto thid = static_cast<std::int32_t>(ctx.gpr[4]);  // $a0
    const std::uint32_t arglen = ctx.gpr[5];                  // $a1
    const std::uint32_t argp = ctx.gpr[6];                    // $a2
    const std::int32_t result = kernel.start_thread(thid, arglen, argp, runtime.memory());
    if (result == 0)
        std::fprintf(stderr, "[p3p] start thread uid %d\n", thid);
    ctx.set_gpr(2, static_cast<std::uint32_t>(result));  // $v0
    return result;
}

void register_threadman(psprecomp::Runtime &runtime, P3PKernel &kernel) {
    runtime.register_hle(kThreadManLibrary, kNidCreateThread,
                         [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                             hle_create_thread(rt, ctx, kernel);
                         });
    runtime.register_hle(kThreadManLibrary, kNidStartThread,
                         [&kernel](psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
                             hle_start_thread(rt, ctx, kernel);
                         });
}

namespace {

// Fake clock for sceKernelGetSystemTimeLow / sceKernelDelayThread. Headless
// runs have no real timer thread; the clock advances by the requested delay
// and by a fixed step per read so guest timing loops always make progress.
std::uint32_t g_fake_time_us = 0;

void set_v0(psprecomp::AllegrexContext &ctx, std::int32_t result) {
    ctx.set_gpr(2, static_cast<std::uint32_t>(result));
}

}  // namespace

bool hle_threadman_extra_dispatch(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                                  P3PKernel &kernel, std::uint32_t nid) {
    psprecomp::GuestMemory &memory = runtime.memory();
    const auto a0 = static_cast<std::int32_t>(ctx.gpr[4]);
    switch (nid) {
        case kNidCreateCallback: {
            std::string name;
            if (!read_guest_string(memory, ctx.gpr[4], 32u, name)) {
                set_v0(ctx, static_cast<std::int32_t>(kErrorIllegalAddr));
                return true;
            }
            set_v0(ctx, kernel.create_callback(name, ctx.gpr[5], ctx.gpr[6]));
            return true;
        }
        case 0x9FA03CD3u:  // sceKernelDeleteThread
            set_v0(ctx, kernel.delete_thread(a0));
            return true;
        case 0x278C0DF5u: {  // sceKernelWaitThreadEnd(uid, timeoutPtr)
            // Completion hand-off from the target's exit.
            std::int32_t exit_result = 0;
            if (kernel.take_thread_end_result(a0, exit_result)) {
                set_v0(ctx, exit_result);
                return true;
            }
            const P3PThread *target = kernel.find_thread(a0);
            if (target == nullptr || a0 == kernel.current_uid()) {
                set_v0(ctx, static_cast<std::int32_t>(0x80020198u));  // ILLEGAL_THREAD_ID
                return true;
            }
            if (target->status == ThreadStatus::Dead ||
                target->status == ThreadStatus::Deleted) {
                set_v0(ctx, target->exit_status);
                return true;
            }
            // Target still alive. A re-dispatch whose wait record is
            // gone was woken by its timeout (the exit wake records a
            // hand-off instead): report the timeout rather than
            // blocking again.
            const P3PThread *self = kernel.find_thread(kernel.current_uid());
            if (self != nullptr && self->wait_thread_uid == a0 &&
                self->wait_kind != P3PThread::WaitKind::ThreadEnd) {
                kernel.clear_wait();
                set_v0(ctx, static_cast<std::int32_t>(kErrorWaitTimeout));
                return true;
            }
            const std::uint32_t timeout_addr = ctx.gpr[5];
            const bool has_timeout =
                timeout_addr != 0u && memory.contains(timeout_addr, 4u);
            const std::uint32_t timeout_us =
                has_timeout ? memory.load32(timeout_addr) : 0u;
            if (has_timeout && timeout_us == 0u) {
                set_v0(ctx, static_cast<std::int32_t>(kErrorWaitTimeout));
                return true;
            }
            kernel.note_wait_thread_end(a0, has_timeout, timeout_us);
            if (kernel.block_current(ctx))
                return true;
            kernel.clear_wait();
            set_v0(ctx, static_cast<std::int32_t>(kErrorWaitTimeout));
            return true;
        }
        case kNidDeleteCallback:
            set_v0(ctx, kernel.delete_callback(a0));
            return true;
        case kNidCheckCallback:
            set_v0(ctx, kernel.check_callbacks());
            return true;
        case kNidCreateEventFlag: {
            std::string name;
            if (!read_guest_string(memory, ctx.gpr[4], 32u, name)) {
                set_v0(ctx, static_cast<std::int32_t>(kErrorIllegalAddr));
                return true;
            }
            set_v0(ctx, kernel.create_event_flag(name, ctx.gpr[5], ctx.gpr[6]));
            return true;
        }
        case kNidSetEventFlag:
            set_v0(ctx, kernel.set_event_flag(a0, ctx.gpr[5]));
            return true;
        case kNidClearEventFlag:
            set_v0(ctx, kernel.clear_event_flag(a0, ctx.gpr[5]));
            return true;
        case kNidDeleteEventFlag:
            set_v0(ctx, kernel.delete_event_flag(a0));
            return true;
        case kNidWaitEventFlag:
        case kNidWaitEventFlagCB: {
            const std::uint32_t wait_pattern = ctx.gpr[5];
            const std::uint32_t mode = ctx.gpr[6];
            const std::uint32_t result_addr = ctx.gpr[7];
            const std::uint32_t timeout_addr = ctx.gpr[8];  // EABI $t0
            std::uint32_t pattern = 0;
            if (!kernel.event_flag_pattern(a0, pattern)) {
                set_v0(ctx, static_cast<std::int32_t>(kErrorUnknownEvfId));
                return true;
            }
            const bool satisfied = (mode & 1u) != 0u ? (pattern & wait_pattern) != 0u
                                                     : (pattern & wait_pattern) == wait_pattern;
            if (satisfied) {
                kernel.clear_wait();
                if (result_addr != 0u)
                    memory.store32(result_addr, pattern);
                if ((mode & 0x10u) != 0u)
                    kernel.clear_event_flag(a0, 0xFFFFFFFFu);      // WAITCLEARALL
                else if ((mode & 0x20u) != 0u)
                    kernel.clear_event_flag(a0, wait_pattern);     // WAITCLEARPATTERN
                set_v0(ctx, 0);
                return true;
            }
            // Poll form (timeout pointer to a zero timeout) fails at once.
            if (timeout_addr != 0u && memory.contains(timeout_addr, 4u) &&
                memory.load32(timeout_addr) == 0u) {
                set_v0(ctx, static_cast<std::int32_t>(kErrorEvfCond));
                return true;
            }
            // Gate 5C: block for real. The wait is recorded on the thread and
            // its saved context keeps pointing at this call, so when the
            // thread is scheduled again the call re-dispatches and is
            // re-evaluated (completing or blocking again). $v0/$pc must not
            // be touched once the context has been switched away.
            kernel.note_wait_event_flag(a0, wait_pattern, mode);
            if (kernel.block_current(ctx))
                return true;
            kernel.clear_wait();
            // No other thread can run, so blocking would deadlock the host.
            // Fall back to the Gate 5B behaviour (log once, report success).
            static std::set<std::pair<std::int32_t, std::uint32_t>> logged;
            if (logged.emplace(a0, wait_pattern).second)
                std::fprintf(stderr,
                             "[p3p] wait event flag uid %d would block (pattern %s, "
                             "wait %s, mode %s); returning success (headless)\n",
                             a0, psprecomp::hex32(pattern).c_str(),
                             psprecomp::hex32(wait_pattern).c_str(),
                             psprecomp::hex32(mode).c_str());
            if (result_addr != 0u)
                memory.store32(result_addr, pattern);
            set_v0(ctx, 0);
            return true;
        }
        case kNidCreateMutex: {
            std::string name;
            if (!read_guest_string(memory, ctx.gpr[4], 32u, name)) {
                set_v0(ctx, static_cast<std::int32_t>(kErrorIllegalAddr));
                return true;
            }
            const std::int32_t uid = kernel.create_mutex(
                name, ctx.gpr[5], static_cast<std::int32_t>(ctx.gpr[6]));
            if (uid > 0)
                std::fprintf(stderr, "[p3p] create mutex '%s' attr %s count %d -> uid %d\n",
                             name.c_str(), psprecomp::hex32(ctx.gpr[5]).c_str(),
                             static_cast<std::int32_t>(ctx.gpr[6]), uid);
            set_v0(ctx, uid);
            return true;
        }
        case kNidDeleteMutex:
            set_v0(ctx, kernel.delete_mutex(a0));
            return true;
        case kNidLockMutex:
        case kNidLockMutexCB: {
            const std::int32_t count = static_cast<std::int32_t>(ctx.gpr[5]);
            const std::uint32_t timeout_addr = ctx.gpr[6];
            std::int32_t error = 0;
            const P3PKernel::MutexLockResult result = kernel.lock_mutex(a0, count, error);
            if (result == P3PKernel::MutexLockResult::Acquired) {
                set_v0(ctx, 0);
                return true;
            }
            if (result == P3PKernel::MutexLockResult::Error) {
                set_v0(ctx, error);
                return true;
            }
            // Would block. A zero timeout makes the call a poll.
            if (timeout_addr != 0u && memory.contains(timeout_addr, 4u) &&
                memory.load32(timeout_addr) == 0u) {
                set_v0(ctx, static_cast<std::int32_t>(kErrorWaitTimeout));
                return true;
            }
            // Block for real (Gate 5C): the saved context stays on this
            // call; an unlock hands ownership over and readies the
            // thread, and the re-dispatched lock completes through the
            // kernel's hand-off branch.
            kernel.note_wait_mutex(a0, count);
            if (kernel.block_current(ctx))
                return true;
            kernel.clear_wait();
            static std::set<std::int32_t> mutex_logged;
            if (mutex_logged.emplace(a0).second)
                std::fprintf(stderr,
                             "[p3p] lock mutex uid %d would block with no thread "
                             "to run; returning success (headless)\n",
                             a0);
            set_v0(ctx, 0);
            return true;
        }
        case kNidUnlockMutex:
            set_v0(ctx, kernel.unlock_mutex(a0, static_cast<std::int32_t>(ctx.gpr[5])));
            return true;
        case kNidCreateSema: {
            std::string name;
            if (!read_guest_string(memory, ctx.gpr[4], 32u, name)) {
                set_v0(ctx, static_cast<std::int32_t>(kErrorIllegalAddr));
                return true;
            }
            const std::int32_t uid = kernel.create_semaphore(
                name, ctx.gpr[5], static_cast<std::int32_t>(ctx.gpr[6]),
                static_cast<std::int32_t>(ctx.gpr[7]));
            if (uid > 0)
                std::fprintf(stderr, "[p3p] create sema '%s' attr %s init %d max %d -> uid %d\n",
                             name.c_str(), psprecomp::hex32(ctx.gpr[5]).c_str(),
                             static_cast<std::int32_t>(ctx.gpr[6]),
                             static_cast<std::int32_t>(ctx.gpr[7]), uid);
            set_v0(ctx, uid);
            return true;
        }
        case kNidDeleteSema:
            set_v0(ctx, kernel.delete_semaphore(a0));
            return true;
        case kNidWaitSema:
        case kNidWaitSemaCB: {
            const std::int32_t count = static_cast<std::int32_t>(ctx.gpr[5]);
            const std::uint32_t timeout_addr = ctx.gpr[6];
            std::int32_t error = 0;
            const P3PKernel::SemaWaitResult result = kernel.wait_semaphore(a0, count, error);
            if (result == P3PKernel::SemaWaitResult::Acquired) {
                set_v0(ctx, 0);
                return true;
            }
            if (result == P3PKernel::SemaWaitResult::Error) {
                set_v0(ctx, error);
                return true;
            }
            // Would block. A zero timeout makes the call a poll.
            if (timeout_addr != 0u && memory.contains(timeout_addr, 4u) &&
                memory.load32(timeout_addr) == 0u) {
                set_v0(ctx, static_cast<std::int32_t>(kErrorWaitTimeout));
                return true;
            }
            // Block for real: a signal that satisfies the wait deducts
            // the count and readies the thread; the re-dispatched wait
            // completes through the kernel's hand-off branch.
            kernel.note_wait_semaphore(a0, count);
            if (kernel.block_current(ctx))
                return true;
            kernel.clear_wait();
            static std::set<std::int32_t> sema_logged;
            if (sema_logged.emplace(a0).second)
                std::fprintf(stderr,
                             "[p3p] wait sema uid %d would block with no thread "
                             "to run; returning success (headless)\n",
                             a0);
            set_v0(ctx, 0);
            return true;
        }
        case kNidSignalSema: {
            set_v0(ctx, kernel.signal_semaphore(a0, static_cast<std::int32_t>(ctx.gpr[5])));
            return true;
        }
            return true;
        case kNidPollSema:
            set_v0(ctx, kernel.poll_semaphore(a0, static_cast<std::int32_t>(ctx.gpr[5])));
            return true;
        case kNidGetThreadCurrentPriority:
            set_v0(ctx, kernel.thread_priority(0));
            return true;
        case kNidChangeThreadPriority:
            set_v0(ctx, kernel.change_thread_priority(
                            a0, static_cast<std::int32_t>(ctx.gpr[5])));
            return true;
        case kNidChangeCurrentThreadAttr:
            set_v0(ctx, kernel.change_current_thread_attr(ctx.gpr[4], ctx.gpr[5]));
            return true;
        case kNidGetSystemTimeLow:
            g_fake_time_us += 100u;
            set_v0(ctx, static_cast<std::int32_t>(g_fake_time_us));
            return true;
        case kNidSleepThread:
            // Blocks indefinitely until another thread calls
            // sceKernelWakeupThread. (As a return-0 stub this was the
            // boot's late freeze: the 'SDK WRAP' thread's main loop is
            // work + SleepThread, so the stub made it spin at full
            // speed and starve every other thread.)
            set_v0(ctx, 0);
            ctx.pc = ctx.gpr[31];
            kernel.sleep_thread_current(ctx);
            return true;
        case kNidWakeupThread:
            set_v0(ctx, static_cast<std::uint32_t>(kernel.wakeup_thread(
                            static_cast<std::int32_t>(ctx.gpr[4]))));
            return true;
        case kNidDelayThread:
        case kNidDelayThreadCB:
            g_fake_time_us += ctx.gpr[4];
            // Gate 5C: a delay blocks in the reference kernel. Preset
            // the return value and the resume address (the caller's $ra)
            // so the saved context continues right after this call, then
            // sleep for the delay on the kernel's fake clock: other Ready
            // threads run meanwhile, and when none can, the clock skips
            // forward and execution simply continues at $ra.
            set_v0(ctx, 0);
            ctx.pc = ctx.gpr[31];
            kernel.sleep_current(ctx, ctx.gpr[4]);
            return true;
        case kNidExitThread:
            if (!kernel.exit_current_and_schedule_next(
                    ctx, static_cast<std::int32_t>(ctx.gpr[4])))
                runtime.stop("p3p: sceKernelExitThread with no ready thread remaining");
            return true;
        default:
            return false;
    }
}

}  // namespace p3p

