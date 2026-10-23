// Unit tests for the P3P kernel (Gate 4B): RangeAllocator invariants, the
// Gate 4A stack-layout boundary table, thread lifecycle, the start_thread
// failure-atomicity matrix, and the scheduler hand-off.

#include "kernel/p3p_kernel.hpp"
#include "hle/p3p_hle_threadman.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char *expr, int line) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, expr);
    }
}

void check_eq(std::uint32_t actual, std::uint32_t expected, const char *expr, int line) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf("FAIL line %d: %s = 0x%08x, expected 0x%08x\n", line, expr, actual, expected);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)
#define CHECK_EQ(actual, expected) \
    check_eq(static_cast<std::uint32_t>(actual), static_cast<std::uint32_t>(expected), #actual, __LINE__)

std::int32_t as_err(std::uint32_t code) { return static_cast<std::int32_t>(code); }

void test_allocator() {
    using namespace p3p;
    RangeAllocator alloc(0x09000000u, 0x40000u);

    const auto a = alloc.allocate(0x1000u, kAllocBottomUp, 0x100u);
    CHECK(a && *a == 0x09000000u);
    const auto b = alloc.allocate(0x1000u, kAllocBottomUp, 0x100u);
    CHECK(b && *b == 0x09001000u);
    const auto top = alloc.allocate(0x4000u, kAllocTopDown, 0x100u);
    CHECK(top && *top == 0x0903C000u);
    CHECK_EQ(alloc.allocated_block_count(), 3u);
    CHECK(alloc.is_allocated(0x09001000u));
    CHECK(!alloc.is_allocated(0x09002000u));

    // Alignment: bottom-up request after a small allocation aligns up.
    RangeAllocator alloc2(0x09000000u, 0x40000u);
    const auto small = alloc2.allocate(0x10u, kAllocBottomUp, 0x10u);
    CHECK(small && *small == 0x09000000u);
    const auto aligned = alloc2.allocate(0x100u, kAllocBottomUp, 0x1000u);
    CHECK(aligned && *aligned == 0x09001000u);

    // Failed allocation leaves the free list untouched (invariant 6).
    const std::size_t free_before = alloc.free_range_count();
    CHECK(!alloc.allocate(0x50000u, kAllocBottomUp, 0x100u).has_value());
    CHECK_EQ(alloc.free_range_count(), free_before);

    // Freeing everything coalesces back to a single full range (invariant 4).
    CHECK_EQ(alloc.free_block(0x0903C000u), 0u);
    CHECK_EQ(alloc.free_block(0x09001000u), 0u);
    CHECK_EQ(alloc.free_block(0x09000000u), 0u);
    CHECK_EQ(alloc.free_range_count(), 1u);
    const auto whole = alloc.allocate(0x40000u, kAllocBottomUp, 0x100u);
    CHECK(whole && *whole == 0x09000000u);
    CHECK_EQ(alloc.free_block(0x09000000u), 0u);

    // Double free rejected (invariant 5).
    CHECK_EQ(alloc.free_block(0x09000000u), as_err(kErrorDoubleFree));
    CHECK_EQ(alloc.free_block(0x09012345u), as_err(kErrorDoubleFree));

    // Coalescing detail: a freed middle block stays separate while its
    // neighbours are allocated, then merges both sides as they are freed.
    RangeAllocator alloc3(0x09000000u, 0x40000u);
    CHECK(alloc3.allocate(0x1000u, kAllocBottomUp, 0x100u).value() == 0x09000000u);
    CHECK(alloc3.allocate(0x1000u, kAllocBottomUp, 0x100u).value() == 0x09001000u);
    CHECK(alloc3.allocate(0x1000u, kAllocBottomUp, 0x100u).value() == 0x09002000u);
    CHECK_EQ(alloc3.free_block(0x09001000u), 0u);
    CHECK_EQ(alloc3.free_range_count(), 2u);  // freed middle + tail
    CHECK_EQ(alloc3.free_block(0x09000000u), 0u);
    CHECK_EQ(alloc3.free_range_count(), 2u);  // merged pair + tail
    CHECK_EQ(alloc3.free_block(0x09002000u), 0u);
    CHECK_EQ(alloc3.free_range_count(), 1u);  // fully coalesced
}

void test_stack_layout() {
    using namespace p3p;
    StackLayout layout;
    // Gate 4A boundary table: bottom 0x09000000, size 0x40000.
    CHECK_EQ(compute_stack_layout(0x09000000u, 0x40000u, 0u, layout), 0u);
    CHECK_EQ(layout.initial_sp, 0x0903FEC0u);
    CHECK_EQ(layout.sp_arg, 0x0903FF00u);
    CHECK_EQ(layout.tcb_address, 0x0903FF00u);

    CHECK_EQ(compute_stack_layout(0x09000000u, 0x40000u, 27u, layout), 0u);
    CHECK_EQ(layout.initial_sp, 0x0903FEA0u);
    CHECK_EQ(layout.sp_arg, 0x0903FEE0u);
    CHECK_EQ(layout.aligned_arglen, 32u);

    CHECK_EQ(compute_stack_layout(0x09000000u, 0x40000u, 261824u, layout), 0u);
    CHECK_EQ(layout.initial_sp, 0x09000000u);
    CHECK_EQ(compute_stack_layout(0x09000000u, 0x40000u, 261809u, layout), 0u);
    CHECK_EQ(layout.initial_sp, 0x09000000u);

    CHECK_EQ(compute_stack_layout(0x09000000u, 0x40000u, 261825u, layout), as_err(kErrorNoMemory));
    CHECK_EQ(compute_stack_layout(0x09000000u, 0x40000u, 0xFFFFFFFFu, layout),
             as_err(kErrorIllegalAddr));
}

void test_kernel_lifecycle() {
    using namespace p3p;
    P3PKernel kernel;
    psprecomp::GuestMemory memory(32u * 1024u * 1024u);

    const std::int32_t loader = kernel.bootstrap_loader("loader", 0x08804108u, 0x20, 0x08C42A50u, 0x10000u);
    CHECK_EQ(loader, 100);
    CHECK_EQ(kernel.current_uid(), 100);

    const std::int32_t uid =
        kernel.create_thread("user_main", 0x0880421Cu, 0x20, 0x40000u, 0u, 0x08C42A50u, memory);
    CHECK_EQ(uid, 101);
    const P3PThread *thread = kernel.find_thread(uid);
    CHECK(thread != nullptr);
    CHECK(thread->status == ThreadStatus::Dormant);
    // Loader took the top 64 KiB; user_main's stack sits directly below it.
    CHECK_EQ(thread->stack_address, 0x09FB0000u);
    CHECK_EQ(thread->stack_size, 0x40000u);
    // Reference-kernel stack fill: 0xFF body, UID at the stack bottom.
    CHECK_EQ(memory.load8(0x09FB0000u + 0x1000u), 0xFFu);
    CHECK_EQ(memory.load32(0x09FB0000u), 101u);

    // Boot argument payload in a scratch area of guest RAM.
    const char *boot_arg = "/PSP_GAME/SYSDIR/EBOOT.BIN";
    const std::uint32_t argp = 0x09000000u;
    for (std::uint32_t i = 0; i < 27u; ++i)
        memory.store8(argp + i, static_cast<std::uint8_t>(boot_arg[i]));  // includes NUL

    CHECK_EQ(kernel.start_thread(uid, 27u, argp, memory), 0);
    thread = kernel.find_thread(uid);
    CHECK(thread->status == ThreadStatus::Ready);
    CHECK_EQ(thread->context.pc, 0x0880421Cu);
    CHECK_EQ(thread->context.gpr[29], 0x09FEFEA0u);   // initial $sp
    CHECK_EQ(thread->context.gpr[30], 0x09FEFEA0u);   // $fp follows $sp
    CHECK_EQ(thread->context.gpr[4], 27u);           // $a0 = arglen
    CHECK_EQ(thread->context.gpr[5], 0x09FEFEE0u);   // $a1 = sp_arg
    CHECK_EQ(thread->context.gpr[31], 0x08800000u);  // $ra = exit stub
    CHECK_EQ(thread->context.gpr[26], 0x09FEFF00u);  // $k0 = TCB
    CHECK_EQ(thread->context.gpr[28], 0x08C42A50u);  // $gp
    // Registers the kernel does not set carry the oracle boot poison.
    CHECK_EQ(thread->context.gpr[6], 0xDEADBEEFu);   // $a2
    CHECK_EQ(thread->context.gpr[7], 0xDEADBEEFu);   // $a3
    CHECK_EQ(thread->context.gpr[3], 0xDEADBEEFu);   // $v1
    CHECK_EQ(thread->context.gpr[2], 0u);            // $v0 = 0 at thread entry
    for (std::uint32_t i = 0; i < 27u; ++i)
        CHECK_EQ(memory.load8(0x09FEFEE0u + i), static_cast<std::uint8_t>(boot_arg[i]));
    // TCB fields per the reference kernel (PSPThread::FillStack).
    CHECK_EQ(memory.load32(0x09FEFF00u + 0xC0u), 101u);
    CHECK_EQ(memory.load32(0x09FEFF00u + 0xC8u), 0x09FB0000u);
    CHECK_EQ(memory.load32(0x09FEFF00u + 0xF8u), 0xFFFFFFFFu);
    CHECK_EQ(memory.load32(0x09FEFF00u + 0xFCu), 0xFFFFFFFFu);

    // Starting a non-Dormant thread is rejected.
    CHECK_EQ(kernel.start_thread(uid, 0u, 0u, memory), as_err(kErrorNotDormant));

    // Scheduler hand-off: loader exits, user_main becomes live.
    psprecomp::AllegrexContext live{};
    CHECK(kernel.exit_current_and_schedule_next(live));
    CHECK_EQ(live.pc, 0x0880421Cu);
    CHECK_EQ(live.gpr[29], 0x09FEFEA0u);
    CHECK_EQ(kernel.current_uid(), 101);
    CHECK(kernel.find_thread(100)->status == ThreadStatus::Deleted);
    CHECK(kernel.find_thread(101)->status == ThreadStatus::Running);

    // No Ready thread remains.
    CHECK(!kernel.exit_current_and_schedule_next(live));
}

void test_start_failure_matrix() {
    using namespace p3p;
    P3PKernel kernel;
    psprecomp::GuestMemory memory(32u * 1024u * 1024u);
    kernel.bootstrap_loader("loader", 0x08804108u, 0x20, 0x08C42A50u, 0x10000u);
    const std::int32_t uid =
        kernel.create_thread("worker", 0x08805000u, 0x30, 0x40000u, 0u, 0x08C42A50u, memory);
    CHECK_EQ(uid, 101);

    const std::uint32_t sentinel_addr = 0x09000000u;
    memory.store32(sentinel_addr, 0xCAFEBABEu);

    CHECK_EQ(kernel.start_thread(0, 0u, 0u, memory), as_err(kErrorInvalidUid));
    CHECK_EQ(kernel.start_thread(-3, 0u, 0u, memory), as_err(kErrorInvalidUid));
    CHECK_EQ(kernel.start_thread(999, 0u, 0u, memory), as_err(kErrorUnknownUid));
    CHECK_EQ(kernel.start_thread(uid, 0xFFFFFFFFu, 0u, memory), as_err(kErrorIllegalAddr));
    CHECK_EQ(kernel.start_thread(uid, 16u, 0u, memory), as_err(kErrorIllegalAddr));
    CHECK_EQ(kernel.start_thread(uid, 16u, 0x0A000000u, memory), as_err(kErrorIllegalAddr));
    CHECK_EQ(kernel.start_thread(uid, 261825u, sentinel_addr, memory), as_err(kErrorNoMemory));

    // Atomicity: thread untouched, guest memory untouched, nothing Ready.
    const P3PThread *thread = kernel.find_thread(uid);
    CHECK(thread->status == ThreadStatus::Dormant);
    CHECK_EQ(memory.load32(sentinel_addr), 0xCAFEBABEu);
    psprecomp::AllegrexContext live{};
    live.pc = 0xDEADC0DEu;
    // exit_current would schedule a Ready thread if one existed; the loader
    // exit leaves nothing Ready, so live must stay untouched.
    CHECK(!kernel.exit_current_and_schedule_next(live));
    CHECK_EQ(live.pc, 0xDEADC0DEu);
}

void test_hle_cores() {
    using namespace p3p;
    P3PKernel kernel;
    psprecomp::Runtime runtime(32u * 1024u * 1024u);
    kernel.bootstrap_loader("loader", 0x08804108u, 0x20, 0x08C42A50u, 0x10000u);

    // Name string + EABI $t0 attr for create.
    const std::uint32_t name_addr = 0x09001000u;
    const char *name = "user_main";
    for (std::uint32_t i = 0; i <= std::strlen(name); ++i)
        runtime.memory().store8(name_addr + i, static_cast<std::uint8_t>(name[i]));
    const std::uint32_t fake_sp = 0x09002000u;

    psprecomp::AllegrexContext ctx{};
    ctx.set_gpr(4, name_addr);
    ctx.set_gpr(5, 0x0880421Cu);
    ctx.set_gpr(6, 0x20u);
    ctx.set_gpr(7, 0x40000u);
    ctx.set_gpr(8, 0x1234u);  // attr (EABI $t0)
    ctx.set_gpr(28, 0x08C42A50u);
    ctx.set_gpr(29, fake_sp);
    const std::int32_t uid = hle_create_thread(runtime, ctx, kernel);
    CHECK_EQ(uid, 101);
    CHECK_EQ(ctx.gpr[2], 101u);
    const P3PThread *thread = kernel.find_thread(101);
    CHECK(thread != nullptr);
    CHECK(thread->name == "user_main");
    CHECK_EQ(thread->attr, 0x1234u);
    CHECK_EQ(thread->priority, 0x20);

    // Start with arglen 0: $a0 = 0, $a1 = NULL (ARCHITECTURE section 4).
    psprecomp::AllegrexContext start_ctx{};
    start_ctx.set_gpr(4, 101u);
    start_ctx.set_gpr(5, 0u);
    start_ctx.set_gpr(6, 0u);
    CHECK_EQ(hle_start_thread(runtime, start_ctx, kernel), 0);
    CHECK_EQ(start_ctx.gpr[2], 0u);
    thread = kernel.find_thread(101);
    CHECK(thread->status == ThreadStatus::Ready);
    CHECK_EQ(thread->context.gpr[4], 0u);
    CHECK_EQ(thread->context.gpr[5], 0u);
    CHECK_EQ(thread->context.gpr[29], thread->stack_address + thread->stack_size - 320u);

    // Create with an unreadable name pointer fails cleanly.
    psprecomp::AllegrexContext bad_ctx{};
    bad_ctx.set_gpr(4, 0u);
    bad_ctx.set_gpr(5, 0x0880421Cu);
    bad_ctx.set_gpr(6, 0x20u);
    bad_ctx.set_gpr(7, 0x40000u);
    CHECK_EQ(hle_create_thread(runtime, bad_ctx, kernel), as_err(kErrorIllegalAddr));
}

}  // namespace

int main() {
    test_allocator();
    test_stack_layout();
    test_kernel_lifecycle();
    test_start_failure_matrix();
    test_hle_cores();
    std::printf("test_p3p_threading: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
