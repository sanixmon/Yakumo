#include "p3p_kernel.hpp"

#include <algorithm>
#include <utility>

namespace p3p {

namespace {

std::uint64_t align_up_u64(std::uint64_t value, std::uint32_t alignment) {
    return (value + (alignment - 1u)) & ~static_cast<std::uint64_t>(alignment - 1u);
}

std::uint64_t align_down_u64(std::uint64_t value, std::uint32_t alignment) {
    return value & ~static_cast<std::uint64_t>(alignment - 1u);
}

bool is_power_of_two(std::uint32_t value) { return value != 0u && (value & (value - 1u)) == 0u; }

}  // namespace

// ---------------------------------------------------------------------------
// RangeAllocator

RangeAllocator::RangeAllocator(std::uint32_t base, std::uint32_t size) : base_(base), size_(size) {
    if (size_ != 0u)
        free_ranges_.push_back(Range{base_, size_});
}

std::optional<std::uint32_t> RangeAllocator::allocate(std::uint32_t size, int type,
                                                      std::uint32_t alignment) {
    if (size == 0u || !is_power_of_two(alignment))
        return std::nullopt;

    const std::uint64_t effective = align_up_u64(size, alignment);
    if (effective > 0xFFFFFFFFull)
        return std::nullopt;
    const auto effective_size = static_cast<std::uint32_t>(effective);

    // Find the fit first; mutate only on success (Gate 4A invariant 6).
    std::size_t chosen = free_ranges_.size();
    std::uint32_t chosen_start = 0;
    if (type == kAllocTopDown) {
        for (std::size_t i = free_ranges_.size(); i-- > 0;) {
            const Range &range = free_ranges_[i];
            const std::uint64_t range_end = static_cast<std::uint64_t>(range.start) + range.size;
            if (range_end < effective)
                continue;
            const std::uint64_t start = align_down_u64(range_end - effective, alignment);
            if (start < range.start)
                continue;
            chosen = i;
            chosen_start = static_cast<std::uint32_t>(start);
            break;
        }
    } else {
        for (std::size_t i = 0; i < free_ranges_.size(); ++i) {
            const Range &range = free_ranges_[i];
            const std::uint64_t start = align_up_u64(range.start, alignment);
            const std::uint64_t range_end = static_cast<std::uint64_t>(range.start) + range.size;
            if (start + effective > range_end)
                continue;
            chosen = i;
            chosen_start = static_cast<std::uint32_t>(start);
            break;
        }
    }
    if (chosen == free_ranges_.size())
        return std::nullopt;

    const Range range = free_ranges_[chosen];
    const std::uint64_t range_end = static_cast<std::uint64_t>(range.start) + range.size;
    const std::uint64_t block_end = static_cast<std::uint64_t>(chosen_start) + effective_size;

    std::vector<Range> remainder;
    if (chosen_start > range.start)
        remainder.push_back(Range{range.start, chosen_start - range.start});
    if (block_end < range_end)
        remainder.push_back(
            Range{static_cast<std::uint32_t>(block_end), static_cast<std::uint32_t>(range_end - block_end)});

    free_ranges_.erase(free_ranges_.begin() + static_cast<std::ptrdiff_t>(chosen));
    free_ranges_.insert(free_ranges_.begin() + static_cast<std::ptrdiff_t>(chosen),
                        remainder.begin(), remainder.end());
    allocated_[chosen_start] = effective_size;
    return chosen_start;
}

std::int32_t RangeAllocator::free_block(std::uint32_t address) {
    const auto it = allocated_.find(address);
    if (it == allocated_.end())
        return static_cast<std::int32_t>(kErrorDoubleFree);

    Range freed{address, it->second};
    allocated_.erase(it);

    // Insert sorted, then coalesce with both neighbours (Gate 4A invariant 4).
    auto pos = std::lower_bound(free_ranges_.begin(), free_ranges_.end(), freed.start,
                                [](const Range &range, std::uint32_t start) { return range.start < start; });
    pos = free_ranges_.insert(pos, freed);
    if (pos != free_ranges_.begin()) {
        auto prev = pos - 1;
        if (static_cast<std::uint64_t>(prev->start) + prev->size == pos->start) {
            prev->size += pos->size;
            free_ranges_.erase(pos);
            pos = prev;
        }
    }
    auto next = pos + 1;
    if (next != free_ranges_.end() &&
        static_cast<std::uint64_t>(pos->start) + pos->size == next->start) {
        pos->size += next->size;
        free_ranges_.erase(next);
    }
    return 0;
}

bool RangeAllocator::is_allocated(std::uint32_t address) const {
    return allocated_.find(address) != allocated_.end();
}

std::optional<std::uint32_t> RangeAllocator::allocate_at(std::uint32_t address,
                                                         std::uint32_t size) {
    if (size == 0u)
        return std::nullopt;
    const std::uint64_t block_end = static_cast<std::uint64_t>(address) + size;
    if (block_end > 0xFFFFFFFFull)
        return std::nullopt;
    for (std::size_t i = 0; i < free_ranges_.size(); ++i) {
        const Range range = free_ranges_[i];
        const std::uint64_t range_end = static_cast<std::uint64_t>(range.start) + range.size;
        if (address < range.start || block_end > range_end)
            continue;
        std::vector<Range> remainder;
        if (address > range.start)
            remainder.push_back(Range{range.start, address - range.start});
        if (block_end < range_end)
            remainder.push_back(Range{static_cast<std::uint32_t>(block_end),
                                      static_cast<std::uint32_t>(range_end - block_end)});
        free_ranges_.erase(free_ranges_.begin() + static_cast<std::ptrdiff_t>(i));
        free_ranges_.insert(free_ranges_.begin() + static_cast<std::ptrdiff_t>(i),
                            remainder.begin(), remainder.end());
        allocated_[address] = size;
        return address;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Stack layout (Gate 4A section 1)

std::int32_t compute_stack_layout(std::uint32_t stack_bottom, std::uint32_t stack_size,
                                  std::uint32_t arglen, StackLayout &out) {
    if (arglen > 0xFFFFFFF0u)
        return static_cast<std::int32_t>(kErrorIllegalAddr);

    const std::uint32_t aligned_arglen = (arglen + 15u) & ~15u;
    if (stack_size < P3PKernel::kMinStackOverhead ||
        aligned_arglen > stack_size - P3PKernel::kMinStackOverhead)
        return static_cast<std::int32_t>(kErrorNoMemory);

    const std::uint32_t stack_top = stack_bottom + stack_size;
    out.tcb_address = stack_top - P3PKernel::kTcbSize;
    out.sp_arg = out.tcb_address - aligned_arglen;
    out.initial_sp = out.sp_arg - P3PKernel::kAbiReserve;
    out.aligned_arglen = aligned_arglen;
    return 0;
}

// ---------------------------------------------------------------------------
// P3PKernel

P3PKernel::P3PKernel() : allocator_(kUserPartitionBase, kUserPartitionSize) {}

const P3PThread *P3PKernel::find_thread(std::int32_t uid) const {
    const auto it = threads_.find(uid);
    return it == threads_.end() ? nullptr : &it->second;
}

std::int32_t P3PKernel::bootstrap_loader(const std::string &name, std::uint32_t entry,
                                         std::int32_t priority, std::uint32_t gp,
                                         std::uint32_t stack_size) {
    const auto stack = allocator_.allocate(stack_size, kAllocTopDown, kStackAlignment);
    if (!stack)
        return static_cast<std::int32_t>(kErrorNoMemory);

    P3PThread thread;
    thread.uid = next_uid_++;
    thread.name = name;
    thread.entry = entry;
    thread.priority = priority;
    thread.gp = gp;
    thread.stack_address = *stack;
    thread.stack_size = stack_size;
    thread.status = ThreadStatus::Running;
    thread.context.pc = entry;

    const std::int32_t uid = thread.uid;
    threads_[uid] = std::move(thread);
    current_uid_ = uid;
    return uid;
}

std::int32_t P3PKernel::create_thread(const std::string &name, std::uint32_t entry,
                                      std::int32_t priority, std::uint32_t stack_size,
                                      std::uint32_t attr, std::uint32_t gp,
                                      psprecomp::GuestMemory &memory) {
    if (stack_size == 0u)
        return static_cast<std::int32_t>(kErrorNoMemory);

    const std::uint64_t aligned_size = align_up_u64(stack_size, kStackAlignment);
    if (aligned_size > 0xFFFFFFFFull)
        return static_cast<std::int32_t>(kErrorNoMemory);
    const auto stack =
        allocator_.allocate(static_cast<std::uint32_t>(aligned_size), kAllocTopDown, kStackAlignment);
    if (!stack)
        return static_cast<std::int32_t>(kErrorNoMemory);

    P3PThread thread;
    thread.uid = next_uid_++;
    thread.name = name;
    thread.entry = entry;
    thread.priority = priority;
    thread.attr = attr;
    thread.gp = gp;
    thread.stack_address = *stack;
    thread.stack_size = static_cast<std::uint32_t>(aligned_size);
    thread.status = ThreadStatus::Dormant;
    thread.context.pc = entry;
    thread.context.set_gpr(28, gp);

    // Reference-kernel stack preparation (PSPThread::FillStack): fill the
    // stack with 0xFF and write the thread UID at the stack bottom.
    for (std::uint32_t i = 0; i < thread.stack_size; ++i)
        memory.store8(thread.stack_address + i, 0xFFu);
    memory.store32(thread.stack_address, static_cast<std::uint32_t>(thread.uid));

    const std::int32_t uid = thread.uid;
    threads_[uid] = std::move(thread);
    return uid;
}

std::int32_t P3PKernel::start_thread(std::int32_t thid, std::uint32_t arglen, std::uint32_t argp,
                                     psprecomp::GuestMemory &memory) {
    // Validation phase: nothing is mutated before every check has passed
    // (Gate 4A failure-atomicity matrix).
    if (thid <= 0)
        return static_cast<std::int32_t>(kErrorInvalidUid);
    const auto it = threads_.find(thid);
    if (it == threads_.end())
        return static_cast<std::int32_t>(kErrorUnknownUid);
    P3PThread &thread = it->second;
    if (thread.status != ThreadStatus::Dormant)
        return static_cast<std::int32_t>(kErrorNotDormant);
    if (arglen > 0xFFFFFFF0u)
        return static_cast<std::int32_t>(kErrorIllegalAddr);
    if (arglen > 0u && argp == 0u)
        return static_cast<std::int32_t>(kErrorIllegalAddr);
    if (arglen > 0u && !memory.contains(argp, arglen))
        return static_cast<std::int32_t>(kErrorIllegalAddr);

    StackLayout layout;
    if (const std::int32_t rc =
            compute_stack_layout(thread.stack_address, thread.stack_size, arglen, layout);
        rc != 0)
        return rc;

    // Phase 1 (ADR-005): stage the arguments in host memory. The source range
    // was bounds-checked above, so these loads cannot fail.
    std::vector<std::uint8_t> staged(arglen);
    for (std::uint32_t i = 0; i < arglen; ++i)
        staged[i] = memory.load8(argp + i);

    // Phase 2: commit. The destination is the thread's own allocated stack,
    // so these stores cannot fail either; the start is atomic in practice.
    for (std::uint32_t i = 0; i < arglen; ++i)
        memory.store8(layout.sp_arg + i, staged[i]);
    // TCB ($k0) area: zeroed, then the reference kernel's fields
    // (PSPThread::FillStack): UID at +0xC0, stack base at +0xC8,
    // 0xFFFFFFFF sentinels at +0xF8/+0xFC.
    for (std::uint32_t i = 0; i < kTcbSize; ++i)
        memory.store8(layout.tcb_address + i, 0u);
    memory.store32(layout.tcb_address + 0xC0u, static_cast<std::uint32_t>(thread.uid));
    memory.store32(layout.tcb_address + 0xC8u, thread.stack_address);
    memory.store32(layout.tcb_address + 0xF8u, 0xFFFFFFFFu);
    memory.store32(layout.tcb_address + 0xFCu, 0xFFFFFFFFu);

    // Registers the kernel does not set for a new thread are architecturally
    // undefined; the oracle environment observes them as the boot poison
    // 0xDEADBEEF (Gate 3 equalization), so the saved context carries that
    // value for every register not explicitly initialized below.
    psprecomp::AllegrexContext ctx{};
    for (int i = 1; i < 32; ++i)
        ctx.set_gpr(i, 0xDEADBEEFu);
    ctx.pc = thread.entry;
    ctx.set_gpr(2, 0u);                      // $v0: kernel return register, observed 0 at thread entry
    ctx.set_gpr(28, thread.gp);              // $gp
    ctx.set_gpr(29, layout.initial_sp);      // $sp
    ctx.set_gpr(30, layout.initial_sp);      // $fp follows $sp (reference kernel)
    ctx.set_gpr(31, kExitStubAddress);       // $ra -> native exit stub (ADR-002)
    ctx.set_gpr(26, layout.tcb_address);     // $k0 -> TCB (ARCHITECTURE section 4)
    ctx.set_gpr(4, arglen);                  // $a0
    ctx.set_gpr(5, arglen > 0u ? layout.sp_arg : 0u);  // $a1 (NULL for arglen 0)
    thread.context = ctx;
    thread.status = ThreadStatus::Ready;
    return 0;
}

bool P3PKernel::exit_current_and_schedule_next(psprecomp::AllegrexContext &live) {
    if (current_uid_ != 0) {
        if (P3PThread *current = [&]() -> P3PThread * {
                const auto it = threads_.find(current_uid_);
                return it == threads_.end() ? nullptr : &it->second;
            }()) {
            current->status = ThreadStatus::Dead;
            if (current->stack_address != 0u) {
                allocator_.free_block(current->stack_address);
                current->stack_address = 0u;
            }
            current->status = ThreadStatus::Deleted;
        }
        current_uid_ = 0;
    }

    P3PThread *best = nullptr;
    for (auto &[uid, thread] : threads_) {
        if (thread.status != ThreadStatus::Ready)
            continue;
        if (best == nullptr || thread.priority < best->priority ||
            (thread.priority == best->priority && uid < best->uid))
            best = &thread;
    }
    if (best == nullptr)
        return false;

    best->status = ThreadStatus::Running;
    current_uid_ = best->uid;
    live = best->context;
    return true;
}

// ---------------------------------------------------------------------------
// Gate 5B: memory blocks, callbacks, event flags, main module

std::int32_t P3PKernel::alloc_partition_memory(std::int32_t partition, const std::string &name,
                                               std::int32_t type, std::uint32_t size,
                                               std::uint32_t addr) {
    if (partition < 1 || partition > 6 || size == 0u || type < 0 || type > 4)
        return static_cast<std::int32_t>(kErrorIllegalArgument);

    std::optional<std::uint32_t> address;
    switch (type) {
        case 0:  // PSP_SMEM_Low
            address = allocator_.allocate(size, kAllocBottomUp, 256u);
            break;
        case 1:  // PSP_SMEM_High
            address = allocator_.allocate(size, kAllocTopDown, 256u);
            break;
        case 2:  // PSP_SMEM_Addr
            address = allocator_.allocate_at(addr, size);
            break;
        case 3:  // PSP_SMEM_LowAligned (addr = alignment)
        case 4:  // PSP_SMEM_HighAligned
            if (addr == 0u || (addr & (addr - 1u)) != 0u)
                return static_cast<std::int32_t>(kErrorIllegalArgument);
            address = allocator_.allocate(size, type == 3 ? kAllocBottomUp : kAllocTopDown, addr);
            break;
    }
    if (!address)
        return static_cast<std::int32_t>(kErrorNoMemory);

    const std::int32_t uid = next_uid_++;
    memory_blocks_[uid] = MemoryBlock{name, partition, *address, size};
    return uid;
}

std::int32_t P3PKernel::free_partition_memory(std::int32_t uid) {
    if (uid <= 0)
        return static_cast<std::int32_t>(kErrorInvalidUid);
    const auto it = memory_blocks_.find(uid);
    if (it == memory_blocks_.end())
        return static_cast<std::int32_t>(kErrorUnknownUid);
    const std::int32_t result = allocator_.free_block(it->second.address);
    if (result != 0)
        return result;
    memory_blocks_.erase(it);
    return 0;
}

std::uint32_t P3PKernel::block_head_address(std::int32_t uid) const {
    const auto it = memory_blocks_.find(uid);
    return it == memory_blocks_.end() ? 0u : it->second.address;
}

std::int32_t P3PKernel::create_callback(const std::string &name, std::uint32_t func,
                                        std::uint32_t arg) {
    const std::int32_t uid = next_uid_++;
    callbacks_[uid] = Callback{name, current_uid_, func, arg, 0u, 0u};
    return uid;
}

std::int32_t P3PKernel::delete_callback(std::int32_t uid) {
    if (uid <= 0)
        return static_cast<std::int32_t>(kErrorInvalidUid);
    const auto it = callbacks_.find(uid);
    if (it == callbacks_.end())
        return static_cast<std::int32_t>(kErrorUnknownUid);
    callbacks_.erase(it);
    return 0;
}

std::int32_t P3PKernel::notify_callback(std::int32_t uid, std::uint32_t arg) {
    const auto it = callbacks_.find(uid);
    if (it == callbacks_.end())
        return static_cast<std::int32_t>(kErrorUnknownUid);
    it->second.notify_count += 1u;
    it->second.notify_arg = arg;
    return 0;
}

std::int32_t P3PKernel::check_callbacks() const {
    std::int32_t signaled = 0;
    for (const auto &entry : callbacks_)
        if (entry.second.thread_uid == current_uid_ && entry.second.notify_count > 0u)
            ++signaled;
    return signaled;
}

std::int32_t P3PKernel::create_event_flag(const std::string &name, std::uint32_t attr,
                                          std::uint32_t init_pattern) {
    const std::int32_t uid = next_uid_++;
    event_flags_[uid] = EventFlag{name, attr, init_pattern};
    return uid;
}

std::int32_t P3PKernel::delete_event_flag(std::int32_t uid) {
    const auto it = event_flags_.find(uid);
    if (it == event_flags_.end())
        return static_cast<std::int32_t>(kErrorUnknownEvfId);
    event_flags_.erase(it);
    return 0;
}

std::int32_t P3PKernel::set_event_flag(std::int32_t uid, std::uint32_t bits) {
    const auto it = event_flags_.find(uid);
    if (it == event_flags_.end())
        return static_cast<std::int32_t>(kErrorUnknownEvfId);
    it->second.pattern |= bits;
    return 0;
}

std::int32_t P3PKernel::clear_event_flag(std::int32_t uid, std::uint32_t bits) {
    const auto it = event_flags_.find(uid);
    if (it == event_flags_.end())
        return static_cast<std::int32_t>(kErrorUnknownEvfId);
    it->second.pattern &= ~bits;
    return 0;
}

bool P3PKernel::event_flag_pattern(std::int32_t uid, std::uint32_t &out) const {
    const auto it = event_flags_.find(uid);
    if (it == event_flags_.end())
        return false;
    out = it->second.pattern;
    return true;
}

std::int32_t P3PKernel::change_current_thread_attr(std::uint32_t clear_mask,
                                                   std::uint32_t set_mask) {
    const auto it = threads_.find(current_uid_);
    if (it == threads_.end())
        return static_cast<std::int32_t>(kErrorUnknownUid);
    it->second.attr = (it->second.attr & ~clear_mask) | set_mask;
    return 0;
}

std::int32_t P3PKernel::register_main_module(std::uint32_t base, std::uint32_t size) {
    if (main_module_uid_ != 0)
        return main_module_uid_;
    main_module_uid_ = next_uid_++;
    main_module_base_ = base;
    main_module_size_ = size;
    return main_module_uid_;
}

std::int32_t P3PKernel::module_uid_at(std::uint32_t address) const {
    if (main_module_uid_ == 0)
        return 0;
    if (address >= main_module_base_ && address - main_module_base_ < main_module_size_)
        return main_module_uid_;
    return 0;
}

}  // namespace p3p

