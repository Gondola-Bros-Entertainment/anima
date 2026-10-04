#include "../detail/image_limits.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
// stb reads its bound from a macro before its header, so the shared limit is restated here and checked below.
#define STBI_MAX_DIMENSIONS 8192
#include <stb_image.h>

static_assert(std::uint32_t{STBI_MAX_DIMENSIONS} == anima::detail::maximum_image_edge,
              "The decoder's dimension bound must be the shared image edge limit");
static_assert(std::size_t{STBI_rgb_alpha} == anima::detail::rgba_bytes_per_pixel,
              "Decoding to STBI_rgb_alpha must give the shared RGBA texel size");
