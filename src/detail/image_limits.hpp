#pragma once
#include <cstddef>
#include <cstdint>

namespace anima::detail {
// Image limits for the shared stb decoder (src/assets/image.cpp) and the UI's textures, which build without the assets
// module. include/anima/assets/asset.hpp publishes the first two as max_image_edge and max_image_texels, which the GLB
// importer and the KTX2 reader apply, and the importer asserts that both agree with these.
inline constexpr std::uint32_t maximum_image_edge = 8192;
inline constexpr std::size_t maximum_image_texels = std::size_t{16} * 1024 * 1024;
// Bytes of one texel of the 8-bit RGBA that the decoder produces and UI textures hold.
inline constexpr std::size_t rgba_bytes_per_pixel = 4;
} // namespace anima::detail
