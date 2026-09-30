#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace anima::detail {
// One BC7 block: bc7_block_edge by bc7_block_edge texels in bc7_block_bytes.
inline constexpr std::uint32_t bc7_block_edge = 4;
inline constexpr std::size_t bc7_block_bytes = 16, bc7_block_texels = std::size_t{bc7_block_edge} * bc7_block_edge;
// A decoded texel, 8-bit RGBA with alpha last, and a decoded block.
inline constexpr std::size_t rgba_texel_bytes = 4, rgba_alpha = 3,
                             bc7_decoded_bytes = bc7_block_texels * rgba_texel_bytes;
// The texels of @p block as 8-bit RGBA, row by row, decoded as the Khronos Data Format Specification's BPTC section
// defines; a block of the reserved mode 8 decodes to zero in every channel.
[[nodiscard]] std::array<std::uint8_t, bc7_decoded_bytes>
decode_bc7_block(std::span<const std::uint8_t, bc7_block_bytes> block);
// Mip levels from @p width by @p height down to 1x1, each half the one before, rounding down to at least 1.
[[nodiscard]] std::uint32_t full_mip_levels(std::uint32_t width, std::uint32_t height);
// Bytes of the BC7 blocks that cover @p width by @p height texels.
[[nodiscard]] std::size_t bc7_level_bytes(std::uint32_t width, std::uint32_t height);
// Bytes of @p levels BC7 levels, the first @p width by @p height.
[[nodiscard]] std::size_t bc7_image_bytes(std::uint32_t width, std::uint32_t height, std::uint32_t levels);
} // namespace anima::detail
