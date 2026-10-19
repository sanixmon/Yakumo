#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cstring>
#include <filesystem>
#include <iomanip>

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Usage: p3p_trace_runner <ELF> [--max-dispatches N]\n";
        return 1;
    }

    const std::filesystem::path elf_path = argv[1];
    std::uint64_t max_dispatches = 1000;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--max-dispatches" && i + 1 < argc) {
            max_dispatches = std::stoull(argv[++i]);
        }
    }

    try {
        std::cout << "[p3p_trace_runner] Loading ELF: " << elf_path << "\n";
        auto elf = psprecomp::Elf32Image::from_file(elf_path);
        
        // Allocate 32 MiB guest RAM
        psprecomp::Runtime runtime(32u * 1024u * 1024u);
        const std::uint32_t load_base = psprecomp::kDefaultPspUserLoadBase;
        const auto relocations = elf.load_and_relocate(runtime.memory(), load_base);
        std::cout << "[p3p_trace_runner] Relocated " << relocations.total << " entries to load base " 
                  << psprecomp::hex32(load_base) << "\n";

        // Register default NID stubs so unresolved imports don't crash immediately
        const auto module = elf.find_module_info(runtime.memory(), load_base);
        if (module) {
            const auto imports = elf.scan_imports(runtime.memory(), *module);
            for (const auto &imp : imports) {
                runtime.register_hle(imp.library, imp.nid, [imp](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                    ctx.set_gpr(2, 0); // return 0 in $v0
                });
                // Also register function stub at stub_address so interpreter intercepts it
                runtime.register_function(imp.stub_address, [](psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
                    ctx.set_gpr(2, 0); // return 0 in $v0
                    ctx.pc = ctx.gpr[31]; // return to $ra
                }, imp.library + "::" + psprecomp::hex32(imp.nid));
            }
            std::cout << "[p3p_trace_runner] Registered " << imports.size() << " stub imports\n";
        }

        const std::uint32_t entry = elf.runtime_entry(load_base);
        std::cout << "[p3p_trace_runner] Starting execution at entry " << psprecomp::hex32(entry) 
                  << " (max_dispatches=" << max_dispatches << ")...\n";

        // Equalize initial conditions with PPSSPP:
        // Poison initial registers with 0xdeadbeef to match emulator baseline
        for (int i = 1; i < 32; ++i) {
            runtime.cpu().set_gpr(i, 0xdeadbeefu);
        }

        // Setup boot path string in stack area
        const char *boot_path = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
        const uint32_t path_len = static_cast<uint32_t>(strlen(boot_path));
        const uint32_t path_addr = 0x09fffee0u;
        for (uint32_t i = 0; i <= path_len; ++i) {
            runtime.memory().store8(path_addr + i, static_cast<uint8_t>(boot_path[i]));
        }

        runtime.cpu().set_gpr(29, 0x09fffea0u); // $sp matching stack offset
        if (module) runtime.cpu().set_gpr(28, module->gp); // $gp
        runtime.cpu().set_gpr(4, path_len); // $a0 = 27 (0x1b)
        runtime.cpu().set_gpr(5, path_addr); // $a1 = argv pointer
        runtime.cpu().set_gpr(31, 0x08800000u); // dummy $ra

        runtime.run(entry, max_dispatches);
        std::cout << "[p3p_trace_runner] Execution completed. Stop reason: " << runtime.stop_reason() << "\n";

        return 0;
    } catch (const std::exception &e) {
        std::cerr << "[p3p_trace_runner] Error: " << e.what() << "\n";
        return 2;
    }
}
