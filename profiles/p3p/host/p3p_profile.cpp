#include "p3p_profile.hpp"

#include "hle/p3p_hle_io.hpp"
#include "hle/p3p_hle_gedisplay.hpp"
#include "hle/p3p_hle_services.hpp"
#include "hle/p3p_hle_sysmem.hpp"
#include "hle/p3p_hle_audio.hpp"
#include "hle/p3p_hle_umd.hpp"
#include "hle/p3p_hle_threadman.hpp"
#include "kernel/p3p_kernel.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/interpreter.hpp"

#include <unordered_map>

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace p3p {

namespace {

// Runtime::register_function takes plain function pointers (no captures), so
// the profile keeps its wiring state in file-scope variables. The profile is
// a single-instance host by design; init_profile (re)binds them.
P3PKernel *g_kernel = nullptr;
std::map<std::uint32_t, std::pair<std::string, std::uint32_t>> g_import_by_stub;

bool stub_seen(const std::string &library, std::uint32_t nid) {
    static std::set<std::pair<std::string, std::uint32_t>> seen;
    return !seen.emplace(library, nid).second;
}

// Shared import behaviour for both dispatch forms: performs the IoFileMgr
// implementation (Gate 5) and the ThreadMan implementation when a kernel is
// attached, otherwise acts as a logged return-0 stub. Writes the result to
// $v0; does not touch $pc.
void dispatch_import(P3PKernel *kernel, const std::string &library, std::uint32_t nid,
                     psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    if (library == kIoFileMgrLibrary && hle_io_dispatch(runtime, ctx, nid))
        return;
    // Gate 5B early services. Kernel-backed units need the kernel; the
    // Ge/Display and services units also answer without one (their
    // kernel-dependent queries degrade to not-found results).
    if (kernel != nullptr && library == kThreadManLibrary &&
        hle_threadman_extra_dispatch(runtime, ctx, *kernel, nid))
        return;
    if (kernel != nullptr && library == kSysMemLibrary &&
        hle_sysmem_dispatch(runtime, ctx, *kernel, nid))
        return;
    if ((library == kGeLibrary || library == kDisplayLibrary) &&
        hle_gedisplay_dispatch(runtime, ctx, kernel, library, nid))
        return;
    if (library == "sceUmdUser" && hle_umd_dispatch(runtime, ctx, nid))
        return;
    if (kernel != nullptr && library == "sceAudio" &&
        hle_audio_dispatch(runtime, ctx, *kernel, nid))
        return;
    if (hle_services_dispatch(runtime, ctx, kernel, library, nid))
        return;
    if (kernel != nullptr && library == kThreadManLibrary && nid == kNidCreateThread) {
        hle_create_thread(runtime, ctx, *kernel);
        return;
    }
    if (kernel != nullptr && library == kThreadManLibrary && nid == kNidStartThread) {
        hle_start_thread(runtime, ctx, *kernel);
        return;
    }
    if (!stub_seen(library, nid))
        std::fprintf(stderr, "[p3p] stub import %s::%s (returns 0)\n", library.c_str(),
                     psprecomp::hex32(nid).c_str());
    ctx.set_gpr(2, 0u);  // $v0 = 0
}

// Interpreter-mode import dispatch: the interpreter reaches an import
// through the function registered at its stub address, so ctx.pc identifies
// the import. Performs the import, then returns to $ra.
void import_stub_function(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const auto it = g_import_by_stub.find(ctx.pc);
    if (it != g_import_by_stub.end()) {
        dispatch_import(g_kernel, it->second.first, it->second.second, runtime, ctx);
    } else {
        if (!stub_seen("<unknown-stub>", ctx.pc))
            std::fprintf(stderr, "[p3p] stub import at unknown address %s (returns 0)\n",
                         psprecomp::hex32(ctx.pc).c_str());
        ctx.set_gpr(2, 0u);
    }
    if (!runtime.stopped())
        ctx.pc = ctx.gpr[31];  // return to $ra
}

void thread_exit_function(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    if (g_kernel == nullptr) {
        runtime.stop("p3p: thread returned to exit stub (no kernel attached)");
        return;
    }
    if (!g_kernel->exit_current_and_schedule_next(ctx))
        runtime.stop("p3p: last thread exited; no ready thread remains");
}

}  // namespace

namespace {

// Unsupported-instruction fallback (Gate 6 WP2 follow-up): a handful
// of corpus instructions were never lowered by the code generator
// (VFPU pairs 0x66818102/0x66808003, VFPU4-group words, a break
// encoding) and sit inside otherwise-AOT functions. The generated code
// traps them via rt.unsupported(); with this hook installed the
// runtime resumes dispatch at the trap pc, finds no registered block
// there, and the interpreter executes from that point until control
// reaches registered code again -- per-block granularity, everything
// else stays AOT. The per-pc cap keeps a pathological case loud
// instead of an infinite trap loop.
std::unordered_map<std::uint32_t, std::uint32_t> g_trap_counts;
std::unordered_map<std::uint32_t, std::uint32_t> g_bridge_skips;

// Exact-PC stand-in for a trap address: interpret guest code from
// here until the interpreter hands control back to registered code
// (its Dispatch exit), its budget cycles, or the run stops. One call
// is enough -- the outer dispatcher re-enters the fallback itself for
// any still-unregistered pc.
void p3p_interpret_bridge(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    // TEMP PROBE (Gate 6): neither the code generator nor the
    // interpreter implements this instruction (the trap that landed
    // here). Skip it -- its result register keeps a stale value -- and
    // interpret onward, purely to map how far the boot advances and
    // which trap sites are actually load-bearing. NOT a semantic fix;
    // the sites this probe proves reachable get real implementations.
    const std::uint32_t count = ++g_bridge_skips[ctx.pc];
    if (count == 1u)
        std::fprintf(stderr, "[p3p] probe: skipping unimplemented instruction at %s\n",
                     psprecomp::hex32(ctx.pc).c_str());
    ctx.pc += 4u;
    psprecomp::interpret_allegrex(rt, ctx, 0u);
}

bool p3p_unsupported_hook(psprecomp::Runtime &rt, psprecomp::AllegrexContext &,
                          std::uint32_t pc, std::uint32_t instruction) {
    const std::uint32_t count = ++g_trap_counts[pc];
    if (count == 1u)
        std::fprintf(stderr, "[p3p] unsupported %s at %s -> interpreter fallback\n",
                     psprecomp::hex32(instruction).c_str(), psprecomp::hex32(pc).c_str());
    // Registering a non-"recomp_unit_" function at the trap address
    // poisons its 16 KiB generated unit (the same mechanism that keeps
    // import stubs dispatchable): outer dispatch stops entering the
    // unit wholesale and resolves this pc exactly -- to the bridge --
    // while the bucket's other blocks keep running AOT via exact
    // dispatch. The currently-running AOT function returns right
    // after this hook, and the dispatcher lands here.
    rt.register_function(pc, &p3p_interpret_bridge, "p3p-interpret-bridge");
    return count <= 1000000u;
}

}  // namespace

void init_profile(psprecomp::Runtime &runtime, const psprecomp::Elf32Image &elf,
                  std::uint32_t load_base, P3PKernel *kernel, bool aot) {
    g_kernel = kernel;
    psprecomp::set_runtime_unsupported_hook(&p3p_unsupported_hook);
    psprecomp::set_interpreter_fallback_enabled(true);
    g_import_by_stub.clear();

    const auto module = elf.find_module_info(runtime.memory(), load_base);
    if (module) {
        const auto imports = elf.scan_imports(runtime.memory(), *module);
        for (const auto &imp : imports) {
            const std::string library = imp.library;
            const std::uint32_t nid = imp.nid;
            runtime.register_hle(library, nid,
                                 [kernel, library, nid](psprecomp::Runtime &rt,
                                                        psprecomp::AllegrexContext &ctx) {
                                     dispatch_import(kernel, library, nid, rt, ctx);
                                 });
            if (!aot) {
                // Interpreter dispatch reaches imports through the function
                // registered at the stub address (trace-runner pattern).
                g_import_by_stub[imp.stub_address] = {library, nid};
                runtime.register_function(imp.stub_address, &import_stub_function,
                                          library + "::" + psprecomp::hex32(nid));
            }
        }
        std::fprintf(stderr, "[p3p] registered %zu import bindings (aot=%d, kernel=%d)\n",
                     imports.size(), aot ? 1 : 0, kernel != nullptr ? 1 : 0);
    }

    if (kernel != nullptr)
        kernel->register_main_module(load_base, P3PKernel::kRamEnd - load_base);

    runtime.register_function(P3PKernel::kExitStubAddress, &thread_exit_function,
                              "p3p::thread_exit_stub");
}

}  // namespace p3p
