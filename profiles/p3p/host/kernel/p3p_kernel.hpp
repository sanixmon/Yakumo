#pragma once

// P3P kernel: user-partition range allocator + cooperative thread lifecycle.
//
// Implements the Gate 4A-R3 specification (docs/p3p-recomp/reports/
// gate-4a-final-audit.md) and the contracts in docs/p3p-recomp/ARCHITECTURE.md
// sections 4-5. All P3P-specific kernel state lives in the profile (ADR-004);
// the core framework is never modified.
//
// Threading model: single guest CPU context owned by psprecomp::Runtime.
// Threads are records holding a saved AllegrexContext; the cooperative
// scheduler copies a Ready thread's context into the live context when the
// running thread exits through kExitStubAddress (ADR-002).

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"

#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace p3p {

// PSP kernel error codes used by the Gate 4A failure-atomicity matrix.
inline constexpr std::uint32_t kErrorIllegalAddr = 0x800200D3u;  // SCE_KERNEL_ERROR_ILLEGAL_ADDR
inline constexpr std::uint32_t kErrorNoMemory = 0x80020190u;     // SCE_KERNEL_ERROR_NO_MEMORY
inline constexpr std::uint32_t kErrorInvalidUid = 0x80020197u;   // thid <= 0 (Gate 4A matrix)
inline constexpr std::uint32_t kErrorUnknownUid = 0x80020198u;   // thid not found (Gate 4A matrix)
inline constexpr std::uint32_t kErrorNotDormant = 0x800201A4u;   // start on non-Dormant thread
inline constexpr std::uint32_t kErrorDoubleFree = 0x800201B6u;   // allocator double-free (Gate 4A invariant 5)
inline constexpr std::uint32_t kErrorIllegalArgument = 0x800200D2u;  // SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT
inline constexpr std::uint32_t kErrorUnknownEvfId = 0x8002019Au;     // SCE_KERNEL_ERROR_UNKNOWN_EVFID
inline constexpr std::uint32_t kErrorEvfCond = 0x800201AFu;          // SCE_KERNEL_ERROR_EVF_COND
inline constexpr std::uint32_t kErrorUnknownObject = 0x800200CBu;  // SCE_KERNEL_ERROR_UNKNOWN_UID (object pools)
inline constexpr std::uint32_t kErrorUnknownModule = 0x8002012Eu;   // SCE_KERNEL_ERROR_UNKNOWN_MODULE
inline constexpr std::uint32_t kErrorIllegalAttr = 0x80020191u;       // SCE_KERNEL_ERROR_ILLEGAL_ATTR
inline constexpr std::uint32_t kErrorWaitTimeout = 0x800201A8u;       // SCE_KERNEL_ERROR_WAIT_TIMEOUT
inline constexpr std::uint32_t kErrorIllegalCount = 0x800201BDu;      // SCE_KERNEL_ERROR_ILLEGAL_COUNT
inline constexpr std::uint32_t kMutexErrorNotLocked = 0x800201C5u;    // SCE_MUTEX_ERROR_NOT_LOCKED
inline constexpr std::uint32_t kMutexErrorLockOverflow = 0x800201C6u; // SCE_MUTEX_ERROR_LOCK_OVERFLOW
inline constexpr std::uint32_t kMutexErrorUnlockUnderflow = 0x800201C7u;  // SCE_MUTEX_ERROR_UNLOCK_UNDERFLOW
inline constexpr std::uint32_t kMutexErrorAlreadyLocked = 0x800201C8u;     // SCE_MUTEX_ERROR_ALREADY_LOCKED
inline constexpr std::uint32_t kErrorSemaZero = 0x800201ADu;        // SCE_KERNEL_ERROR_SEMA_ZERO
inline constexpr std::uint32_t kErrorSemaOvf = 0x800201AEu;         // SCE_KERNEL_ERROR_SEMA_OVF
inline constexpr std::uint32_t kErrorNotFoundUidFunc = 0x800200CEu;  // SCE_KERNEL_ERROR_NOT_FOUND_UIDFUNC

// RangeAllocator allocation types (mirrors sceKernelAllocPartitionMemory).
inline constexpr int kAllocBottomUp = 0;  // lowest fitting address
inline constexpr int kAllocTopDown = 1;   // highest fitting address (stacks)

// Coalescing free-range allocator over one contiguous guest partition.
// Invariants (Gate 4A section 4): allocated blocks never overlap each other
// or the free ranges; free ranges are sorted, non-adjacent (always coalesced)
// and never overlap; a failed allocation leaves the free list untouched; a
// double free is rejected with kErrorDoubleFree.
class RangeAllocator {
public:
    RangeAllocator(std::uint32_t base, std::uint32_t size);

    // Allocates `size` bytes (rounded up to `alignment`, which must be a
    // power of two). Returns the guest start address or std::nullopt without
    // modifying any state.
    [[nodiscard]] std::optional<std::uint32_t> allocate(std::uint32_t size, int type,
                                                        std::uint32_t alignment);

    // Frees the block starting exactly at `address`. Returns 0 on success or
    // kErrorDoubleFree when no live block starts there.
    std::int32_t free_block(std::uint32_t address);

    // Allocates exactly at `address` when [address, address+size) lies fully
    // inside one free range (sceKernelAllocPartitionMemory PSP_SMEM_Addr).
    [[nodiscard]] std::optional<std::uint32_t> allocate_at(std::uint32_t address,
                                                           std::uint32_t size);

    [[nodiscard]] bool is_allocated(std::uint32_t address) const;
    [[nodiscard]] std::uint32_t base() const noexcept { return base_; }
    [[nodiscard]] std::uint32_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t allocated_block_count() const noexcept { return allocated_.size(); }
    [[nodiscard]] std::size_t free_range_count() const noexcept { return free_ranges_.size(); }

private:
    struct Range {
        std::uint32_t start;
        std::uint32_t size;
    };

    std::uint32_t base_;
    std::uint32_t size_;
    std::vector<Range> free_ranges_;                        // sorted by start, coalesced
    std::map<std::uint32_t, std::uint32_t> allocated_;      // start -> size
};

// Result of the Gate 4A stack-layout computation (ARCHITECTURE.md section 4).
struct StackLayout {
    std::uint32_t tcb_address = 0;    // stack_top - 256; $k0 points here
    std::uint32_t sp_arg = 0;         // tcb_address - aligned_arglen; $a1 when arglen > 0
    std::uint32_t initial_sp = 0;     // sp_arg - 64
    std::uint32_t aligned_arglen = 0; // (arglen + 15) & ~15
};

// Computes the stack layout for a thread. Returns 0 on success,
// kErrorIllegalAddr when arglen would overflow the alignment rounding, or
// kErrorNoMemory when the arguments cannot fit the stack (Gate 4A section 1).
std::int32_t compute_stack_layout(std::uint32_t stack_bottom, std::uint32_t stack_size,
                                  std::uint32_t arglen, StackLayout &out);

enum class ThreadStatus : std::uint32_t {
    Dormant = 0,
    Ready = 1,
    Running = 2,
    Dead = 3,
    Deleted = 4,
    Waiting = 5,  // Gate 5C: blocked on a recorded wait (see P3PThread)
};

struct P3PThread {
    std::int32_t uid = 0;
    std::string name;
    std::uint32_t entry = 0;
    std::int32_t priority = 0;  // PSP semantics: lower value = higher priority
    std::uint32_t attr = 0;
    std::uint32_t gp = 0;
    std::uint32_t stack_address = 0;  // guest address of the stack block (bottom)
    std::uint32_t stack_size = 0;     // allocated stack size in bytes
    ThreadStatus status = ThreadStatus::Dormant;
    psprecomp::AllegrexContext context{};  // saved context; live once Running
    // Gate 5C: what a Waiting thread is blocked on. Event-flag waiters store
    // the wait so the kernel can re-evaluate satisfaction when bits change;
    // the blocked call itself stays pointed at its import stub in the saved
    // context and is re-dispatched when the thread is scheduled again.
    enum class WaitKind : std::uint32_t { None = 0, EventFlag = 1, Sleep = 2, Mutex = 3, ThreadSleep = 4, Semaphore = 5, ThreadEnd = 6 };
    WaitKind wait_kind = WaitKind::None;
    std::int32_t wait_evf_uid = 0;
    std::uint32_t wait_pattern = 0;
    std::uint32_t wait_mode = 0;
    std::int32_t wait_mutex_uid = 0;
    std::int32_t wakeup_count = 0;  // pending sceKernelWakeupThread calls
    std::int32_t wait_mutex_count = 0;
    // Set when an unlock handed this thread ownership of a mutex while it
    // was blocked in LockMutex: its re-dispatched lock call completes
    // through lock_mutex's hand-off branch instead of acquiring again.
    std::int32_t mutex_handed_uid = 0;
    std::int32_t wait_sema_uid = 0;
    std::int32_t wait_sema_count = 0;
    // Set when a signal satisfied this thread's wait while it was
    // blocked: the count was already deducted at signal time, and the
    // re-dispatched wait completes through wait_semaphore's hand-off
    // branch instead of deducting again.
    std::int32_t sema_handed_uid = 0;
    // sceKernelWaitThreadEnd: the thread this one is blocked on, the
    // hand-off marker set when that thread exits (the re-dispatched
    // wait then returns thread_end_result), and this thread's own exit
    // status once it has exited.
    std::int32_t wait_thread_uid = 0;
    std::int32_t thread_end_handed_uid = 0;
    std::int32_t thread_end_result = 0;
    std::int32_t exit_status = 0;
    // Gate 5C timed waits (DelayThread, vblank pacing, audio drain): while
    // timed_wait is set, a Waiting thread becomes Ready again once the
    // kernel's fake clock reaches wake_at_us.
    bool timed_wait = false;
    std::uint64_t wake_at_us = 0;
};

class P3PKernel {
public:
    static constexpr std::uint32_t kExitStubAddress = 0x08800000u;  // ADR-002
    static constexpr std::uint32_t kUserPartitionBase = 0x08EC1000u;
    // ARCHITECTURE.md documents the user partition as ending at 0x0A800000
    // (0x193F000 bytes), which assumes a 64 MiB address model. The runtime
    // uses the 32 MiB model (ARCHITECTURE.md section 6), whose guest RAM ends
    // at 0x0A000000, so the partition is clamped to RAM: 0x113F000 bytes.
    static constexpr std::uint32_t kRamEnd = 0x0A000000u;
    static constexpr std::uint32_t kUserPartitionSize = kRamEnd - kUserPartitionBase;
    static constexpr std::int32_t kFirstUid = 100;  // ADR-003
    static constexpr std::uint32_t kStackAlignment = 256u;
    static constexpr std::uint32_t kTcbSize = 256u;
    static constexpr std::uint32_t kAbiReserve = 64u;
    static constexpr std::uint32_t kMinStackOverhead = kTcbSize + kAbiReserve;  // 320

    P3PKernel();
    explicit P3PKernel(RangeAllocator allocator) : allocator_(std::move(allocator)) {}

    // Registers the initial (loader) thread as Running and reserves its stack
    // from the top of the partition. The caller owns the live register setup.
    std::int32_t bootstrap_loader(const std::string &name, std::uint32_t entry,
                                  std::int32_t priority, std::uint32_t gp,
                                  std::uint32_t stack_size);

    // sceKernelCreateThread core. Returns the new UID (> 0) or a negative
    // error code (as int32_t). The new thread is Dormant with a saved context
    // whose $pc/$gp are set; $sp/$a0/$a1 are finalized by start_thread.
    // Following the reference kernel, the stack is filled with 0xFF and the
    // thread UID is written at the stack bottom.
    std::int32_t create_thread(const std::string &name, std::uint32_t entry,
                               std::int32_t priority, std::uint32_t stack_size,
                               std::uint32_t attr, std::uint32_t gp,
                               psprecomp::GuestMemory &memory);

    // sceKernelStartThread core, following the Gate 4A failure-atomicity
    // matrix exactly: on any error, guest memory, the thread record and the
    // ready set are all unmodified. On success the thread becomes Ready and
    // its saved context is fully initialized (arguments staged via a host
    // buffer first, ADR-005). Returns 0 or a negative error code.
    std::int32_t start_thread(std::int32_t thid, std::uint32_t arglen, std::uint32_t argp,
                              psprecomp::GuestMemory &memory);

    // Thread-exit hand-off (ARCHITECTURE.md section 5): marks the current
    // thread Dead, reclaims its stack, then loads the highest-priority Ready
    // thread's context into `live` and marks it Running. Returns false when
    // no Ready thread remains (`live` is then untouched).
    bool exit_current_and_schedule_next(psprecomp::AllegrexContext &live,
                                        std::int32_t exit_status = 0);
    // sceKernelWaitThreadEnd support: record the current thread as
    // blocked on `uid` (optionally with a fake-clock timeout), and the
    // hand-off probe the re-dispatched wait uses to complete.
    void note_wait_thread_end(std::int32_t uid, bool has_timeout, std::uint64_t timeout_us);
    bool take_thread_end_result(std::int32_t uid, std::int32_t &out);

    // Gate 5C cooperative scheduling points. yield_current saves the live
    // context into the current thread, marks it Ready, and loads the
    // highest-priority OTHER Ready thread (priority, then lowest UID — the
    // exit hand-off's rule). block_current is identical except the current
    // thread is marked Waiting. Both return false with `live` and every
    // record untouched when no other thread can run, so the caller keeps
    // the CPU and decides how to complete the call.
    bool yield_current(psprecomp::AllegrexContext &live);
    bool block_current(psprecomp::AllegrexContext &live);

    // Gate 5C timed sleep: like block_current, but the thread wakes once
    // the kernel's fake clock reaches now + duration_us. When no thread
    // is Ready, rescheduling skips the clock forward to the next wake, so
    // a lone sleeping thread simply continues after its (virtual) delay.
    bool sleep_current(psprecomp::AllegrexContext &live, std::uint64_t duration_us);
    // sceKernelSleepThread core: blocks the current thread indefinitely
    // (WaitKind::ThreadSleep) until wakeup_thread. A pending wakeup
    // count is consumed instead and the call does not block (returns
    // false). Returns true when the thread actually blocked (its saved
    // context already points past the call).
    bool sleep_thread_current(psprecomp::AllegrexContext &live);
    // sceKernelWakeupThread core: wakes a ThreadSleep waiter, or banks
    // a wakeup count on the target. 0 on success, error code otherwise.
    std::int32_t wakeup_thread(std::int32_t uid);
    [[nodiscard]] std::uint64_t now_us() const noexcept { return now_us_; }

    // Records (or clears, with clear_wait) the current thread's pending
    // wait. set_event_flag/delete_event_flag wake satisfied/dead waiters.
    void note_wait_event_flag(std::int32_t evf_uid, std::uint32_t pattern, std::uint32_t mode);
    void clear_wait();

    // Mutexes (sceKernelCreateMutex family), modelled on the reference
    // (PPSSPP sceKernelMutex.cpp): a lock level plus an owner; recursive
    // re-locking only with attr 0x200; an initial count > 0 creates the
    // mutex already locked by its creator. A blocked LockMutex waits as
    // WaitKind::Mutex; unlock hands ownership straight to the best
    // waiter (FIFO, or priority order with attr 0x100), whose resumed
    // lock call then completes through lock_mutex's hand-off branch.
    enum class MutexLockResult : std::uint32_t { Acquired = 0, Blocked = 1, Error = 2 };
    std::int32_t create_mutex(const std::string &name, std::uint32_t attr,
                              std::int32_t initial_count);
    std::int32_t delete_mutex(std::int32_t uid);
    MutexLockResult lock_mutex(std::int32_t uid, std::int32_t count, std::int32_t &error);
    std::int32_t unlock_mutex(std::int32_t uid, std::int32_t count);
    void note_wait_mutex(std::int32_t mutex_uid, std::int32_t count);
    [[nodiscard]] std::size_t mutex_count() const noexcept { return mutexes_.size(); }

    // Semaphores (sceKernelCreateSema family), modelled on PPSSPP
    // sceKernelSemaphore.cpp: a count between 0 and a maximum; a wait
    // deducts its wanted count when satisfied. Blocked waits record
    // WaitKind::Semaphore; a signal deducts for each waiter it wakes
    // (in waiter order, priority order under attr 0x100) and hands the
    // acquisition to the woken thread, whose re-dispatched wait then
    // completes through wait_semaphore's hand-off branch. Poll
    // additionally refuses to deduct while any thread is waiting.
    enum class SemaWaitResult : std::uint32_t { Acquired = 0, Blocked = 1, Error = 2 };
    std::int32_t create_semaphore(const std::string &name, std::uint32_t attr,
                                  std::int32_t initial_count, std::int32_t max_count);
    std::int32_t delete_semaphore(std::int32_t uid);
    SemaWaitResult wait_semaphore(std::int32_t uid, std::int32_t count, std::int32_t &error);
    std::int32_t poll_semaphore(std::int32_t uid, std::int32_t count);
    std::int32_t signal_semaphore(std::int32_t uid, std::int32_t count);
    void note_wait_semaphore(std::int32_t sema_uid, std::int32_t count);
    [[nodiscard]] std::size_t semaphore_count() const noexcept { return semaphores_.size(); }

    [[nodiscard]] const P3PThread *find_thread(std::int32_t uid) const;
    [[nodiscard]] std::int32_t current_uid() const noexcept { return current_uid_; }
    [[nodiscard]] std::size_t thread_count() const noexcept { return threads_.size(); }
    [[nodiscard]] RangeAllocator &allocator() noexcept { return allocator_; }
    [[nodiscard]] const RangeAllocator &allocator() const noexcept { return allocator_; }

    // --- Gate 5B: memory blocks, callbacks, event flags, main module -----
    // All UIDs come from the same counter as threads (ADR-003).

    // sceKernelAllocPartitionMemory core. `type` is the PSP_SMEM_* value
    // (0=Low, 1=High, 2=Addr, 3=LowAligned, 4=HighAligned); for Addr,
    // `addr` is the requested address, for the Aligned types it is the
    // alignment. Partitions other than the user partition (2) are served
    // from the same allocator: the profile models a single partition
    // (ARCHITECTURE.md section 6). Returns the block UID (> 0) or a
    // negative error code.
    std::int32_t alloc_partition_memory(std::int32_t partition, const std::string &name,
                                        std::int32_t type, std::uint32_t size,
                                        std::uint32_t addr);
    // sceKernelFreePartitionMemory core. Returns 0 or a negative error code.
    std::int32_t free_partition_memory(std::int32_t uid);
    // sceKernelGetBlockHeadAddr core. Returns 0 when the UID is unknown.
    [[nodiscard]] std::uint32_t block_head_address(std::int32_t uid) const;
    [[nodiscard]] std::size_t memory_block_count() const noexcept { return memory_blocks_.size(); }

    // sceKernelCreateCallback/DeleteCallback/CheckCallback cores. Callbacks
    // belong to the current thread; check_callbacks counts the current
    // thread's callbacks with a pending notification.
    std::int32_t create_callback(const std::string &name, std::uint32_t func,
                                 std::uint32_t arg);
    std::int32_t delete_callback(std::int32_t uid);
    // sceKernelDeleteThread (PPSSPP semantics): only a thread that has
    // already stopped may be deleted. This kernel transitions exiting
    // threads straight to Deleted (stack freed), so deleting an exited
    // thread is a successful no-op; deleting a live or current thread
    // is NOT_DORMANT; an unknown uid is ILLEGAL_THREAD_ID.
    std::int32_t delete_thread(std::int32_t uid);
    std::int32_t notify_callback(std::int32_t uid, std::uint32_t arg);
    [[nodiscard]] std::int32_t check_callbacks() const;

    // sceKernelCreateEventFlag family cores.
    std::int32_t create_event_flag(const std::string &name, std::uint32_t attr,
                                   std::uint32_t init_pattern);
    std::int32_t delete_event_flag(std::int32_t uid);
    std::int32_t set_event_flag(std::int32_t uid, std::uint32_t bits);
    std::int32_t clear_event_flag(std::int32_t uid, std::uint32_t bits);
    [[nodiscard]] bool event_flag_pattern(std::int32_t uid, std::uint32_t &out) const;

    // sceKernelChangeCurrentThreadAttr core.
    std::int32_t change_current_thread_attr(std::uint32_t clear_mask, std::uint32_t set_mask);
    // sceKernelGetThreadCurrentPriority / sceKernelChangeThreadPriority
    // cores (Gate 5C follow-up). A uid of 0 means the current thread.
    // thread_priority returns the priority or a negative error.
    // change_thread_priority stores the new priority; the cooperative
    // scheduler applies it at the next scheduling point (no mid-timeslice
    // preemption), which is where every guest wait already reschedules.
    [[nodiscard]] std::int32_t thread_priority(std::int32_t uid) const;
    std::int32_t change_thread_priority(std::int32_t uid, std::int32_t priority);

    // Registers the main module's address span and returns its UID
    // (idempotent). Backs ModuleMgrForUser::sceKernelGetModuleIdByAddress.
    std::int32_t register_main_module(std::uint32_t base, std::uint32_t size);
    [[nodiscard]] std::int32_t module_uid_at(std::uint32_t address) const;
    [[nodiscard]] std::int32_t main_module_uid() const noexcept { return main_module_uid_; }

    // --- Gate 5D: runtime-loaded modules (sceKernelLoadModule) -----------
    struct ModuleRecord {
        std::string name;
        std::uint32_t base = 0;
        std::uint32_t size = 0;
        std::uint32_t gp = 0;
        std::uint32_t start_entry = 0;  // module_start, absolute (0 = none)
        bool started = false;
        std::int32_t block_uid = 0;  // partition block holding the image
    };
    std::int32_t register_module(const std::string &name, std::uint32_t base, std::uint32_t size,
                                 std::uint32_t gp, std::uint32_t start_entry,
                                 std::int32_t block_uid);
    [[nodiscard]] const ModuleRecord *find_module(std::int32_t uid) const;
    [[nodiscard]] std::int32_t find_module_by_name(const std::string &name) const;
    bool set_module_started(std::int32_t uid, bool started);
    // Removes the record and frees its partition block. False if unknown.
    bool unregister_module(std::int32_t uid);

    // Diagnostics: one line per thread (status, wait target, saved pc).
    void dump_thread_states(std::FILE *out) const;

private:
    P3PThread *pick_ready_excluding(std::int32_t excluded_uid);
    bool switch_away(psprecomp::AllegrexContext &live, ThreadStatus new_status, bool timed,
                     std::uint64_t wake_at_us, const char *reason);
    // Advances the fake clock to the earliest timed wake and readies every
    // thread due by then. Returns false when no timed wait exists.
    bool wake_due_timed_waiters();
    void wake_event_flag_waiters(std::int32_t evf_uid);

    RangeAllocator allocator_;
    std::map<std::int32_t, P3PThread> threads_;
    std::int32_t next_uid_ = kFirstUid;
    std::int32_t current_uid_ = 0;
    std::uint64_t now_us_ = 0;  // Gate 5C fake clock for timed waits

    struct MemoryBlock {
        std::string name;
        std::int32_t partition = 0;
        std::uint32_t address = 0;
        std::uint32_t size = 0;
    };
    struct Callback {
        std::string name;
        std::int32_t thread_uid = 0;
        std::uint32_t func = 0;
        std::uint32_t arg = 0;
        std::uint32_t notify_count = 0;
        std::uint32_t notify_arg = 0;
    };
    struct EventFlag {
        std::string name;
        std::uint32_t attr = 0;
        std::uint32_t pattern = 0;
    };
    struct Mutex {
        std::string name;
        std::uint32_t attr = 0;
        std::int32_t lock_level = 0;
        std::int32_t owner_uid = 0;
        std::vector<std::int32_t> waiters;  // FIFO
    };
    std::map<std::int32_t, MemoryBlock> memory_blocks_;
    std::map<std::int32_t, Callback> callbacks_;
    std::map<std::int32_t, EventFlag> event_flags_;
    std::map<std::int32_t, Mutex> mutexes_;
    struct Semaphore {
        std::string name;
        std::uint32_t attr = 0;
        std::int32_t count = 0;
        std::int32_t max_count = 0;
        std::vector<std::int32_t> waiters;  // FIFO
    };
    std::map<std::int32_t, Semaphore> semaphores_;
    std::map<std::int32_t, ModuleRecord> modules_;
    std::int32_t main_module_uid_ = 0;
    std::uint32_t main_module_base_ = 0;
    std::uint32_t main_module_size_ = 0;
};

}  // namespace p3p
