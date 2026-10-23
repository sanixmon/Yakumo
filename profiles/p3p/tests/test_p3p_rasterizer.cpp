// Unit tests for the P3P Gate 6 WP2 software rasterizer: synthetic
// draw calls rendered into a guest framebuffer — untextured sprites,
// CLUT8 textured sprites, alpha blending, triangles, block transfers,
// and the non-through skip.

#include "gpu/p3p_rasterizer.hpp"

#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <cstdio>
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

using namespace p3p::ge;

constexpr std::uint32_t kFb = 0x04000000u;       // VRAM
constexpr std::uint32_t kVerts = 0x08810000u;    // RAM
constexpr std::uint32_t kTex = 0x08820000u;
constexpr std::uint32_t kClut = 0x08830000u;

GeDrawCall base_draw() {
    GeDrawCall d;
    d.regs[0x9C] = 0;                    // fb low bits (0x04000000 low 24 = 0)
    d.regs[0x9D] = (0x04u << 16) | 512u; // fb high bits + buffer width 512
    return d;
}

void put16(psprecomp::GuestMemory &m, std::uint32_t a, std::uint16_t v) {
    m.store8(a, static_cast<std::uint8_t>(v));
    m.store8(a + 1, static_cast<std::uint8_t>(v >> 8));
}
void put32(psprecomp::GuestMemory &m, std::uint32_t a, std::uint32_t v) {
    m.store32(a, v);
}
std::uint32_t fb_pixel(psprecomp::GuestMemory &m, int x, int y) {
    return m.load32(kFb + (static_cast<std::uint32_t>(y) * 512u + static_cast<std::uint32_t>(x)) * 4u);
}

// Vertex writers for the layouts used below (through mode).
void vert_color_pos(psprecomp::GuestMemory &m, std::uint32_t a, std::uint32_t rgba,
                    std::int16_t x, std::int16_t y) {
    put32(m, a, rgba);
    put16(m, a + 4, static_cast<std::uint16_t>(x));
    put16(m, a + 6, static_cast<std::uint16_t>(y));
    put16(m, a + 8, 0);
}
void vert_uv_color_pos(psprecomp::GuestMemory &m, std::uint32_t a, std::uint16_t u,
                       std::uint16_t v, std::uint32_t rgba, std::int16_t x, std::int16_t y) {
    put16(m, a, u);
    put16(m, a + 2, v);
    put32(m, a + 4, rgba);
    put16(m, a + 8, static_cast<std::uint16_t>(x));
    put16(m, a + 10, static_cast<std::uint16_t>(y));
    put16(m, a + 12, 0);
}

constexpr std::uint32_t kRed = 0xFF0000FFu;    // R,G,B,A bytes -> u32 LE: R low
constexpr std::uint32_t kBlue = 0xFFFF0000u;

}  // namespace

int main() {
    psprecomp::Runtime runtime(32u * 1024u * 1024u);
    auto &mem = runtime.memory();
    RasterStats stats;

    // Case A: untextured red sprite, 10x5 px at (0,0).
    {
        vert_color_pos(mem, kVerts, kRed, 0, 0);
        vert_color_pos(mem, kVerts + 10, kRed, 160, 80);
        GeDrawCall d = base_draw();
        d.prim_type = kPrimSprites;
        d.vertex_count = 2;
        d.vertex_address = kVerts;
        d.vertex_type = (4u << 2) | (2u << 7) | (1u << 23);  // color 8888, pos s16, through
        rasterize_draws(mem, {d}, 0, 3, 272, stats);
        check(fb_pixel(mem, 0, 0) == kRed, "sprite fills its top-left");
        check(fb_pixel(mem, 9, 4) == kRed, "sprite fills its bottom-right");
        check(fb_pixel(mem, 10, 0) == 0u, "sprite stops at its width");
        check(fb_pixel(mem, 0, 5) == 0u, "sprite stops at its height");
        check(stats.sprites == 1u && stats.pixels_written == 50u, "sprite stats");
    }

    // Case B: CLUT8 textured sprite 4x4, uv (0,0)-(4,4) over (0,0)-(4,4).
    {
        for (int i = 0; i < 16; ++i)
            mem.store8(kTex + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(i));
        for (int i = 0; i < 16; ++i) {
            const std::uint32_t rgba = static_cast<std::uint32_t>(i * 16) |
                                       (static_cast<std::uint32_t>(255 - i * 16) << 8) |
                                       (128u << 16) | (255u << 24);
            put32(mem, kClut + static_cast<std::uint32_t>(i) * 4u, rgba);
        }
        vert_uv_color_pos(mem, kVerts, 0, 0, 0xFFFFFFFFu, 0, 0);
        vert_uv_color_pos(mem, kVerts + 14, 128, 128, 0xFFFFFFFFu, 64, 64);
        GeDrawCall d = base_draw();
        d.prim_type = kPrimSprites;
        d.vertex_count = 2;
        d.vertex_address = kVerts;
        d.vertex_type = 2u | (4u << 2) | (2u << 7) | (1u << 23);  // tc u16 + color + pos
        d.regs[0x1E] = 1;                       // texture map enable
        d.regs[0xA0] = kTex & 0xFFFFFFu;        // tex addr low
        d.regs[0xA8] = ((kTex >> 8) & 0xFF0000u) | 4u;  // tex bufwidth 4 + addr high
        d.regs[0xB8] = 2u | (2u << 8);          // tex size 4x4
        d.regs[0xC3] = 5;                       // CLUT8
        d.regs[0xB0] = kClut & 0xFFFFFFu;       // clut addr low
        d.regs[0xB1] = (kClut >> 24) & 0xFFu;   // clut addr high
        d.regs[0xC5] = 3u | (0xFFu << 8);       // clut format 8888, mask 0xFF
        rasterize_draws(mem, {d}, 0, 3, 272, stats);
        const std::uint32_t expect00 = (0u) | (255u << 8) | (128u << 16) | (255u << 24);
        const std::uint32_t expect33 = (240u) | (15u << 8) | (128u << 16) | (255u << 24);
        const std::uint32_t expect10 = (16u) | (239u << 8) | (128u << 16) | (255u << 24);
        check(fb_pixel(mem, 0, 0) == expect00, "CLUT8 texel (0,0) sampled");
        check(fb_pixel(mem, 3, 3) == expect33, "CLUT8 texel (3,3) sampled");
        check(fb_pixel(mem, 1, 0) == expect10, "CLUT8 texel (1,0) sampled");
    }

    // Case C: block transfer RAM -> VRAM, bpp 2, 4x2, stride 8.
    {
        const std::uint32_t src = 0x08840000u;
        const std::uint32_t dst = 0x04010000u;
        for (int i = 0; i < 32; ++i)
            mem.store8(src + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(0xA0 + i));
        GeDrawCall d;
        d.is_transfer = true;
        d.regs[0xB2] = src & 0xFFFFF0u;
        d.regs[0xB3] = ((src >> 8) & 0xFF0000u) | 8u;
        d.regs[0xB4] = dst & 0xFFFFF0u;
        d.regs[0xB5] = ((dst >> 8) & 0xFF0000u) | 8u;
        d.regs[0xEE] = 3u | (1u << 10);  // w=4, h=2
        d.regs[0xEA] = 0;                // bpp 2
        rasterize_draws(mem, {d}, 0, 3, 272, stats);
        check(mem.load8(dst) == 0xA0 && mem.load8(dst + 7) == 0xA7, "transfer row 0 copied");
        check(mem.load8(dst + 16) == 0xB0 && mem.load8(dst + 23) == 0xB7,
              "transfer row 1 honors strides");
        check(stats.transfers == 1u, "transfer counted");
    }

    // Case D: untextured triangle (0,0),(10,0),(0,10) screen px.
    {
        vert_color_pos(mem, kVerts, kBlue, 0, 0);
        vert_color_pos(mem, kVerts + 10, kBlue, 160, 0);
        vert_color_pos(mem, kVerts + 20, kBlue, 0, 160);
        GeDrawCall d = base_draw();
        d.prim_type = kPrimTriangles;
        d.vertex_count = 3;
        d.vertex_address = kVerts;
        d.vertex_type = (4u << 2) | (2u << 7) | (1u << 23);
        rasterize_draws(mem, {d}, 0, 3, 272, stats);
        check(fb_pixel(mem, 1, 1) == kBlue, "triangle interior filled");
        check(fb_pixel(mem, 9, 9) == 0u, "triangle exterior untouched");
        check(stats.triangles == 1u, "triangle counted");
    }

    // Case E: alpha blend — 50% white over a fresh opaque red sprite.
    {
        vert_color_pos(mem, kVerts, kRed, 0, 0);
        vert_color_pos(mem, kVerts + 10, kRed, 160, 80);
        GeDrawCall base = base_draw();
        base.prim_type = kPrimSprites;
        base.vertex_count = 2;
        base.vertex_address = kVerts;
        base.vertex_type = (4u << 2) | (2u << 7) | (1u << 23);
        rasterize_draws(mem, {base}, 0, 3, 272, stats);
        const std::uint32_t white50 = 0x80FFFFFFu;
        vert_color_pos(mem, kVerts, white50, 0, 0);
        vert_color_pos(mem, kVerts + 10, white50, 160, 80);
        GeDrawCall d = base_draw();
        d.prim_type = kPrimSprites;
        d.vertex_count = 2;
        d.vertex_address = kVerts;
        d.vertex_type = (4u << 2) | (2u << 7) | (1u << 23);
        d.regs[0x21] = 1;                        // alpha blend enable
        d.regs[0xDF] = (2u << 8) | (3u << 11);   // add, srcAlpha, 1-srcAlpha
        rasterize_draws(mem, {d}, 0, 3, 272, stats);
        const std::uint32_t px = fb_pixel(mem, 0, 0);
        const int r = static_cast<int>(px & 0xFFu);
        const int g = static_cast<int>((px >> 8) & 0xFFu);
        const int b = static_cast<int>((px >> 16) & 0xFFu);
        check(r == 255 && g >= 127 && g <= 129 && b >= 127 && b <= 129,
              "alpha blend mixes src over dst");
    }

    // Case F: non-through draw is skipped, not rendered.
    {
        GeDrawCall d = base_draw();
        d.prim_type = kPrimSprites;
        d.vertex_count = 2;
        d.vertex_address = kVerts;
        d.vertex_type = (4u << 2) | (2u << 7);  // no through bit
        const std::uint32_t skipped_before = stats.skipped_non_through;
        rasterize_draws(mem, {d}, 0, 3, 272, stats);
        check(stats.skipped_non_through == skipped_before + 1u,
              "non-through draw counted as skipped");
    }

    std::fprintf(stderr, "test_p3p_rasterizer: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
