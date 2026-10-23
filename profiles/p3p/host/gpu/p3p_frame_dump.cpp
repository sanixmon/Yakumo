// P3P profile framebuffer dump (Gate 6 WP2). See the header.

#include "gpu/p3p_frame_dump.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace p3p {
namespace {

std::uint32_t crc32_of(const std::uint8_t *data, std::size_t size) {
    static std::uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i)
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

std::uint32_t adler32_of(const std::uint8_t *data, std::size_t size) {
    std::uint32_t a = 1, b = 0;
    for (std::size_t i = 0; i < size; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void put_u32be(std::vector<std::uint8_t> &out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

void write_chunk(FILE *file, const char *type, const std::vector<std::uint8_t> &data) {
    std::vector<std::uint8_t> head;
    put_u32be(head, static_cast<std::uint32_t>(data.size()));
    std::fwrite(head.data(), 1, head.size(), file);
    std::fwrite(type, 1, 4, file);
    if (!data.empty())
        std::fwrite(data.data(), 1, data.size(), file);
    std::vector<std::uint8_t> crc_in;
    crc_in.insert(crc_in.end(), type, type + 4);
    crc_in.insert(crc_in.end(), data.begin(), data.end());
    std::vector<std::uint8_t> crc_out;
    put_u32be(crc_out, crc32_of(crc_in.data(), crc_in.size()));
    std::fwrite(crc_out.data(), 1, crc_out.size(), file);
}

void decode_rgba(const psprecomp::GuestMemory &memory, std::uint32_t addr, std::uint32_t format,
                 std::uint8_t out[4]) {
    if (format == 3) {
        out[0] = memory.load8(addr);
        out[1] = memory.load8(addr + 1);
        out[2] = memory.load8(addr + 2);
        out[3] = memory.load8(addr + 3);
        return;
    }
    const std::uint16_t v = static_cast<std::uint16_t>(
        memory.load8(addr) | (memory.load8(addr + 1) << 8));
    switch (format) {
        case 0:
            out[0] = static_cast<std::uint8_t>(((v & 0x1Fu) << 3) | ((v >> 2) & 7u));
            out[1] = static_cast<std::uint8_t>(((v >> 5 & 0x3Fu) << 2) | ((v >> 9) & 3u));
            out[2] = static_cast<std::uint8_t>(((v >> 11 & 0x1Fu) << 3) | ((v >> 13) & 7u));
            out[3] = 255;
            break;
        case 1:
            out[0] = static_cast<std::uint8_t>(((v & 0x1Fu) << 3) | ((v >> 2) & 7u));
            out[1] = static_cast<std::uint8_t>(((v >> 5 & 0x1Fu) << 3) | ((v >> 7) & 7u));
            out[2] = static_cast<std::uint8_t>(((v >> 10 & 0x1Fu) << 3) | ((v >> 12) & 7u));
            out[3] = (v & 0x8000u) ? 255 : 0;
            break;
        default:
            out[0] = static_cast<std::uint8_t>((v & 0xFu) * 17u);
            out[1] = static_cast<std::uint8_t>(((v >> 4) & 0xFu) * 17u);
            out[2] = static_cast<std::uint8_t>(((v >> 8) & 0xFu) * 17u);
            out[3] = static_cast<std::uint8_t>(((v >> 12) & 0xFu) * 17u);
            break;
    }
}

}  // namespace

bool write_framebuffer_png(psprecomp::GuestMemory &memory, std::uint32_t addr,
                           std::uint32_t stride_px, std::uint32_t width, std::uint32_t height,
                           std::uint32_t format, const std::string &path) {
    if (width == 0 || height == 0 || stride_px < width)
        return false;
    const std::uint32_t psize = format == 3 ? 4u : 2u;

    // Raw scanlines: filter byte 0 + RGBA pixels.
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(height) * (1u + width * 4u));
    for (std::uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t paddr = addr + (y * stride_px + x) * psize;
            std::uint8_t px[4] = {0, 0, 0, 255};
            if (memory.contains(paddr, psize))
                decode_rgba(memory, paddr, format, px);
            raw.insert(raw.end(), px, px + 4);
        }
    }

    // zlib stream with stored (uncompressed) DEFLATE blocks.
    std::vector<std::uint8_t> idat;
    idat.push_back(0x78);
    idat.push_back(0x01);
    std::size_t off = 0;
    while (off < raw.size()) {
        const std::size_t chunk = std::min<std::size_t>(raw.size() - off, 65535u);
        const bool last = off + chunk >= raw.size();
        idat.push_back(last ? 1 : 0);
        idat.push_back(static_cast<std::uint8_t>(chunk));
        idat.push_back(static_cast<std::uint8_t>(chunk >> 8));
        const std::uint16_t nlen = static_cast<std::uint16_t>(~static_cast<std::uint16_t>(chunk));
        idat.push_back(static_cast<std::uint8_t>(nlen));
        idat.push_back(static_cast<std::uint8_t>(nlen >> 8));
        idat.insert(idat.end(), raw.begin() + static_cast<std::ptrdiff_t>(off),
                    raw.begin() + static_cast<std::ptrdiff_t>(off + chunk));
        off += chunk;
    }
    std::vector<std::uint8_t> adler;
    put_u32be(adler, adler32_of(raw.data(), raw.size()));
    idat.insert(idat.end(), adler.begin(), adler.end());

    FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
        return false;
    static constexpr std::uint8_t kSignature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    std::fwrite(kSignature, 1, 8, file);
    std::vector<std::uint8_t> ihdr;
    put_u32be(ihdr, width);
    put_u32be(ihdr, height);
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(6);  // color type RGBA
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    write_chunk(file, "IHDR", ihdr);
    write_chunk(file, "IDAT", idat);
    write_chunk(file, "IEND", {});
    const bool ok = std::fclose(file) == 0;
    return ok;
}

}  // namespace p3p
