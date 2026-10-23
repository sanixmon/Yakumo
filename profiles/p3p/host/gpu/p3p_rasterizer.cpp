// P3P profile software 2D rasterizer (Gate 6 WP2). See the header.

#include "gpu/p3p_rasterizer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace p3p::ge {
namespace {

using Regs = std::array<std::uint32_t, 256>;

// GE command ids used here (PPSSPP GPU/ge_constants.h).
constexpr std::uint8_t kCmdTextureMapEnable = 0x1E;
constexpr std::uint8_t kCmdAlphaBlendEnable = 0x21;
constexpr std::uint8_t kCmdAlphaTestEnable = 0x22;
constexpr std::uint8_t kCmdOffsetX = 0x4C;
constexpr std::uint8_t kCmdOffsetY = 0x4D;
constexpr std::uint8_t kCmdFrameBufPtr = 0x9C;
constexpr std::uint8_t kCmdFrameBufWidth = 0x9D;
constexpr std::uint8_t kCmdTexAddr0 = 0xA0;
constexpr std::uint8_t kCmdTexBufWidth0 = 0xA8;
constexpr std::uint8_t kCmdClutAddr = 0xB0;
constexpr std::uint8_t kCmdClutAddrHigh = 0xB1;
constexpr std::uint8_t kCmdTransferSrc = 0xB2;
constexpr std::uint8_t kCmdTransferSrcW = 0xB3;
constexpr std::uint8_t kCmdTransferDst = 0xB4;
constexpr std::uint8_t kCmdTransferDstW = 0xB5;
constexpr std::uint8_t kCmdTexSize0 = 0xB8;
constexpr std::uint8_t kCmdTexMode = 0xC2;
constexpr std::uint8_t kCmdTexFormat = 0xC3;
constexpr std::uint8_t kCmdClutFormat = 0xC5;
constexpr std::uint8_t kCmdAlphaTest = 0xDB;
constexpr std::uint8_t kCmdBlendMode = 0xDF;
constexpr std::uint8_t kCmdBlendFixedA = 0xE0;
constexpr std::uint8_t kCmdBlendFixedB = 0xE1;
constexpr std::uint8_t kCmdTransferStart = 0xEA;
constexpr std::uint8_t kCmdTransferSrcPos = 0xEB;
constexpr std::uint8_t kCmdTransferDstPos = 0xEC;
constexpr std::uint8_t kCmdTransferSize = 0xEE;

struct Rgba {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
};

Rgba expand565(std::uint16_t v) {
    Rgba c;
    c.r = static_cast<std::uint8_t>(((v & 0x1Fu) << 3) | ((v >> 2) & 7u));
    c.g = static_cast<std::uint8_t>(((v >> 5 & 0x3Fu) << 2) | ((v >> 9) & 3u));
    c.b = static_cast<std::uint8_t>(((v >> 11 & 0x1Fu) << 3) | ((v >> 13) & 7u));
    return c;
}
Rgba expand5551(std::uint16_t v) {
    Rgba c;
    c.r = static_cast<std::uint8_t>(((v & 0x1Fu) << 3) | ((v >> 2) & 7u));
    c.g = static_cast<std::uint8_t>(((v >> 5 & 0x1Fu) << 3) | ((v >> 7) & 7u));
    c.b = static_cast<std::uint8_t>(((v >> 10 & 0x1Fu) << 3) | ((v >> 12) & 7u));
    c.a = (v & 0x8000u) ? 255 : 0;
    return c;
}
Rgba expand4444(std::uint16_t v) {
    Rgba c;
    c.r = static_cast<std::uint8_t>((v & 0xFu) * 17u);
    c.g = static_cast<std::uint8_t>(((v >> 4) & 0xFu) * 17u);
    c.b = static_cast<std::uint8_t>(((v >> 8) & 0xFu) * 17u);
    c.a = static_cast<std::uint8_t>(((v >> 12) & 0xFu) * 17u);
    return c;
}
Rgba decode_pixel(std::uint32_t v, std::uint32_t format) {
    switch (format) {
        case 0: return expand565(static_cast<std::uint16_t>(v));
        case 1: return expand5551(static_cast<std::uint16_t>(v));
        case 2: return expand4444(static_cast<std::uint16_t>(v));
        default: {
            Rgba c;
            c.r = static_cast<std::uint8_t>(v);
            c.g = static_cast<std::uint8_t>(v >> 8);
            c.b = static_cast<std::uint8_t>(v >> 16);
            c.a = static_cast<std::uint8_t>(v >> 24);
            return c;
        }
    }
}
std::uint32_t encode_pixel(Rgba c, std::uint32_t format) {
    switch (format) {
        case 0:
            return (static_cast<std::uint32_t>(c.r >> 3)) |
                   (static_cast<std::uint32_t>(c.g >> 2) << 5) |
                   (static_cast<std::uint32_t>(c.b >> 3) << 11);
        case 1:
            return (static_cast<std::uint32_t>(c.r >> 3)) |
                   (static_cast<std::uint32_t>(c.g >> 3) << 5) |
                   (static_cast<std::uint32_t>(c.b >> 3) << 10) | (c.a >= 128 ? 0x8000u : 0u);
        case 2:
            return (static_cast<std::uint32_t>(c.r >> 4)) |
                   (static_cast<std::uint32_t>(c.g >> 4) << 4) |
                   (static_cast<std::uint32_t>(c.b >> 4) << 8) |
                   (static_cast<std::uint32_t>(c.a >> 4) << 12);
        default:
            return static_cast<std::uint32_t>(c.r) | (static_cast<std::uint32_t>(c.g) << 8) |
                   (static_cast<std::uint32_t>(c.b) << 16) |
                   (static_cast<std::uint32_t>(c.a) << 24);
    }
}
std::uint32_t pixel_size(std::uint32_t format) { return format == 3 ? 4u : 2u; }

std::uint16_t rd16(const psprecomp::GuestMemory &m, std::uint32_t addr) {
    return static_cast<std::uint16_t>(m.load8(addr) | (m.load8(addr + 1) << 8));
}
std::uint32_t rd32(const psprecomp::GuestMemory &m, std::uint32_t addr) {
    return static_cast<std::uint32_t>(rd16(m, addr)) |
           (static_cast<std::uint32_t>(rd16(m, addr + 2)) << 16);
}
void wr_pixel(psprecomp::GuestMemory &m, std::uint32_t addr, std::uint32_t v,
              std::uint32_t format) {
    if (format == 3) {
        m.store32(addr, v);
    } else {
        m.store8(addr, static_cast<std::uint8_t>(v));
        m.store8(addr + 1, static_cast<std::uint8_t>(v >> 8));
    }
}

// --- Vertex decoding -------------------------------------------------------

struct Vertex {
    float u = 0, v = 0;  // texture pixel coords
    Rgba color;          // vertex color (opaque white when none)
    float x = 0, y = 0;  // through-mode screen pixels
};

struct VertexLayout {
    std::uint32_t tc_off = 0, color_off = 0, pos_off = 0, stride = 0;
    int tc_fmt = 0, color_fmt = 0, pos_fmt = 0, idx_fmt = 0;
    bool through = false;
    bool valid = false;
};

std::uint32_t align_up(std::uint32_t v, std::uint32_t a) { return (v + a - 1) & ~(a - 1); }
std::uint32_t fmt_size(int fmt) { return fmt == 1 ? 1u : fmt == 2 ? 2u : 4u; }

VertexLayout vertex_layout(std::uint32_t vt) {
    VertexLayout l;
    l.tc_fmt = static_cast<int>(vt & 3u);
    l.color_fmt = static_cast<int>((vt >> 2) & 7u);
    const int nrm_fmt = static_cast<int>((vt >> 5) & 3u);
    l.pos_fmt = static_cast<int>((vt >> 7) & 3u);
    const int weight_fmt = static_cast<int>((vt >> 9) & 3u);
    l.idx_fmt = static_cast<int>((vt >> 11) & 3u);
    l.through = (vt & (1u << 23)) != 0;
    if (weight_fmt != 0 || l.pos_fmt == 0)
        return l;  // skinned or position-less: out of scope for 2D
    std::uint32_t off = 0;
    if (l.tc_fmt != 0) {
        off = align_up(off, fmt_size(l.tc_fmt));
        l.tc_off = off;
        off += fmt_size(l.tc_fmt) * 2u;
    }
    if (l.color_fmt != 0) {
        const std::uint32_t esz = l.color_fmt == 4 ? 4u : 2u;
        off = align_up(off, esz);
        l.color_off = off;
        off += esz;
    }
    if (nrm_fmt != 0) {
        off = align_up(off, fmt_size(nrm_fmt));
        off += fmt_size(nrm_fmt) * 3u;
    }
    off = align_up(off, fmt_size(l.pos_fmt));
    l.pos_off = off;
    off += fmt_size(l.pos_fmt) * 3u;
    l.stride = off;
    l.valid = l.stride > 0;
    return l;
}

float read_scalar(const psprecomp::GuestMemory &m, std::uint32_t addr, int fmt) {
    switch (fmt) {
        case 1:
            return static_cast<float>(static_cast<std::int8_t>(m.load8(addr)));
        case 2:
            return static_cast<float>(static_cast<std::int16_t>(rd16(m, addr)));
        case 3: {
            const std::uint32_t bits = rd32(m, addr);
            float f;
            std::memcpy(&f, &bits, 4);
            return f;
        }
        default:
            return 0.0f;
    }
}

Vertex decode_vertex(const psprecomp::GuestMemory &m, std::uint32_t base,
                     const VertexLayout &l, const Regs &regs) {
    Vertex v;
    if (l.tc_fmt != 0) {
        const std::uint32_t a = base + l.tc_off;
        if (l.through && l.tc_fmt == 2) {
            v.u = static_cast<float>(rd16(m, a)) / 32.0f;  // 5 fractional bits
            v.v = static_cast<float>(rd16(m, a + 2)) / 32.0f;
        } else if (l.through && l.tc_fmt == 1) {
            v.u = static_cast<float>(m.load8(a)) / 32.0f;
            v.v = static_cast<float>(m.load8(a + 1)) / 32.0f;
        } else {
            v.u = read_scalar(m, a, l.tc_fmt);
            v.v = read_scalar(m, a + fmt_size(l.tc_fmt), l.tc_fmt);
        }
    }
    if (l.color_fmt != 0) {
        const std::uint32_t a = base + l.color_off;
        if (l.color_fmt == 4) {
            v.color.r = m.load8(a);
            v.color.g = m.load8(a + 1);
            v.color.b = m.load8(a + 2);
            v.color.a = m.load8(a + 3);
        } else {
            // GE vertex color formats: 1=565, 2=5551, 3=4444.
            v.color = decode_pixel(rd16(m, a), static_cast<std::uint32_t>(l.color_fmt - 1));
        }
    }
    const std::uint32_t pa = base + l.pos_off;
    const float px = read_scalar(m, pa, l.pos_fmt);
    const float py = read_scalar(m, pa + fmt_size(l.pos_fmt), l.pos_fmt);
    if (l.through && l.pos_fmt == 2) {
        // 12.4 fixed positions minus the 12.4 drawing offset (PPSSPP).
        v.x = (px - static_cast<float>(regs[kCmdOffsetX] & 0xFFFFu)) / 16.0f;
        v.y = (py - static_cast<float>(regs[kCmdOffsetY] & 0xFFFFu)) / 16.0f;
    } else {
        v.x = px;
        v.y = py;
    }
    return v;
}

std::uint32_t vertex_index(const psprecomp::GuestMemory &m, const GeDrawCall &draw,
                           const VertexLayout &l, std::uint32_t i) {
    if (l.idx_fmt == 0 || draw.index_address == 0)
        return i;
    if (l.idx_fmt == 1)
        return m.load8(draw.index_address + i);
    if (l.idx_fmt == 2)
        return rd16(m, draw.index_address + i * 2u);
    return rd32(m, draw.index_address + i * 4u) & 0xFFFFu;
}

// --- Texture sampling ------------------------------------------------------

struct Texture {
    std::uint32_t addr = 0;
    std::uint32_t stride = 0;  // pixels per row (buffer width)
    int width = 0, height = 0;
    std::uint32_t format = 0;
    bool swizzled = false;
    std::uint32_t clut_addr = 0;
    std::uint32_t clut_format = 3;
    std::uint32_t clut_shift = 0, clut_mask = 0xFF, clut_start = 0;
    bool valid = false;
};

Texture texture_from_state(const Regs &regs) {
    Texture t;
    const std::uint32_t addr_arg = regs[kCmdTexAddr0];
    const std::uint32_t bw_arg = regs[kCmdTexBufWidth0];
    t.addr = (addr_arg & 0xFFFFFFu) | ((bw_arg & 0xFF0000u) << 8);
    t.stride = bw_arg & 0x7FFu;
    const std::uint32_t size_arg = regs[kCmdTexSize0];
    const std::uint32_t wexp = size_arg & 0xFFu;
    const std::uint32_t hexp = (size_arg >> 8) & 0xFFu;
    if (wexp <= 11u && hexp <= 11u) {
        t.width = 1 << wexp;
        t.height = 1 << hexp;
    }
    if (t.stride == 0)
        t.stride = static_cast<std::uint32_t>(t.width);
    t.format = regs[kCmdTexFormat] & 0xFFu;
    t.swizzled = (regs[kCmdTexMode] & 1u) != 0;
    t.clut_addr = (regs[kCmdClutAddr] & 0xFFFFFFu) | ((regs[kCmdClutAddrHigh] & 0xFFu) << 24);
    const std::uint32_t cf = regs[kCmdClutFormat];
    t.clut_format = cf & 3u;
    t.clut_shift = (cf >> 2) & 0x1Fu;
    t.clut_mask = (cf >> 8) & 0xFFu;
    t.clut_start = ((cf >> 16) & 0x1Fu) | (((cf >> 21) & 3u) << 5);
    t.valid = t.addr != 0 && t.width > 0 && t.height > 0;
    return t;
}

// PSP texture swizzle works on byte offsets: the texture is stored in
// 16-byte-wide, 8-row blocks. Map a linear byte offset accordingly.
std::uint32_t swizzle_offset(std::uint32_t row_bytes, std::uint32_t linear) {
    if (row_bytes < 16u)
        return linear;
    const std::uint32_t in_row = linear % row_bytes;
    const std::uint32_t row = linear / row_bytes;
    const std::uint32_t block = (row / 8u) * (row_bytes / 16u) + (in_row / 16u);
    return block * 128u + (row % 8u) * 16u + (in_row % 16u);
}

std::uint32_t texel_offset(const Texture &t, int x, int y, std::uint32_t bpp_bytes) {
    const std::uint32_t row_bytes = t.stride * bpp_bytes;
    const std::uint32_t linear =
        static_cast<std::uint32_t>(y) * row_bytes + static_cast<std::uint32_t>(x) * bpp_bytes;
    return t.swizzled ? swizzle_offset(row_bytes, linear) : linear;
}

Rgba clut_lookup(const psprecomp::GuestMemory &m, const Texture &t, std::uint32_t raw) {
    const std::uint32_t idx = ((raw >> t.clut_shift) & t.clut_mask) + t.clut_start;
    if (pixel_size(t.clut_format) == 4u)
        return decode_pixel(rd32(m, t.clut_addr + idx * 4u), t.clut_format);
    return decode_pixel(rd16(m, t.clut_addr + idx * 2u), t.clut_format);
}

Rgba sample_texture(const psprecomp::GuestMemory &m, const Texture &t, int x, int y) {
    x = std::clamp(x, 0, t.width - 1);
    y = std::clamp(y, 0, t.height - 1);
    switch (t.format) {
        case 0:
        case 1:
        case 2:
            return decode_pixel(rd16(m, t.addr + texel_offset(t, x, y, 2)), t.format);
        case 3:
            return decode_pixel(rd32(m, t.addr + texel_offset(t, x, y, 4)), 3);
        case 4: {  // CLUT4: two texels per byte
            const std::uint32_t row_bytes = t.stride / 2u;
            std::uint32_t linear = static_cast<std::uint32_t>(y) * row_bytes +
                                   static_cast<std::uint32_t>(x) / 2u;
            if (t.swizzled)
                linear = swizzle_offset(row_bytes, linear);
            const std::uint8_t packed = m.load8(t.addr + linear);
            return clut_lookup(m, t, (x & 1) ? (packed >> 4) : (packed & 0xFu));
        }
        case 5:  // CLUT8
            return clut_lookup(m, t, m.load8(t.addr + texel_offset(t, x, y, 1)));
        case 6: {  // CLUT16
            const std::uint32_t raw = rd16(m, t.addr + texel_offset(t, x, y, 2));
            return clut_lookup(m, t, raw);
        }
        default:
            return {255, 0, 255, 255};  // DXT etc.: magenta marker (later)
    }
}

// --- Blending / tests ------------------------------------------------------

// GE blend factor encoding (BLENDMODE bits 8-10 / 11-13).
std::uint32_t blend_factor(int factor, Rgba src, Rgba dst, std::uint32_t fixed, int channel) {
    const auto fixed_ch = [&](int i) { return (fixed >> (i * 8)) & 0xFFu; };
    switch (factor) {
        case 0: return 0;
        case 1: return 255;
        case 2: return src.a;
        case 3: return 255u - src.a;
        case 4: return dst.a;
        case 5: return 255u - dst.a;
        case 6: return std::min(255u, 2u * src.a);
        case 7: return 255u - std::min(255u, 2u * src.a);
        case 10: return fixed_ch(channel);
        default: return 255;
    }
}

Rgba blend_pixel(const Regs &regs, Rgba src, Rgba dst) {
    const std::uint32_t mode = regs[kCmdBlendMode];
    const int equation = static_cast<int>(mode & 7u);
    const int sfactor = static_cast<int>((mode >> 8) & 7u);
    const int dfactor = static_cast<int>((mode >> 11) & 7u);
    const std::uint32_t fixed_a = regs[kCmdBlendFixedA];
    const std::uint32_t fixed_b = regs[kCmdBlendFixedB];
    Rgba out;
    std::uint8_t *oc[4] = {&out.r, &out.g, &out.b, &out.a};
    const std::uint8_t sc[4] = {src.r, src.g, src.b, src.a};
    const std::uint8_t dc[4] = {dst.r, dst.g, dst.b, dst.a};
    for (int i = 0; i < 4; ++i) {
        const std::int32_t fs =
            static_cast<std::int32_t>(blend_factor(sfactor, src, dst, fixed_a, i));
        const std::int32_t fd =
            static_cast<std::int32_t>(blend_factor(dfactor, src, dst, fixed_b, i));
        std::int32_t val = sc[i];
        switch (equation) {
            case 0: val = (sc[i] * fs + dc[i] * fd) / 255; break;
            case 1: val = (sc[i] * fs - dc[i] * fd) / 255; break;
            case 2: val = (dc[i] * fd - sc[i] * fs) / 255; break;
            case 3: val = std::min(sc[i], dc[i]); break;
            case 4: val = std::max(sc[i], dc[i]); break;
            default: break;
        }
        *oc[i] = static_cast<std::uint8_t>(std::clamp(val, 0, 255));
    }
    return out;
}

bool alpha_test_passes(const Regs &regs, std::uint8_t a) {
    if ((regs[kCmdAlphaTestEnable] & 1u) == 0)
        return true;
    const std::uint32_t arg = regs[kCmdAlphaTest];
    const std::uint32_t ref = arg & 0xFFu;
    switch (static_cast<int>((arg >> 8) & 7u)) {
        case 0: return false;
        case 1: return true;
        case 2: return a == ref;
        case 3: return a != ref;
        case 4: return a < ref;
        case 5: return a <= ref;
        case 6: return a > ref;
        case 7: return a >= ref;
        default: return true;
    }
}

// --- Raster core -----------------------------------------------------------

struct Target {
    std::uint32_t addr = 0;
    std::uint32_t stride = 512;  // pixels per row
    std::uint32_t format = 3;
    std::uint32_t height = 0;    // clip rows (0 = unclipped)
};

void plot(psprecomp::GuestMemory &m, const Target &tg, const Regs &regs, int x, int y,
          Rgba color, bool blend_enabled, RasterStats &stats) {
    if (x < 0 || y < 0 || x >= static_cast<int>(tg.stride))
        return;
    if (tg.height != 0 && y >= static_cast<int>(tg.height))
        return;
    const std::uint32_t psize = pixel_size(tg.format);
    const std::uint32_t addr = tg.addr + (static_cast<std::uint32_t>(y) * tg.stride +
                                         static_cast<std::uint32_t>(x)) * psize;
    if (!m.contains(addr, psize))
        return;
    if (blend_enabled) {
        const Rgba dst = decode_pixel(psize == 4 ? rd32(m, addr) : rd16(m, addr), tg.format);
        color = blend_pixel(regs, color, dst);
    }
    wr_pixel(m, addr, encode_pixel(color, tg.format), tg.format);
    ++stats.pixels_written;
}

Rgba shade(const psprecomp::GuestMemory &m, const Texture *tex, Rgba vertex_color, float u,
           float v) {
    Rgba c = vertex_color;
    if (tex != nullptr) {
        const Rgba t = sample_texture(m, *tex, static_cast<int>(std::floor(u)),
                                      static_cast<int>(std::floor(v)));
        c.r = static_cast<std::uint8_t>(c.r * t.r / 255);
        c.g = static_cast<std::uint8_t>(c.g * t.g / 255);
        c.b = static_cast<std::uint8_t>(c.b * t.b / 255);
        c.a = static_cast<std::uint8_t>(c.a * t.a / 255);
    }
    return c;
}

void draw_sprite(psprecomp::GuestMemory &m, const Target &tg, const GeDrawCall &draw,
                 const Vertex &a, const Vertex &b, RasterStats &stats) {
    const Regs &regs = draw.regs;
    Texture tex;
    const Texture *texp = nullptr;
    if ((regs[kCmdTextureMapEnable] & 1u) != 0) {
        tex = texture_from_state(regs);
        if (tex.valid)
            texp = &tex;
    }
    const bool blend = (regs[kCmdAlphaBlendEnable] & 1u) != 0;
    const int x0 = static_cast<int>(std::ceil(std::min(a.x, b.x)));
    const int x1 = static_cast<int>(std::ceil(std::max(a.x, b.x)));
    const int y0 = static_cast<int>(std::ceil(std::min(a.y, b.y)));
    const int y1 = static_cast<int>(std::ceil(std::max(a.y, b.y)));
    for (int y = y0; y < y1; ++y) {
        const float ty = (b.y - a.y) != 0 ? (static_cast<float>(y) + 0.5f - a.y) / (b.y - a.y) : 0.0f;
        const float vcoord = a.v + (b.v - a.v) * ty;
        for (int x = x0; x < x1; ++x) {
            const float tx = (b.x - a.x) != 0 ? (static_cast<float>(x) + 0.5f - a.x) / (b.x - a.x) : 0.0f;
            const float ucoord = a.u + (b.u - a.u) * tx;
            const Rgba c = shade(m, texp, b.color, ucoord, vcoord);
            if (!alpha_test_passes(regs, c.a))
                continue;
            plot(m, tg, regs, x, y, c, blend, stats);
        }
    }
    ++stats.sprites;
}

void draw_triangle(psprecomp::GuestMemory &m, const Target &tg, const GeDrawCall &draw,
                   const Vertex &v0, const Vertex &v1, const Vertex &v2, RasterStats &stats) {
    const Regs &regs = draw.regs;
    Texture tex;
    const Texture *texp = nullptr;
    if ((regs[kCmdTextureMapEnable] & 1u) != 0) {
        tex = texture_from_state(regs);
        if (tex.valid)
            texp = &tex;
    }
    const bool blend = (regs[kCmdAlphaBlendEnable] & 1u) != 0;
    const float denom = (v1.y - v2.y) * (v0.x - v2.x) + (v2.x - v1.x) * (v0.y - v2.y);
    if (denom == 0.0f)
        return;
    const int minx = static_cast<int>(std::floor(std::min({v0.x, v1.x, v2.x})));
    const int maxx = static_cast<int>(std::ceil(std::max({v0.x, v1.x, v2.x})));
    const int miny = static_cast<int>(std::floor(std::min({v0.y, v1.y, v2.y})));
    const int maxy = static_cast<int>(std::ceil(std::max({v0.y, v1.y, v2.y})));
    for (int y = miny; y < maxy; ++y) {
        for (int x = minx; x < maxx; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            const float w0 = ((v1.y - v2.y) * (px - v2.x) + (v2.x - v1.x) * (py - v2.y)) / denom;
            const float w1 = ((v2.y - v0.y) * (px - v2.x) + (v0.x - v2.x) * (py - v2.y)) / denom;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0)
                continue;
            Rgba vc;
            vc.r = static_cast<std::uint8_t>(v0.color.r * w0 + v1.color.r * w1 + v2.color.r * w2);
            vc.g = static_cast<std::uint8_t>(v0.color.g * w0 + v1.color.g * w1 + v2.color.g * w2);
            vc.b = static_cast<std::uint8_t>(v0.color.b * w0 + v1.color.b * w1 + v2.color.b * w2);
            vc.a = static_cast<std::uint8_t>(v0.color.a * w0 + v1.color.a * w1 + v2.color.a * w2);
            const float u = v0.u * w0 + v1.u * w1 + v2.u * w2;
            const float vv = v0.v * w0 + v1.v * w1 + v2.v * w2;
            const Rgba c = shade(m, texp, vc, u, vv);
            if (!alpha_test_passes(regs, c.a))
                continue;
            plot(m, tg, regs, x, y, c, blend, stats);
        }
    }
    ++stats.triangles;
}

void do_transfer(psprecomp::GuestMemory &m, const GeDrawCall &draw, RasterStats &stats) {
    const Regs &regs = draw.regs;
    const std::uint32_t src = (regs[kCmdTransferSrc] & 0xFFFFF0u) |
                              ((regs[kCmdTransferSrcW] & 0xFF0000u) << 8);
    const std::uint32_t dst = (regs[kCmdTransferDst] & 0xFFFFF0u) |
                              ((regs[kCmdTransferDstW] & 0xFF0000u) << 8);
    std::uint32_t src_stride = regs[kCmdTransferSrcW] & 0x7F8u;
    if (src_stride > 0x400u)
        src_stride = 0;
    std::uint32_t dst_stride = regs[kCmdTransferDstW] & 0x7F8u;
    if (dst_stride > 0x400u)
        dst_stride = 0;
    const std::uint32_t sx = regs[kCmdTransferSrcPos] & 0x3FFu;
    const std::uint32_t sy = (regs[kCmdTransferSrcPos] >> 10) & 0x3FFu;
    const std::uint32_t dx = regs[kCmdTransferDstPos] & 0x3FFu;
    const std::uint32_t dy = (regs[kCmdTransferDstPos] >> 10) & 0x3FFu;
    const std::uint32_t w = (regs[kCmdTransferSize] & 0x3FFu) + 1u;
    const std::uint32_t h = ((regs[kCmdTransferSize] >> 10) & 0x3FFu) + 1u;
    const std::uint32_t bpp = (regs[kCmdTransferStart] & 1u) ? 4u : 2u;
    const std::uint32_t src_line = src_stride != 0 ? src_stride : w;
    const std::uint32_t dst_line = dst_stride != 0 ? dst_stride : w;
    for (std::uint32_t row = 0; row < h; ++row) {
        const std::uint32_t srow = src + ((sy + row) * src_line + sx) * bpp;
        const std::uint32_t drow = dst + ((dy + row) * dst_line + dx) * bpp;
        for (std::uint32_t i = 0; i < w * bpp; ++i) {
            if (m.contains(srow + i, 1) && m.contains(drow + i, 1))
                m.store8(drow + i, m.load8(srow + i));
        }
    }
    ++stats.transfers;
}

}  // namespace

void rasterize_draws(psprecomp::GuestMemory &memory, const std::vector<GeDrawCall> &draws,
                     std::size_t begin, std::uint32_t fb_pixel_format, std::uint32_t fb_height,
                     RasterStats &stats) {
    for (std::size_t di = begin; di < draws.size(); ++di) {
        const GeDrawCall &draw = draws[di];
        if (draw.is_transfer) {
            do_transfer(memory, draw, stats);
            continue;
        }
        Target tg;
        tg.addr = (draw.regs[kCmdFrameBufPtr] & 0xFFFFFFu) |
                  ((draw.regs[kCmdFrameBufWidth] & 0xFF0000u) << 8);
        const std::uint32_t width_reg = draw.regs[kCmdFrameBufWidth] & 0x7F8u;
        tg.stride = width_reg != 0 ? width_reg : 512u;
        tg.format = fb_pixel_format;
        tg.height = fb_height;
        if (tg.addr == 0) {
            ++stats.skipped_unsupported;
            continue;
        }
        const VertexLayout layout = vertex_layout(draw.vertex_type);
        if (!layout.valid) {
            ++stats.skipped_unsupported;
            continue;
        }
        if (!layout.through) {
            ++stats.skipped_non_through;
            continue;
        }
        if (draw.vertex_address == 0 || draw.vertex_count == 0) {
            ++stats.skipped_unsupported;
            continue;
        }
        const auto fetch = [&](std::uint32_t i) {
            const std::uint32_t idx = vertex_index(memory, draw, layout, i);
            return decode_vertex(memory, draw.vertex_address + idx * layout.stride, layout,
                                 draw.regs);
        };
        switch (draw.prim_type) {
            case kPrimSprites:
                for (std::uint32_t i = 0; i + 1 < draw.vertex_count; i += 2)
                    draw_sprite(memory, tg, draw, fetch(i), fetch(i + 1), stats);
                break;
            case kPrimTriangles:
                for (std::uint32_t i = 0; i + 2 < draw.vertex_count; i += 3)
                    draw_triangle(memory, tg, draw, fetch(i), fetch(i + 1), fetch(i + 2),
                                  stats);
                break;
            case kPrimTriangleStrip:
                for (std::uint32_t i = 0; i + 2 < draw.vertex_count; ++i)
                    draw_triangle(memory, tg, draw, fetch(i), fetch(i + 1), fetch(i + 2),
                                  stats);
                break;
            case kPrimTriangleFan:
                for (std::uint32_t i = 1; i + 1 < draw.vertex_count; ++i)
                    draw_triangle(memory, tg, draw, fetch(0), fetch(i), fetch(i + 1), stats);
                break;
            case kPrimPoints: {
                const bool blend = (draw.regs[kCmdAlphaBlendEnable] & 1u) != 0;
                for (std::uint32_t i = 0; i < draw.vertex_count; ++i) {
                    const Vertex v = fetch(i);
                    if (alpha_test_passes(draw.regs, v.color.a))
                        plot(memory, tg, draw.regs, static_cast<int>(v.x),
                             static_cast<int>(v.y), v.color, blend, stats);
                }
                ++stats.points;
                break;
            }
            default:
                ++stats.skipped_unsupported;  // lines: later if a game needs them
                break;
        }
    }
}

}  // namespace p3p::ge
