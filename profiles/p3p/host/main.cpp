// P3P AOT executable host.
//
// Boots the decrypted P3P ELF through the generated AOT corpus (or, with
// --interpret, through the interpreter fallback for oracle tracing), with
// the profile kernel providing thread bootstrap (Gate 4B).
//
// Usage: p3p <ELF> [--max-dispatches N] [--interpret]
// Oracle tracing (interpreter mode): set YAKUMO_ORACLE_TRACE=<file> and
// YAKUMO_ORACLE_MODE=instruction|call in the environment.

#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"

#include "kernel/p3p_kernel.hpp"
#include "p3p_profile.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#ifdef P3P_HAS_GENERATED
namespace psprecomp {
void register_generated_functions(Runtime &runtime);
}
#endif

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: p3p <ELF> [--max-dispatches N] [--interpret] [--game-root DIR]\n");
        return 1;
    }
    const std::filesystem::path elf_path = argv[1];
    std::uint64_t max_dispatches = 10'000'000u;
    bool interpret = false;
    std::filesystem::path game_root;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--max-dispatches" && i + 1 < argc)
            max_dispatches = std::stoull(argv[++i]);
        else if (arg == "--interpret")
            interpret = true;
        else if (arg == "--game-root" && i + 1 < argc)
            game_root = argv[++i];
    }

    try {
        auto elf = psprecomp::Elf32Image::from_file(elf_path);

        p3p::P3PKernel kernel;
        psprecomp::Runtime runtime(32u * 1024u * 1024u);
        const std::uint32_t load_base = psprecomp::kDefaultPspUserLoadBase;
        const auto relocations = elf.load_and_relocate(runtime.memory(), load_base);
        std::fprintf(stderr, "[p3p] relocated %u entries to load base %s\n", relocations.total,
                     psprecomp::hex32(load_base).c_str());

        if (!game_root.empty()) {
            runtime.set_game_root(game_root);
            std::fprintf(stderr, "[p3p] game root: %s\n", game_root.string().c_str());
        }

        bool aot = !interpret;
#ifdef P3P_HAS_GENERATED
        if (aot)
            psprecomp::register_generated_functions(runtime);
#else
        aot = false;
#endif
        p3p::init_profile(runtime, elf, load_base, &kernel, aot);

        const auto module = elf.find_module_info(runtime.memory(), load_base);
        const std::uint32_t gp = module ? module->gp : 0u;
        const std::uint32_t entry = elf.runtime_entry(load_base);

        const std::int32_t loader_uid =
            kernel.bootstrap_loader("loader", entry, 0x20, gp, 0x10000u);
        if (loader_uid < 0) {
            std::fprintf(stderr, "[p3p] failed to bootstrap loader thread: %s\n",
                         psprecomp::hex32(static_cast<std::uint32_t>(loader_uid)).c_str());
            return 2;
        }

        // Initial register state, equalized with the PPSSPP oracle baseline
        // (same setup as tools/p3p_trace_runner): poison, then the boot
        // argument block. PPSSPP passes a 27-byte boot argument: the 26-char
        // path "/PSP_GAME/SYSDIR/EBOOT.BIN" plus its NUL terminator
        // (PROJECT_STATE.md section 4: observed arglen 27).
        for (int i = 1; i < 32; ++i)
            runtime.cpu().set_gpr(i, 0xdeadbeefu);
        const char *boot_path = "/PSP_GAME/SYSDIR/EBOOT.BIN";
        const std::uint32_t arglen = static_cast<std::uint32_t>(std::strlen(boot_path)) + 1u;
        const std::uint32_t path_addr = 0x09fffee0u;
        for (std::uint32_t i = 0; i < arglen; ++i)
            runtime.memory().store8(path_addr + i, static_cast<std::uint8_t>(boot_path[i]));
        runtime.cpu().set_gpr(29, 0x09fffea0u);  // $sp
        runtime.cpu().set_gpr(28, gp);           // $gp
        runtime.cpu().set_gpr(4, arglen);        // $a0
        runtime.cpu().set_gpr(5, path_addr);     // $a1
        runtime.cpu().set_gpr(31, p3p::P3PKernel::kExitStubAddress);  // $ra

        std::fprintf(stderr, "[p3p] starting at entry %s (aot=%d, max_dispatches=%llu)\n",
                     psprecomp::hex32(entry).c_str(), aot ? 1 : 0,
                     static_cast<unsigned long long>(max_dispatches));
        runtime.run(entry, max_dispatches);
        std::fprintf(stderr, "[p3p] stopped: %s\n", runtime.stop_reason().c_str());
        std::fprintf(stderr, "[p3p] kernel: %zu threads, current uid %d\n",
                     kernel.thread_count(), kernel.current_uid());
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[p3p] error: %s\n", e.what());
        return 2;
    }
}
