// Unit tests for the P3P Gate 5D PRX module loader: a hand-built
// ET_SCE_RELEXE fixture exercised end to end — segment placement, bss
// zeroing, R_MIPS_32/26/HI16/LO16 relocation math, module info and
// module_start recovery, kernel registry lifecycle.

#include "kernel/p3p_kernel.hpp"
#include "kernel/p3p_module_loader.hpp"

#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

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

// Builds the synthetic module described in GATE5D_PLAN.md's test plan.
std::vector<std::uint8_t> build_fixture() {
    std::vector<std::uint8_t> b(0x600u, 0u);
    const auto w16 = [&](std::size_t off, std::uint16_t v) {
        b[off] = static_cast<std::uint8_t>(v);
        b[off + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    const auto w32 = [&](std::size_t off, std::uint32_t v) {
        b[off] = static_cast<std::uint8_t>(v);
        b[off + 1] = static_cast<std::uint8_t>(v >> 8);
        b[off + 2] = static_cast<std::uint8_t>(v >> 16);
        b[off + 3] = static_cast<std::uint8_t>(v >> 24);
    };
    // ELF header.
    b[0] = 0x7F; b[1] = 'E'; b[2] = 'L'; b[3] = 'F'; b[4] = 1; b[5] = 1; b[6] = 1;
    w16(16, 0xFFA0u);  // ET_SCE_RELEXE
    w16(18, 8u);       // EM_MIPS
    w32(24, 0xFFFFFFFFu);
    w32(28, 52u);      // phoff
    w32(32, 0x400u);   // shoff
    w16(42, 32u);      // phentsize
    w16(44, 3u);       // phnum
    w16(46, 40u);      // shentsize
    w16(48, 4u);       // shnum
    w16(50, 3u);       // shstrndx
    // Program headers @52.
    const auto phdr = [&](std::size_t i, std::uint32_t type, std::uint32_t off, std::uint32_t va,
                          std::uint32_t filesz, std::uint32_t memsz, std::uint32_t flags) {
        const std::size_t p = 52u + i * 32u;
        w32(p + 0, type); w32(p + 4, off); w32(p + 8, va); w32(p + 12, 0u);
        w32(p + 16, filesz); w32(p + 20, memsz); w32(p + 24, flags); w32(p + 28, 0x10u);
    };
    phdr(0, 1u, 0x100u, 0u, 0x200u, 0x200u, 5u);
    phdr(1, 1u, 0x300u, 0x400u, 0x10u, 0x30u, 6u);
    phdr(2, 0x700000A0u, 0u, 0u, 0u, 0u, 0u);
    // Segment 0 body (file 0x100 = vaddr 0).
    w32(0x100, 0x00001234u);   // vaddr 0x00: R_MIPS_32 target
    w32(0x104, 0x3C080002u);   // vaddr 0x04: lui t0, 2      (HI16)
    w32(0x108, 0x25084000u);   // vaddr 0x08: addiu t0, 0x4000 (LO16)
    w32(0x10C, 0x08000040u);   // vaddr 0x0C: j 0x100        (R_MIPS_26)
    w32(0x140, 0x03E00008u);   // vaddr 0x40: module_start body (jr ra)
    // Module info @ vaddr 0x80 (file 0x180).
    std::memcpy(&b[0x184], "testmod", 7u);
    w32(0x180 + 32, 0x200u);        // gp
    w32(0x180 + 36, 0xC0u);         // ent_top
    w32(0x180 + 40, 0xC0u + 16u);   // ent_end (one 16-byte entry)
    // Syslib entry @ vaddr 0xC0 (file 0x1C0), 16-byte layout:
    // {name, version u16, attribute u16, size u8, varCount u8,
    //  funcCount u16, resident u32}.
    w32(0x1C0 + 4, 0x80000000u);    // version 0, attribute 0x8000
    b[0x1C0 + 8] = 4u;              // entry size (words)
    b[0x1C0 + 9] = 0u;              // var count
    w16(0x1C0 + 10, 1u);            // func count
    w32(0x1C0 + 12, 0xE0u);         // resident table vaddr
    // Resident table @ vaddr 0xE0: [fnids][func addrs].
    w32(0x1E0, 0xD632ACDBu);        // fnid: module_start
    w32(0x1E4, 0x40u);              // func: module_start vaddr
    // Segment 1 body (file 0x300 = vaddr 0x400): 16 bytes of 0xAA.
    for (std::size_t i = 0; i < 16u; ++i)
        b[0x300 + i] = 0xAAu;
    // Section headers @0x400.
    const auto shdr = [&](std::size_t i, std::uint32_t name, std::uint32_t type,
                          std::uint32_t off, std::uint32_t size, std::uint32_t entsize) {
        const std::size_t s = 0x400u + i * 40u;
        w32(s + 0, name); w32(s + 4, type); w32(s + 16, off); w32(s + 20, size);
        w32(s + 36, entsize);
    };
    shdr(1, 1u, 0x700000A0u, 0x500u, 6u * 8u, 8u);   // .rel.text
    shdr(2, 11u, 1u, 0x180u, 52u, 0u);               // .rodata.sceModuleInfo
    shdr(3, 34u, 3u, 0x580u, 44u, 0u);               // .shstrtab
    // Relocations @0x500: {r_offset, r_info = type}.
    const std::uint32_t relocs[][2] = {
        {0x00u, 2u}, {0x04u, 5u}, {0x08u, 6u}, {0x0Cu, 4u},
        {0xCCu, 2u}, {0xE4u, 2u},
    };
    for (std::size_t i = 0; i < 6u; ++i) {
        w32(0x500 + i * 8, relocs[i][0]);
        w32(0x500 + i * 8 + 4, relocs[i][1]);
    }
    // shstrtab @0x580.
    const char *names = "\0.rel.text\0.rodata.sceModuleInfo\0.shstrtab\0";
    std::memcpy(&b[0x580], names, 44u);
    return b;
}

}  // namespace

int main() {
    using namespace p3p;

    psprecomp::Runtime runtime(32u * 1024u * 1024u);
    P3PKernel kernel;
    kernel.register_main_module(0x08804000u, 0x1000u);

    const std::vector<std::uint8_t> bytes = build_fixture();
    const PrxLoadResult res = load_prx_module(runtime.memory(), kernel, bytes);
    check(res.ok, "fixture loads");
    if (!res.ok) {
        std::fprintf(stderr, "load error: %s\n", res.error.c_str());
        std::fprintf(stderr, "test_p3p_modules: %d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }
    const std::uint32_t base = res.base;
    auto &mem = runtime.memory();
    check(res.name == "testmod", "module name parsed");
    check(res.size == 0x430u, "span covers both segments + bss");
    check(res.gp == base + 0x200u, "gp relocated");
    check(res.module_start == base + 0x40u, "module_start recovered from syslib export");
    check(res.relocations_applied == 5u && res.relocations_skipped == 0u, "relocation counts");

    check(mem.load32(base + 0x00u) == base + 0x1234u, "R_MIPS_32 adds the bias");
    const std::uint32_t pair_value = base + 0x24000u;
    check((mem.load32(base + 0x04u) & 0xFFFFu) == ((pair_value + 0x8000u) >> 16 & 0xFFFFu),
          "HI16 holds the adjusted high half");
    check((mem.load32(base + 0x08u) & 0xFFFFu) == (pair_value & 0xFFFFu),
          "LO16 holds the low half");
    check((mem.load32(base + 0x0Cu) & 0x03FFFFFFu) == (((base + 0x100u) >> 2) & 0x03FFFFFFu),
          "R_MIPS_26 retargets the jump");
    check(mem.load8(base + 0x400u) == 0xAAu && mem.load8(base + 0x40Fu) == 0xAAu,
          "segment 1 bytes placed");
    check(mem.load8(base + 0x410u) == 0u && mem.load8(base + 0x420u) == 0u,
          "bss tail zeroed");

    check(kernel.module_uid_at(base + 0x100u) == res.uid, "module_uid_at finds the PRX");
    check(kernel.module_uid_at(0x08804000u) == kernel.main_module_uid(),
          "module_uid_at still finds the main module");
    check(kernel.find_module_by_name("testmod") == res.uid, "find by name");
    check(kernel.set_module_started(res.uid, true), "start marks the module");
    const P3PKernel::ModuleRecord *rec = kernel.find_module(res.uid);
    check(rec != nullptr && rec->started && rec->start_entry == base + 0x40u,
          "record carries started state + entry");
    check(kernel.unregister_module(res.uid), "unload removes the module");
    check(kernel.module_uid_at(base + 0x100u) == 0, "address unclaimed after unload");

    const std::vector<std::uint8_t> truncated(bytes.begin(), bytes.begin() + 30);
    check(!load_prx_module(runtime.memory(), kernel, truncated).ok, "truncated file rejected");

    std::fprintf(stderr, "test_p3p_modules: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
