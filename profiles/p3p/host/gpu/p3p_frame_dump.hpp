// P3P profile framebuffer dump (Gate 6 WP2): writes a guest
// framebuffer as a PNG file. The encoder is dependency-free (zlib
// "stored" blocks), so it works on the minimal VPS toolchain.

#pragma once

#include "psprecomp/guest_memory.hpp"

#include <cstdint>
#include <string>

namespace p3p {

// Converts the framebuffer at `addr` (rows of `stride_px` pixels,
// `width` x `height` visible, PSP pixel format 0=565, 1=5551, 2=4444,
// 3=8888) to an RGBA PNG at `path`. Returns false on any failure.
bool write_framebuffer_png(psprecomp::GuestMemory &memory, std::uint32_t addr,
                           std::uint32_t stride_px, std::uint32_t width, std::uint32_t height,
                           std::uint32_t format, const std::string &path);

}  // namespace p3p
