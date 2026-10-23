#include "p3p_profile.hpp"

#include "hle/p3p_hle_io.hpp"
#include "hle/p3p_hle_gedisplay.hpp"
#include "hle/p3p_hle_services.hpp"
#include "hle/p3p_hle_sysmem.hpp"
#include "hle/p3p_hle_audio.hpp"
#include "hle/p3p_hle_umd.hpp"
#include "hle/p3p_hle_mpeg.hpp"
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
    if (library == "sceMpeg" && hle_mpeg_dispatch(runtime, ctx, nid))
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
std::unordered_map<std::uint32_t, std::uint32_t> g_bridge_execs;

// ---------------------------------------------------------------------------
// VFPU instructions neither the code generator nor the framework
// interpreter implements: vcrs/vdet from the VFPU1 group and the VFPU7
// int-pack family (vi2uc/vi2c/vi2us/vi2s). The bridge executes these
// directly on the context, mirroring PPSSPP's interpreter semantics
// (forced prefix rewrites included) with the framework's own prefix
// machinery. Operand field extraction matches the interpreter's local
// helpers in src/interpreter.cpp (duplicated here; ADR-004 keeps this
// profile-side).

constexpr std::uint32_t p3p_vfpu_length(std::uint32_t word) noexcept {
    return (((word >> 7u) & 1u) | (((word >> 15u) & 1u) << 1u)) + 1u;
}
constexpr std::uint32_t p3p_vfpu_vd(std::uint32_t word) noexcept { return word & 0x7Fu; }
constexpr std::uint32_t p3p_vfpu_vs(std::uint32_t word) noexcept { return (word >> 8u) & 0x7Fu; }
constexpr std::uint32_t p3p_vfpu_vt(std::uint32_t word) noexcept { return (word >> 16u) & 0x7Fu; }

// Apply a source prefix whose control register was rewritten first --
// the execute_vfpu_vhdp idiom: swap, apply, restore.
void p3p_apply_rewritten_prefix(psprecomp::AllegrexContext &ctx, float *values,
                                std::uint32_t control_index, std::uint32_t rewritten) {
    const std::uint32_t original = ctx.vfpu_ctrl[control_index];
    ctx.vfpu_ctrl[control_index] = rewritten;
    ctx.apply_vfpu_source_prefix(values, 4u, control_index);
    ctx.vfpu_ctrl[control_index] = original;
}

// vcrs (VFPU1 sub-op 5): "half a cross product". Hardware rewrites the
// S swizzle to (y,z,x) and the T swizzle to (z,x,y), then multiplies
// elementwise, yielding only the positive terms of the cross product
// (s.y*t.z, s.z*t.x, s.x*t.y); callers supply the negative terms.
bool p3p_exec_vcrs(psprecomp::AllegrexContext &ctx, std::uint32_t word) {
    float s[4]{}, t[4]{}, d[4]{};
    ctx.read_vfpu_vector(s, p3p_vfpu_vs(word), 3u);
    ctx.read_vfpu_vector(t, p3p_vfpu_vt(word), 3u);
    p3p_apply_rewritten_prefix(ctx, s, 0u, (ctx.vfpu_ctrl[0] & ~0x3Fu) | 0x09u);
    p3p_apply_rewritten_prefix(ctx, t, 1u, (ctx.vfpu_ctrl[1] & ~0x3Fu) | 0x12u);
    for (int i = 0; i < 4; ++i) d[i] = s[i] * t[i];
    ctx.write_vfpu_vector_with_destination_prefix(d, p3p_vfpu_vd(word), 3u);
    return true;
}

// vdet (VFPU1 sub-op 6): determinant of two pairs,
// d = s.x * t.y - s.y * t.x, computed PPSSPP-style: sources read as
// pairs, prefixes applied through a four-lane view, the T swizzle
// forced to (y,x), s.y negated, then a four-lane dot product.
bool p3p_exec_vdet(psprecomp::AllegrexContext &ctx, std::uint32_t word) {
    float s[4]{}, t[4]{};
    ctx.read_vfpu_vector(s, p3p_vfpu_vs(word), 2u);
    ctx.read_vfpu_vector(t, p3p_vfpu_vt(word), 2u);
    ctx.apply_vfpu_source_prefix(s, 4u, 0u);
    p3p_apply_rewritten_prefix(ctx, t, 1u, (ctx.vfpu_ctrl[1] & ~0x0Fu) | 0x01u);
    s[1] = -s[1];
    float d[1]{s[0] * t[0] + s[1] * t[1] + s[2] * t[2] + s[3] * t[3]};
    ctx.write_vfpu_vector_with_destination_prefix(d, p3p_vfpu_vd(word), 1u);
    return true;
}

// vi2uc/vi2c/vi2us/vi2s (VFPU7 sub-table 28-31): pack the source
// lanes (their raw float bit patterns, after the S prefix) into
// bytes or halves. Mirrors PPSSPP Int_Vi2x.
bool p3p_exec_vi2x(psprecomp::AllegrexContext &ctx, std::uint32_t word) {
    const std::uint32_t length = p3p_vfpu_length(word);
    float sf[4]{};
    ctx.read_vfpu_vector(sf, p3p_vfpu_vs(word), length);
    ctx.apply_vfpu_source_prefix(sf, 4u, 0u);
    std::uint32_t s[4]{};
    for (int i = 0; i < 4; ++i) s[i] = std::bit_cast<std::uint32_t>(sf[i]);
    std::uint32_t bits[4]{};
    std::uint32_t out_length = 1u;
    switch ((word >> 16u) & 3u) {
        case 0u: {  // vi2uc: byte = clamp-to-zero(bits) >> 23
            std::uint32_t packed = 0u;
            for (std::uint32_t i = 0u; i < 4u; ++i) {
                std::int32_t v = std::bit_cast<std::int32_t>(s[i]);
                if (v < 0) v = 0;
                v >>= 23;
                packed |= (static_cast<std::uint32_t>(v) & 0xFFu) << (i * 8u);
            }
            bits[0] = packed;
            break;
        }
        case 1u: {  // vi2c: byte = bits >> 24
            std::uint32_t packed = 0u;
            for (std::uint32_t i = 0u; i < 4u; ++i)
                packed |= (s[i] >> 24u) << (i * 8u);
            bits[0] = packed;
            break;
        }
        case 2u:    // vi2us: halves = clamp-to-zero(bits) >> 15
        case 3u: {  // vi2s:  halves = bits >> 16
            const bool clamp_shift15 = ((word >> 16u) & 3u) == 2u;
            const std::uint32_t elems = (length + 1u) / 2u;
            for (std::uint32_t i = 0u; i < elems; ++i) {
                std::uint32_t lo = s[i * 2u], hi = s[i * 2u + 1u];
                if (clamp_shift15) {
                    std::int32_t ilo = std::bit_cast<std::int32_t>(lo);
                    std::int32_t ihi = std::bit_cast<std::int32_t>(hi);
                    if (ilo < 0) ilo = 0;
                    if (ihi < 0) ihi = 0;
                    lo = static_cast<std::uint32_t>(ilo >> 15);
                    hi = static_cast<std::uint32_t>(ihi >> 15);
                } else {
                    lo >>= 16u;
                    hi >>= 16u;
                }
                bits[i] = (lo & 0xFFFFu) | ((hi & 0xFFFFu) << 16u);
            }
            out_length = elems;
            break;
        }
    }
    float result[4]{};
    for (std::uint32_t i = 0u; i < out_length; ++i)
        result[i] = std::bit_cast<float>(bits[i]);
    ctx.write_vfpu_vector_with_destination_prefix(result, p3p_vfpu_vd(word), out_length);
    return true;
}

// Execute one of the bridge-implemented VFPU instructions; returns
// false when the trapping word is not one of them.
bool p3p_vfpu_execute(psprecomp::AllegrexContext &ctx, std::uint32_t word) {
    switch (word >> 26u) {
        case 0x19u: {  // VFPU1 group
            const std::uint32_t sub = (word >> 23u) & 7u;
            if (sub == 5u) return p3p_exec_vcrs(ctx, word);
            if (sub == 6u) return p3p_exec_vdet(ctx, word);
            return false;
        }
        case 0x34u: {  // VFPU4 jump table -> VFPU7 sub-table
            if (((word >> 21u) & 0x1Fu) != 1u) return false;
            const std::uint32_t sub = (word >> 16u) & 0x1Fu;
            if (sub >= 28u && sub <= 31u) return p3p_exec_vi2x(ctx, word);
            return false;
        }
        default:
            return false;
    }
}

// Exact-PC stand-in for a trap address: interpret guest code from
// here until the interpreter hands control back to registered code
// (its Dispatch exit), its budget cycles, or the run stops. One call
// is enough -- the outer dispatcher re-enters the fallback itself for
// any still-unregistered pc.
void p3p_interpret_bridge(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    // Real implementations first: if the trapping instruction is one
    // of the VFPU ops implemented above, execute it on the context
    // and interpret onward from the next instruction.
    const std::uint32_t word = rt.memory().load32(ctx.pc);
    if (p3p_vfpu_execute(ctx, word)) {
        const std::uint32_t count = ++g_bridge_execs[ctx.pc];
        if (count == 1u)
            std::fprintf(stderr, "[p3p] bridge: executed VFPU %s at %s\n",
                         psprecomp::hex32(word).c_str(), psprecomp::hex32(ctx.pc).c_str());
        ctx.pc += 4u;
        psprecomp::interpret_allegrex(rt, ctx, 0u);
        return;
    }
    // TEMP PROBE (Gate 6): the trapping instruction is not one of the
    // implemented ones. Skip it -- its result register keeps a stale
    // value -- and interpret onward, purely to map how far the boot
    // advances and which trap sites are actually load-bearing. NOT a
    // semantic fix; reachable sites get real implementations.
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
