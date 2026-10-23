// P3P profile PRX module loader (Gate 5D). See the header.

#include "kernel/p3p_module_loader.hpp"

#include "kernel/p3p_kernel.hpp"

#include <algorithm>
#include <cstring>

namespace p3p {
namespace {

constexpr std::uint16_t kElfTypeSceRelexec = 0xFFA0u;
constexpr std::uint32_t kPtLoad = 1u;
constexpr std::uint32_t kShtPrxRel = 0x700000A0u;
constexpr std::uint32_t kFnidModuleStart = 0xD632ACDB;

std::uint16_t rd16(const std::vector<std::uint8_t> &b, std::size_t off) {
    return static_cast<std::uint16_t>(b[off] | (b[off + 1] << 8));
}
std::uint32_t rd32(const std::vector<std::uint8_t> &b, std::size_t off) {
    return static_cast<std::uint32_t>(b[off]) | (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) |
           (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

void write_bytes(psprecomp::GuestMemory &memory, std::uint32_t addr, const std::uint8_t *data,
                 std::size_t size) {
    std::size_t i = 0;
    for (; i + 4u <= size; i += 4u) {
        memory.store32(addr + static_cast<std::uint32_t>(i),
                       static_cast<std::uint32_t>(data[i]) |
                           (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                           (static_cast<std::uint32_t>(data[i + 2]) << 16) |
                           (static_cast<std::uint32_t>(data[i + 3]) << 24));
    }
    for (; i < size; ++i)
        memory.store8(addr + static_cast<std::uint32_t>(i), data[i]);
}

void zero_bytes(psprecomp::GuestMemory &memory, std::uint32_t addr, std::uint32_t size) {
    std::uint32_t i = 0;
    for (; i + 4u <= size; i += 4u)
        memory.store32(addr + i, 0u);
    for (; i < size; ++i)
        memory.store8(addr + i, 0u);
}

std::int32_t sign_extend16(std::uint32_t v) {
    return static_cast<std::int32_t>(static_cast<std::int16_t>(v & 0xFFFFu));
}

}  // namespace

PrxLoadResult load_prx_module(psprecomp::GuestMemory &memory, P3PKernel &kernel,
                              const std::vector<std::uint8_t> &bytes) {
    PrxLoadResult out;
    const auto fail = [&out](const std::string &why) {
        out.ok = false;
        out.error = why;
        return out;
    };

    if (bytes.size() < 52u || bytes[0] != 0x7Fu || bytes[1] != 'E' || bytes[2] != 'L' ||
        bytes[3] != 'F' || bytes[4] != 1u || bytes[5] != 1u)
        return fail("not a little-endian ELF32 file");
    if (rd16(bytes, 16) != kElfTypeSceRelexec)
        return fail("not an ET_SCE_RELEXE module");
    if (rd16(bytes, 18) != 8u)
        return fail("not a MIPS module");

    const std::uint32_t phoff = rd32(bytes, 28);
    const std::uint16_t phnum = rd16(bytes, 44);
    const std::uint32_t shoff = rd32(bytes, 32);
    const std::uint16_t shnum = rd16(bytes, 48);
    const std::uint16_t shstrndx = rd16(bytes, 50);
    if (phoff == 0u || phnum == 0u || phnum > 32u)
        return fail("bad program header table");

    struct LoadSeg {
        std::uint32_t offset, vaddr, filesz, memsz;
    };
    std::vector<LoadSeg> segs;
    std::uint32_t min_vaddr = 0xFFFFFFFFu, max_end = 0;
    for (std::uint16_t i = 0; i < phnum; ++i) {
        const std::size_t p = phoff + static_cast<std::size_t>(i) * 32u;
        if (p + 32u > bytes.size())
            return fail("program header out of file");
        if (rd32(bytes, p) != kPtLoad)
            continue;
        LoadSeg seg{rd32(bytes, p + 4), rd32(bytes, p + 8), rd32(bytes, p + 16),
                    rd32(bytes, p + 20)};
        if (seg.offset + seg.filesz > bytes.size())
            return fail("segment out of file");
        min_vaddr = std::min(min_vaddr, seg.vaddr);
        max_end = std::max(max_end, seg.vaddr + seg.memsz);
        segs.push_back(seg);
    }
    if (segs.empty() || max_end <= min_vaddr)
        return fail("no loadable segments");
    const std::uint32_t span = max_end - min_vaddr;

    // Sections: find .rodata.sceModuleInfo and every SHT_PRXREL section.
    struct RelSec {
        std::uint32_t offset, size;
    };
    std::vector<RelSec> rel_secs;
    std::uint32_t module_info_off = 0;
    if (shoff != 0u && shnum > 0u && shstrndx < shnum) {
        const std::size_t str_hdr = shoff + static_cast<std::size_t>(shstrndx) * 40u;
        if (str_hdr + 40u <= bytes.size()) {
            const std::uint32_t str_off = rd32(bytes, str_hdr + 16);
            const std::uint32_t str_size = rd32(bytes, str_hdr + 20);
            const auto name_at = [&](std::uint32_t name_off) -> std::string {
                if (name_off >= str_size)
                    return {};
                const std::size_t begin = str_off + name_off;
                std::size_t end = begin;
                while (end < bytes.size() && end < str_off + str_size && bytes[end] != 0)
                    ++end;
                return {reinterpret_cast<const char *>(bytes.data() + begin), end - begin};
            };
            for (std::uint16_t i = 0; i < shnum; ++i) {
                const std::size_t s = shoff + static_cast<std::size_t>(i) * 40u;
                if (s + 40u > bytes.size())
                    break;
                const std::uint32_t type = rd32(bytes, s + 4);
                if (type == kShtPrxRel)
                    rel_secs.push_back({rd32(bytes, s + 16), rd32(bytes, s + 20)});
                else if (name_at(rd32(bytes, s)) == ".rodata.sceModuleInfo")
                    module_info_off = rd32(bytes, s + 16);
            }
        }
    }
    if (module_info_off == 0u || module_info_off + 52u > bytes.size())
        return fail("module info section not found");

    // Module info (from the file image): name + gp + table ranges.
    std::string mod_name;
    for (std::size_t i = 0; i < 28u && bytes[module_info_off + 4 + i] != 0; ++i)
        mod_name += static_cast<char>(bytes[module_info_off + 4 + i]);
    if (mod_name.empty())
        return fail("module has no name");
    const std::uint32_t gp_rel = rd32(bytes, module_info_off + 32);
    const std::uint32_t ent_top = rd32(bytes, module_info_off + 36);
    const std::uint32_t ent_end = rd32(bytes, module_info_off + 40);

    // Allocate the image and place the segments.
    const std::int32_t block_uid =
        kernel.alloc_partition_memory(2, mod_name, 0, span, 0u);
    if (block_uid <= 0)
        return fail("partition allocation failed");
    const std::uint32_t head = kernel.block_head_address(block_uid);
    if (head == 0u) {
        kernel.free_partition_memory(block_uid);
        return fail("partition block has no head");
    }
    const std::uint32_t bias = head - min_vaddr;
    const auto at = [&](std::uint32_t vaddr) { return head + (vaddr - min_vaddr); };

    zero_bytes(memory, head, span);
    for (const LoadSeg &seg : segs) {
        if (seg.filesz > 0u)
            write_bytes(memory, at(seg.vaddr), bytes.data() + seg.offset, seg.filesz);
    }

    // Relocations. All entries are against the module's own load bias.
    std::vector<std::uint32_t> pending_hi16;  // guest addresses of lui words
    for (const RelSec &sec : rel_secs) {
        const std::uint32_t count = sec.size / 8u;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t e = sec.offset + static_cast<std::size_t>(i) * 8u;
            if (e + 8u > bytes.size())
                break;
            const std::uint32_t r_offset = rd32(bytes, e);
            const std::uint32_t type = rd32(bytes, e + 4) & 0xFFu;
            if (r_offset >= span) {
                ++out.relocations_skipped;
                continue;
            }
            const std::uint32_t addr = at(r_offset);
            const std::uint32_t word = memory.load32(addr);
            switch (type) {
                case 0u:  // R_MIPS_NONE
                    break;
                case 2u:  // R_MIPS_32
                    memory.store32(addr, word + bias);
                    ++out.relocations_applied;
                    break;
                case 4u: {  // R_MIPS_26
                    const std::uint32_t target = ((word & 0x03FFFFFFu) << 2) + bias;
                    memory.store32(addr, (word & 0xFC000000u) | ((target >> 2) & 0x03FFFFFFu));
                    ++out.relocations_applied;
                    break;
                }
                case 5u:  // R_MIPS_HI16: patch when its LO16 arrives
                    pending_hi16.push_back(addr);
                    break;
                case 6u: {  // R_MIPS_LO16
                    const std::int32_t lo = sign_extend16(word);
                    if (!pending_hi16.empty()) {
                        const std::uint32_t hi_word = memory.load32(pending_hi16.front());
                        const std::int32_t ahl =
                            static_cast<std::int32_t>((hi_word & 0xFFFFu) << 16) + lo;
                        const std::uint32_t value =
                            static_cast<std::uint32_t>(ahl) + bias;
                        const std::uint32_t new_hi = ((value + 0x8000u) >> 16) & 0xFFFFu;
                        for (const std::uint32_t hi_addr : pending_hi16) {
                            const std::uint32_t w = memory.load32(hi_addr);
                            memory.store32(hi_addr, (w & 0xFFFF0000u) | new_hi);
                        }
                        pending_hi16.clear();
                        memory.store32(addr, (word & 0xFFFF0000u) | (value & 0xFFFFu));
                        ++out.relocations_applied;
                    } else {
                        // Lone LO16: relocate the low half on its own.
                        const std::uint32_t value = static_cast<std::uint32_t>(lo) + bias;
                        memory.store32(addr, (word & 0xFFFF0000u) | (value & 0xFFFFu));
                        ++out.relocations_applied;
                    }
                    break;
                }
                default:  // GPREL16 and anything else: not needed pre-execution
                    ++out.relocations_skipped;
                    break;
            }
        }
    }
    out.relocations_skipped += static_cast<std::uint32_t>(pending_hi16.size());

    // module_start from the syslib export. Entry tables are 16 bytes
    // (PPSSPP sceKernelModule.cpp): {name, version u16, attribute u16,
    // size u8 (words), varCount u8, funcCount u16, resident u32}. The
    // resident table holds [func nids][var nids][func addrs][var addrs];
    // its pointer field is relocated like everything else, so it reads
    // as an absolute guest address here. (Entries with size > 4 carry
    // an extended var count in trailing fields; SDK-era modules use
    // size == 4, which is what this walk implements.) A module whose
    // syslib exports no functions legitimately has module_start == 0
    // (e.g. libsuppreacc) — StartModule then has nothing to run.
    std::uint32_t module_start = 0;
    if (ent_top != 0u && ent_end > ent_top && ent_end - ent_top <= 0x1000u) {
        std::uint32_t ent = at(ent_top);
        const std::uint32_t ent_limit = at(ent_end);
        for (int guard = 0; guard < 64 && ent + 16u <= ent_limit; ++guard) {
            const std::uint32_t lib_name = memory.load32(ent);
            const std::uint32_t size_words = memory.load8(ent + 8);
            const std::uint32_t var_count = memory.load8(ent + 9);
            const std::uint32_t func_count = memory.load16(ent + 10);
            const std::uint32_t resident = memory.load32(ent + 12);
            if (size_words == 0u)
                break;
            if (lib_name == 0u && resident != 0u) {  // syslib
                const std::uint32_t addrs = resident + (func_count + var_count) * 4u;
                for (std::uint32_t i = 0; i < func_count && i < 16u; ++i) {
                    if (memory.load32(resident + i * 4u) == kFnidModuleStart) {
                        module_start = memory.load32(addrs + i * 4u);
                        break;
                    }
                }
            }
            ent += size_words * 4u;
        }
    }

    out.uid = kernel.register_module(mod_name, head, span, bias + gp_rel, module_start, block_uid);
    out.ok = true;
    out.name = mod_name;
    out.base = head;
    out.size = span;
    out.gp = bias + gp_rel;
    out.module_start = module_start;
    return out;
}

}  // namespace p3p
