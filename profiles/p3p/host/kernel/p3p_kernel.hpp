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
    bool exit_current_and_schedule_next(psprecomp::AllegrexContext &live);

    [[nodiscard]] const P3PThread *find_thread(std::int32_t uid) const;
    [[nodiscard]] std::int32_t current_uid() const noexcept { return current_uid_; }
    [[nodiscard]] std::size_t thread_count() const noexcept { return threads_.size(); }
    [[nodiscard]] RangeAllocator &allocator() noexcept { return allocator_; }
    [[nodiscard]] const RangeAllocator &allocator() const noexcept { return allocator_; }

private:
    RangeAllocator allocator_;
    std::map<std::int32_t, P3PThread> threads_;
    std::int32_t next_uid_ = kFirstUid;
    std::int32_t current_uid_ = 0;
};

}  // namespace p3p
