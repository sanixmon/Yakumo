// P3P profile software 2D rasterizer (Gate 6 WP2).
//
// Consumes the draw calls recorded by the GE executor and renders them
// into guest memory (framebuffer in VRAM or RAM): through-mode sprites
// and triangles with optional texturing (direct-color formats and
// CLUT4/8), alpha blending, alpha testing, and VRAM block transfers.
// Non-through (transformed) draws are counted and skipped — 3D is
// Gate 8. Semantics follow PPSSPP (GPU/ge_constants.h, GPUState.h,
// VertexDecoderCommon): through s16 positions and the OFFSETX/Y
// registers are 12.4 fixed point; buffer addresses resolve as
// (arg & 0xFFFFFF) | ((width arg & 0xFF0000) << 8).

#pragma once

#include "gpu/p3p_ge_executor.hpp"
#include "psprecomp/guest_memory.hpp"

#include <cstdint>
#include <vector>

namespace p3p::ge {

struct RasterStats {
    std::uint32_t sprites = 0;
    std::uint32_t triangles = 0;
    std::uint32_t points = 0;
    std::uint32_t transfers = 0;
    std::uint32_t skipped_non_through = 0;
    std::uint32_t skipped_unsupported = 0;
    std::uint32_t pixels_written = 0;
};

// Renders draws[begin..] in order. fb_pixel_format is the PSP display
// pixel format (0=565, 1=5551, 2=4444, 3=8888) used for the draw
// target; the GE itself has no framebuffer-format register. Draws are
// clipped to fb_height rows (the display height) when nonzero.
void rasterize_draws(psprecomp::GuestMemory &memory, const std::vector<GeDrawCall> &draws,
                     std::size_t begin, std::uint32_t fb_pixel_format, std::uint32_t fb_height,
                     RasterStats &stats);

}  // namespace p3p::ge
