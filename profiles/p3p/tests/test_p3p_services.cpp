// Unit tests for the P3P Gate 5B early services: SysMem partition blocks,
// ThreadMan callbacks/event flags/attributes, Ge/Display state, and the
// small platform services (ModuleMgr query, Kernel_Library, sceUtility).

#include "hle/p3p_hle_gedisplay.hpp"
#include "hle/p3p_hle_services.hpp"
#include "hle/p3p_hle_sysmem.hpp"
#include "hle/p3p_hle_umd.hpp"
#include "hle/p3p_hle_threadman.hpp"
#include "kernel/p3p_kernel.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

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
constexpr std::uint32_t kBufAddr = 0x09100000u;
constexpr std::uint32_t kOutAddr = 0x09200000u;

psprecomp::Runtime *g_runtime = nullptr;
p3p::P3PKernel *g_kernel = nullptr;

void put_str(std::uint32_t addr, const std::string &s) {
    for (std::size_t i = 0; i <= s.size(); ++i)
        g_runtime->memory().store8(addr + static_cast<std::uint32_t>(i),
                                   static_cast<std::uint8_t>(s.c_str()[i]));
}

std::uint32_t v0() { return g_runtime->cpu().gpr[2]; }

}  // namespace

int main() {
    psprecomp::Runtime runtime(32u * 1024u * 1024u);
    g_runtime = &runtime;
    p3p::P3PKernel kernel;
    g_kernel = &kernel;
    auto &ctx = runtime.cpu();
    ctx.gpr[29] = 0x09FFF000u;  // $sp for stack args

    const std::int32_t module_uid =
        kernel.register_main_module(0x08804000u, p3p::P3PKernel::kRamEnd - 0x08804000u);
    check(module_uid > 0, "main module uid allocated");
    check(kernel.register_main_module(0x08804000u, 16u) == module_uid,
          "register_main_module is idempotent");
    const std::int32_t loader_uid = kernel.bootstrap_loader("loader", 0x08804108u, 0x20, 0u, 0x10000u);
    check(loader_uid > 0 && loader_uid != module_uid, "loader thread uid distinct");

    // --- SysMem partition blocks -------------------------------------------
    put_str(kStrAddr, "testblock");
    ctx.gpr[4] = 2;         // user partition
    ctx.gpr[5] = kStrAddr;  // name
    ctx.gpr[6] = 0;         // PSP_SMEM_Low
    ctx.gpr[7] = 0x10000;   // size
    ctx.gpr[8] = 0u;  // addr (unused for Low)
    check(p3p::hle_sysmem_dispatch(runtime, ctx, kernel, p3p::kNidAllocPartitionMemory),
          "alloc dispatch handled");
    const std::int32_t block_uid = static_cast<std::int32_t>(v0());
    check(block_uid > 0, "alloc returns uid");
    ctx.gpr[4] = static_cast<std::uint32_t>(block_uid);
    check(p3p::hle_sysmem_dispatch(runtime, ctx, kernel, p3p::kNidGetBlockHeadAddr),
          "getblockhead dispatch handled");
    const std::uint32_t head = v0();
    check(head >= p3p::P3PKernel::kUserPartitionBase && head < p3p::P3PKernel::kRamEnd,
          "block head inside user partition");
    check((head & 0xFFu) == 0u, "block head 256-aligned");
    // Second allocation must not overlap.
    put_str(kStrAddr, "second");
    ctx.gpr[4] = 2;
    ctx.gpr[5] = kStrAddr;
    ctx.gpr[6] = 1;  // PSP_SMEM_High (top-down: thread stacks live there too)
    ctx.gpr[7] = 0x8000;
    p3p::hle_sysmem_dispatch(runtime, ctx, kernel, p3p::kNidAllocPartitionMemory);
    const std::int32_t block2_uid = static_cast<std::int32_t>(v0());
    check(block2_uid > 0 && block2_uid != block_uid, "second alloc uid distinct");
    check(kernel.block_head_address(block2_uid) != head, "second block elsewhere");
    // Free + error paths.
    ctx.gpr[4] = static_cast<std::uint32_t>(block_uid);
    p3p::hle_sysmem_dispatch(runtime, ctx, kernel, p3p::kNidFreePartitionMemory);
    check(v0() == 0u, "free block ok");
    check(kernel.block_head_address(block_uid) == 0u, "freed block head is 0");
    p3p::hle_sysmem_dispatch(runtime, ctx, kernel, p3p::kNidFreePartitionMemory);
    check(v0() == p3p::kErrorUnknownUid, "double free by uid is UNKNOWN_UID");
    // Oversized allocation fails with NO_MEMORY.
    ctx.gpr[4] = 2;
    ctx.gpr[5] = kStrAddr;
    ctx.gpr[6] = 0;
    ctx.gpr[7] = p3p::P3PKernel::kUserPartitionSize + 0x1000u;
    p3p::hle_sysmem_dispatch(runtime, ctx, kernel, p3p::kNidAllocPartitionMemory);
    check(v0() == p3p::kErrorNoMemory, "oversized alloc is NO_MEMORY");
    // Specific-address allocation (PSP_SMEM_Addr).
    const std::uint32_t want = p3p::P3PKernel::kUserPartitionBase + 0x200000u;
    ctx.gpr[4] = 2;
    ctx.gpr[5] = kStrAddr;
    ctx.gpr[6] = 2;
    ctx.gpr[7] = 0x4000;
    ctx.gpr[8] = want;
    p3p::hle_sysmem_dispatch(runtime, ctx, kernel, p3p::kNidAllocPartitionMemory);
    const std::int32_t block3_uid = static_cast<std::int32_t>(v0());
    check(block3_uid > 0 && kernel.block_head_address(block3_uid) == want,
          "addr allocation lands exactly");
    // sceKernelPrintf is a handled no-op returning 0.
    put_str(kStrAddr, "hello %d");
    ctx.gpr[4] = kStrAddr;
    check(p3p::hle_sysmem_dispatch(runtime, ctx, kernel, p3p::kNidKernelPrintf) && v0() == 0u,
          "kernel printf returns 0");

    // --- ThreadMan extras: callbacks, event flags, attr ---------------------
    put_str(kStrAddr, "exitcb");
    ctx.gpr[4] = kStrAddr;
    ctx.gpr[5] = 0x08805000u;  // func
    ctx.gpr[6] = 0;            // arg
    check(p3p::hle_threadman_extra_dispatch(runtime, ctx, kernel, p3p::kNidCreateCallback),
          "create callback handled");
    const std::int32_t cb_uid = static_cast<std::int32_t>(v0());
    check(cb_uid > 0, "callback uid valid");
    ctx.gpr[4] = 0;
    check(p3p::hle_threadman_extra_dispatch(runtime, ctx, kernel, p3p::kNidCheckCallback) &&
              v0() == 0u,
          "check callback: none signaled");
    kernel.notify_callback(cb_uid, 7u);
    check(p3p::hle_threadman_extra_dispatch(runtime, ctx, kernel, p3p::kNidCheckCallback) &&
              v0() == 1u,
          "check callback: one signaled after notify");
    ctx.gpr[4] = static_cast<std::uint32_t>(cb_uid);
    check(p3p::hle_threadman_extra_dispatch(runtime, ctx, kernel, p3p::kNidDeleteCallback) &&
              v0() == 0u,
          "delete callback ok");

    put_str(kStrAddr, "geflag");
    ctx.gpr[4] = kStrAddr;
    ctx.gpr[5] = 0;      // attr
    ctx.gpr[6] = 0x10u;  // init pattern
    check(p3p::hle_threadman_extra_dispatch(runtime, ctx, kernel, p3p::kNidCreateEventFlag),
          "create event flag handled");
    const std::int32_t evf_uid = static_cast<std::int32_t>(v0());
    check(evf_uid > 0 && evf_uid != cb_uid, "event flag uid valid + distinct");
    ctx.gpr[4] = static_cast<std::uint32_t>(evf_uid);
    ctx.gpr[5] = 0x01u;
    check(p3p::hle_threadman_extra_dispatch(runtime, ctx, kernel, p3p::kNidSetEventFlag) &&
              v0() == 0u,
          "set event flag ok");
    // Wait AND on bit0: satisfied (pattern 0x11), result pattern written.
    ctx.gpr[4] = static_cast<std::uint32_t>(evf_uid);
    ctx.gpr[5] = 0x01u;
    ctx.gpr[6] = 0u;  // WAITAND, no clear
    ctx.gpr[7] = kOutAddr;
    check(p3p::hle_threadman_extra_dispatch(runtime, ctx, kernel, p3p::kNidWaitEventFlag) &&
              v0() == 0u,
          "wait event flag satisfied returns 0");
    check(runtime.memory().load32(kOutAddr) == 0x11u, "wait writes result pattern");
    // Wait with WAITCLEARPATTERN clears the waited bits.
    ctx.gpr[4] = static_cast<std::uint32_t>(evf_uid);
    ctx.gpr[5] = 0x10u;
    ctx.gpr[6] = 0x20u;
    ctx.gpr[7] = kOutAddr;
    p3p::hle_threadman_extra_dispatch(runtime, ctx, kernel, p3p::kNidWaitEventFlag);
    std::uint32_t pattern = 0;
    check(kernel.event_flag_pattern(evf_uid, pattern) && pattern == 0x01u,
          "wait clearpattern cleared waited bits");
    ctx.gpr[4] = static_cast<std::uint32_t>(evf_uid);
    ctx.gpr[5] = 0x01u;
    check(p3p::hle_threadman_extra_dispatch(runtime, ctx, kernel, p3p::kNidClearEventFlag) &&
              v0() == 0u,
          "clear event flag ok");
    ctx.gpr[4] = 9999;
    check(p3p::hle_threadman_extra_dispatch(runtime, ctx, kernel, p3p::kNidSetEventFlag) &&
              v0() == p3p::kErrorUnknownEvfId,
          "set on unknown evf is UNKNOWN_EVFID");
    ctx.gpr[4] = static_cast<std::uint32_t>(evf_uid);
    check(p3p::hle_threadman_extra_dispatch(runtime, ctx, kernel, p3p::kNidDeleteEventFlag) &&
              v0() == 0u,
          "delete event flag ok");
    // Change current thread attr: set 0x40, verify on the loader record.
    ctx.gpr[4] = 0u;
    ctx.gpr[5] = 0x40u;
    check(p3p::hle_threadman_extra_dispatch(runtime, ctx, kernel,
                                            p3p::kNidChangeCurrentThreadAttr) &&
              v0() == 0u,
          "change current thread attr ok");
    const p3p::P3PThread *loader = kernel.find_thread(loader_uid);
    check(loader != nullptr && (loader->attr & 0x40u) != 0u, "attr bit landed on thread");
    // Fake clock advances.
    check(p3p::hle_threadman_extra_dispatch(runtime, ctx, kernel, p3p::kNidGetSystemTimeLow),
          "get system time handled");
    const std::uint32_t t1 = v0();
    p3p::hle_threadman_extra_dispatch(runtime, ctx, kernel, p3p::kNidGetSystemTimeLow);
    check(v0() > t1, "fake clock advances between reads");

    // --- Ge / Display --------------------------------------------------------
    p3p::gedisplay_reset_state();
    ctx.gpr[4] = 0;
    check(p3p::hle_gedisplay_dispatch(runtime, ctx, nullptr, "sceGe_user", 0xE47E40E4u) &&
              v0() == 0x04000000u,
          "ge edram addr is 0x04000000");
    check(p3p::hle_gedisplay_dispatch(runtime, ctx, nullptr, "sceGe_user", 0x1F6752ADu) &&
              v0() == 0x00200000u,
          "ge edram size is 2MiB");
    check(p3p::hle_gedisplay_dispatch(runtime, ctx, nullptr, "sceGe_user", 0xAB49E76Au),
          "ge list enqueue handled");
    const std::uint32_t list1 = v0();
    p3p::hle_gedisplay_dispatch(runtime, ctx, nullptr, "sceGe_user", 0xAB49E76Au);
    check(v0() == list1 + 1u, "ge list ids increment");
    ctx.gpr[4] = 0x04044000u;  // topaddr
    ctx.gpr[5] = 512u;         // bufferwidth
    ctx.gpr[6] = 1u;           // pixelformat 5551
    check(p3p::hle_gedisplay_dispatch(runtime, ctx, nullptr, "sceDisplay", 0x289D82FEu) && v0() == 0u,
          "display set framebuf ok");
    ctx.gpr[4] = kOutAddr;
    ctx.gpr[5] = kOutAddr + 8u;
    ctx.gpr[6] = kOutAddr + 16u;
    check(p3p::hle_gedisplay_dispatch(runtime, ctx, nullptr, "sceDisplay", 0xEEDA2E54u) &&
              v0() == 0x04044000u,
          "display get framebuf returns topaddr");
    check(runtime.memory().load32(kOutAddr) == 0x04044000u &&
              runtime.memory().load32(kOutAddr + 8u) == 512u &&
              runtime.memory().load32(kOutAddr + 16u) == 1u,
          "display get framebuf writes recorded values");
    check(p3p::hle_gedisplay_dispatch(runtime, ctx, nullptr, "sceDisplay", 0x984C27E7u),
          "wait vblank handled");
    check(p3p::hle_gedisplay_dispatch(runtime, ctx, nullptr, "sceDisplay", 0x9C6EAAD7u) && v0() == 1u,
          "vcount advanced after vblank wait");

    // --- Services -------------------------------------------------------------
    ctx.gpr[4] = 0x08850000u;  // inside the main module image
    check(p3p::hle_services_dispatch(runtime, ctx, g_kernel, "ModuleMgrForUser", 0xD8B73127u) &&
              static_cast<std::int32_t>(v0()) == module_uid,
          "get module id by address finds main module");
    ctx.gpr[4] = 0x04000000u;  // VRAM: not module memory
    check(p3p::hle_services_dispatch(runtime, ctx, g_kernel, "ModuleMgrForUser", 0xD8B73127u) &&
              v0() == p3p::kErrorNotFoundUidFunc,
          "get module id outside image is NOT_FOUND_UIDFUNC");
    // Kernel_Library memcpy / memset.
    put_str(kBufAddr, "COPYME!!");
    ctx.gpr[4] = kBufAddr + 0x100u;
    ctx.gpr[5] = kBufAddr;
    ctx.gpr[6] = 8u;
    check(p3p::hle_services_dispatch(runtime, ctx, g_kernel, "Kernel_Library", 0x1839852Au) &&
              v0() == kBufAddr + 0x100u,
          "kernel memcpy returns dst");
    bool copied = true;
    for (std::uint32_t i = 0; i < 8u; ++i)
        copied = copied && runtime.memory().load8(kBufAddr + 0x100u + i) ==
                               runtime.memory().load8(kBufAddr + i);
    check(copied, "kernel memcpy copied bytes");
    ctx.gpr[4] = kBufAddr + 0x200u;
    ctx.gpr[5] = 0xABu;
    ctx.gpr[6] = 16u;
    check(p3p::hle_services_dispatch(runtime, ctx, g_kernel, "Kernel_Library", 0xA089ECA4u),
          "kernel memset handled");
    check(runtime.memory().load8(kBufAddr + 0x200u) == 0xAB &&
              runtime.memory().load8(kBufAddr + 0x20Fu) == 0xAB,
          "kernel memset filled range");
    // Interrupt suspend/resume state machine.
    ctx.gpr[4] = 0;
    check(p3p::hle_services_dispatch(runtime, ctx, g_kernel, "Kernel_Library", 0x092968F4u) &&
              v0() == 1u,
          "suspend intr returns previous state (enabled)");
    check(p3p::hle_services_dispatch(runtime, ctx, g_kernel, "Kernel_Library", 0x092968F4u) &&
              v0() == 0u,
          "second suspend returns disabled");
    ctx.gpr[4] = 1u;
    check(p3p::hle_services_dispatch(runtime, ctx, g_kernel, "Kernel_Library", 0x5F10D406u) &&
              v0() == 0u,
          "resume intr returns state before restore");
    // GetThreadId returns the loader (current) thread.
    check(p3p::hle_services_dispatch(runtime, ctx, g_kernel, "Kernel_Library", 0x293B45B8u) &&
              static_cast<std::int32_t>(v0()) == loader_uid,
          "get thread id returns current thread");
    // Utility dialog state machine.
    p3p::services_reset_state();
    ctx.gpr[4] = 0;
    check(p3p::hle_services_dispatch(runtime, ctx, g_kernel, "sceUtility", 0x50C4CD57u) &&
              v0() == 0u,
          "savedata init start ok");
    check(p3p::hle_services_dispatch(runtime, ctx, g_kernel, "sceUtility", 0x8874DBE0u) &&
              v0() == 1u,
          "savedata status is INIT_STARTED");
    p3p::hle_services_dispatch(runtime, ctx, g_kernel, "sceUtility", 0xD4B95FFBu);
    check(p3p::hle_services_dispatch(runtime, ctx, g_kernel, "sceUtility", 0x8874DBE0u) &&
              v0() == 2u,
          "savedata status RUNNING after update");
    p3p::hle_services_dispatch(runtime, ctx, g_kernel, "sceUtility", 0xD4B95FFBu);
    check(p3p::hle_services_dispatch(runtime, ctx, g_kernel, "sceUtility", 0x8874DBE0u) &&
              v0() == 3u,
          "savedata status FINISHED after second update");

    // --- UMD: the disc is in, ready, readable (Gate 5C follow-up) ---
    {
        p3p::umd_reset_state();
        psprecomp::AllegrexContext uctx{};
        check(p3p::hle_umd_dispatch(runtime, uctx, 0x46EBB729u) && uctx.gpr[2] == 1u,
              "umd check medium reports the disc");
        check(p3p::hle_umd_dispatch(runtime, uctx, 0x6B4A146Cu) && uctx.gpr[2] == 0x32u,
              "umd drive stat is present|ready|readable");
        uctx.gpr[4] = 0x10u;  // wait for READY
        check(p3p::hle_umd_dispatch(runtime, uctx, 0x8EF08FCEu) && uctx.gpr[2] == 0u,
              "umd wait drive stat completes");
        uctx.gpr[4] = 0u;  // invalid mask
        check(p3p::hle_umd_dispatch(runtime, uctx, 0x8EF08FCEu) &&
                  static_cast<std::int32_t>(uctx.gpr[2]) < 0,
              "umd wait drive stat rejects an empty mask");
        check(p3p::hle_umd_dispatch(runtime, uctx, 0x20628E6Fu) && uctx.gpr[2] == 0u,
              "umd error stat is clean");
        // Activate against a real "disc0:" name in guest memory.
        const std::uint32_t name_addr = 0x09000200u;
        const char *disc = "disc0:";
        for (std::uint32_t i = 0; i <= 6u; ++i)
            runtime.memory().store8(name_addr + i, static_cast<std::uint8_t>(disc[i]));
        uctx.gpr[4] = 1u;
        uctx.gpr[5] = name_addr;
        check(p3p::hle_umd_dispatch(runtime, uctx, 0xC6183D47u) && uctx.gpr[2] == 0u,
              "umd activate accepts disc0:");
        uctx.gpr[4] = 3u;  // bad mode
        check(p3p::hle_umd_dispatch(runtime, uctx, 0xC6183D47u) &&
                  static_cast<std::int32_t>(uctx.gpr[2]) < 0,
              "umd activate rejects a bad mode");
        check(!p3p::hle_umd_dispatch(runtime, uctx, 0xDEADBEEFu),
              "unknown umd nid unhandled");
    }

    std::fprintf(stderr, "test_p3p_services: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
