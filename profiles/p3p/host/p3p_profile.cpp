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

void init_profile(psprecomp::Runtime &runtime, const psprecomp::Elf32Image &elf,
                  std::uint32_t load_base, P3PKernel *kernel, bool aot) {
    g_kernel = kernel;
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
