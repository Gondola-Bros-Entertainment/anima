#include "bc7.hpp"
#include "surface_validation.hpp"
#include <algorithm>
#include <anima/assets/asset.hpp>
#include <stdexcept>
#include <utility>

namespace anima {
namespace {
// Fields of each BC7 mode, from the Khronos Data Format Specification 1.3's table of mode-dependent BPTC
// parameters: subsets, then the bits of the partition, rotation and index selection fields, of each color and
// alpha endpoint channel, the per-endpoint and shared P-bits, and the primary and secondary indices.
struct Mode {
    unsigned subsets, partition_bits, rotation_bits, selection_bits, color_bits, alpha_bits, endpoint_pbits,
        shared_pbits, index_bits, secondary_index_bits;
};
constexpr std::array<Mode, 8> modes{{
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0},
    {2, 6, 0, 0, 6, 0, 0, 1, 3, 0},
    {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
    {1, 0, 2, 1, 5, 6, 0, 0, 2, 3},
    {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0},
    {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
}};
// The specification's partition tables for two and three subsets: texel `x + 4 * y` of partition `p` belongs to
// subset `(table[p] >> (2 * (x + 4 * y))) & 3`.
constexpr std::array<std::uint32_t, 64> two_subsets{
    0x50505050, 0x40404040, 0x54545454, 0x54505040, 0x50404000, 0x55545450, 0x55545040, 0x54504000,
    0x50400000, 0x55555450, 0x55544000, 0x54400000, 0x55555440, 0x55550000, 0x55555500, 0x55000000,
    0x55150100, 0x00004054, 0x15010000, 0x00405054, 0x00004050, 0x15050100, 0x05010000, 0x40505054,
    0x00404050, 0x05010100, 0x14141414, 0x05141450, 0x01155440, 0x00555500, 0x15014054, 0x05414150,
    0x44444444, 0x55005500, 0x11441144, 0x05055050, 0x05500550, 0x11114444, 0x41144114, 0x44111144,
    0x15055054, 0x01055040, 0x05041050, 0x05455150, 0x14414114, 0x50050550, 0x41411414, 0x00141400,
    0x00041504, 0x00105410, 0x10541000, 0x04150400, 0x50410514, 0x41051450, 0x05415014, 0x14054150,
    0x41050514, 0x41505014, 0x40011554, 0x54150140, 0x50505500, 0x00555050, 0x15151010, 0x54540404,
};
constexpr std::array<std::uint32_t, 64> three_subsets{
    0xAA685050, 0x6A5A5040, 0x5A5A4200, 0x5450A0A8, 0xA5A50000, 0xA0A05050, 0x5555A0A0, 0x5A5A5050,
    0xAA550000, 0xAA555500, 0xAAAA5500, 0x90909090, 0x94949494, 0xA4A4A4A4, 0xA9A59450, 0x2A0A4250,
    0xA5945040, 0x0A425054, 0xA5A5A500, 0x55A0A0A0, 0xA8A85454, 0x6A6A4040, 0xA4A45000, 0x1A1A0500,
    0x0050A4A4, 0xAAA59090, 0x14696914, 0x69691400, 0xA08585A0, 0xAA821414, 0x50A4A450, 0x6A5A0200,
    0xA9A58000, 0x5090A0A8, 0xA8A09050, 0x24242424, 0x00AA5500, 0x24924924, 0x24499224, 0x50A50A50,
    0x500AA550, 0xAAAA4444, 0x66660000, 0xA5A0A5A0, 0x50A050A0, 0x69286928, 0x44AAAA44, 0x66666600,
    0xAA444444, 0x54A854A8, 0x95809580, 0x96969600, 0xA85454A8, 0x80959580, 0xAA141414, 0x96960000,
    0xAAAA1414, 0xA05050A0, 0xA0A5A5A0, 0x96000000, 0x40804080, 0xA9A8A9A8, 0xAAAAAA44, 0x2A4A5254,
};
// The specification's anchor index tables: the texel of each subset after the first whose index stores one bit
// fewer. Texel 0 is always the first subset's anchor.
constexpr std::array<std::uint8_t, 64> second_anchor_of_two{
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8, 2,  2, 8,
    8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2, 8,  2, 2,
    2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2, 15,
};
constexpr std::array<std::uint8_t, 64> second_anchor_of_three{
    3, 3,  15, 15, 8, 3,  15, 15, 8,  8, 6,  6, 6,  5,  3,  3,  3, 3,  8, 15, 3, 3, 6, 10, 5, 8,  8, 6,  8,  5,  15, 15,
    8, 15, 3,  5,  6, 10, 8,  15, 15, 3, 15, 5, 15, 15, 15, 15, 3, 15, 5, 5,  5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3,  3,
};
constexpr std::array<std::uint8_t, 64> third_anchor_of_three{
    15, 8, 8, 3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  15, 8,  15, 3,  15, 8,
    15, 8, 3, 15, 6,  10, 15, 15, 10, 8,  15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15,
    3,  6, 6, 8,  15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8,
};
// Interpolation weights of 2-, 3- and 4-bit indices, out of 64.
constexpr std::array<std::uint8_t, 4> weights2{0, 21, 43, 64};
constexpr std::array<std::uint8_t, 8> weights3{0, 9, 18, 27, 37, 46, 55, 64};
constexpr std::array<std::uint8_t, 16> weights4{0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
unsigned weight(unsigned bits, unsigned index) {
    return bits == 2 ? weights2[index] : bits == 3 ? weights3[index] : weights4[index];
}
// The specification's endpoint interpolation, in 8-bit unsigned integers.
std::uint8_t interpolate(unsigned from, unsigned to, unsigned factor) {
    return static_cast<std::uint8_t>(((64 - factor) * from + factor * to + 32) >> 6);
}
// @p value of @p bits (5 to 8) as 8 bits, its top bits replicated into the bits below.
unsigned expand(unsigned value, unsigned bits) { return (value << (8 - bits)) | (value >> (2 * bits - 8)); }
// Reads a block's fields from its least significant bit up.
class BitReader {
  public:
    explicit BitReader(const std::uint8_t *block) {
        for (unsigned i = 0; i < 8; ++i) {
            low_ |= std::uint64_t{block[i]} << (8 * i);
            high_ |= std::uint64_t{block[8 + i]} << (8 * i);
        }
    }
    // The next @p count bits, at most 8.
    unsigned read(unsigned count) {
        if (!count)
            return 0;
        std::uint64_t value = 0;
        if (position_ >= 64)
            value = high_ >> (position_ - 64);
        else if (position_ == 0)
            value = low_;
        else
            value = (low_ >> position_) | (high_ << (64 - position_));
        position_ += count;
        return static_cast<unsigned>(value & ((std::uint64_t{1} << count) - 1));
    }

  private:
    std::uint64_t low_{}, high_{};
    unsigned position_{};
};
} // namespace

std::array<std::uint8_t, 64> detail::decode_bc7_block(const std::uint8_t *block) {
    std::array<std::uint8_t, 64> texels{};
    unsigned mode_number = 0;
    while (mode_number < modes.size() && !(block[0] & (1U << mode_number)))
        ++mode_number;
    if (mode_number == modes.size())
        return texels; // The reserved mode 8.
    const auto &mode = modes[mode_number];
    BitReader bits(block);
    (void)bits.read(mode_number + 1);
    const auto partition = bits.read(mode.partition_bits);
    const auto rotation = bits.read(mode.rotation_bits);
    const auto selection = bits.read(mode.selection_bits);
    // Endpoint `2 * subset + end`, grouped by channel, then by subset, then by end.
    const auto endpoints_count = 2 * mode.subsets;
    std::array<std::array<unsigned, 4>, 6> endpoints{};
    for (unsigned channel = 0; channel < 3; ++channel)
        for (unsigned endpoint = 0; endpoint < endpoints_count; ++endpoint)
            endpoints[endpoint][channel] = bits.read(mode.color_bits);
    if (mode.alpha_bits)
        for (unsigned endpoint = 0; endpoint < endpoints_count; ++endpoint)
            endpoints[endpoint][3] = bits.read(mode.alpha_bits);
    auto color_precision = mode.color_bits, alpha_precision = mode.alpha_bits;
    // A P-bit becomes each channel's lowest bit: one per endpoint, or one per subset shared by its two endpoints.
    const auto append = [&](unsigned endpoint, unsigned bit) {
        for (unsigned channel = 0; channel < (mode.alpha_bits ? 4U : 3U); ++channel)
            endpoints[endpoint][channel] = (endpoints[endpoint][channel] << 1) | bit;
    };
    if (mode.endpoint_pbits) {
        for (unsigned endpoint = 0; endpoint < endpoints_count; ++endpoint)
            append(endpoint, bits.read(1));
        ++color_precision;
        if (mode.alpha_bits)
            ++alpha_precision;
    } else if (mode.shared_pbits) {
        for (unsigned subset = 0; subset < mode.subsets; ++subset) {
            const auto bit = bits.read(1);
            append(2 * subset, bit);
            append(2 * subset + 1, bit);
        }
        ++color_precision;
    }
    for (unsigned endpoint = 0; endpoint < endpoints_count; ++endpoint) {
        for (unsigned channel = 0; channel < 3; ++channel)
            endpoints[endpoint][channel] = expand(endpoints[endpoint][channel], color_precision);
        endpoints[endpoint][3] = mode.alpha_bits ? expand(endpoints[endpoint][3], alpha_precision) : 255;
    }
    const auto subset_of = [&](unsigned texel) -> unsigned {
        if (mode.subsets == 2)
            return (two_subsets[partition] >> (2 * texel)) & 3;
        if (mode.subsets == 3)
            return (three_subsets[partition] >> (2 * texel)) & 3;
        return 0;
    };
    const auto anchor = [&](unsigned texel) {
        if (texel == 0)
            return true;
        if (mode.subsets == 2)
            return texel == second_anchor_of_two[partition];
        if (mode.subsets == 3)
            return texel == second_anchor_of_three[partition] || texel == third_anchor_of_three[partition];
        return false;
    };
    // An anchor's index stores one bit fewer; its highest bit is zero.
    std::array<unsigned, 16> primary{}, secondary{};
    for (unsigned texel = 0; texel < primary.size(); ++texel)
        primary[texel] = bits.read(mode.index_bits - (anchor(texel) ? 1 : 0));
    if (mode.secondary_index_bits)
        for (unsigned texel = 0; texel < secondary.size(); ++texel)
            secondary[texel] = bits.read(mode.secondary_index_bits - (texel == 0 ? 1 : 0));
    for (unsigned texel = 0; texel < 16; ++texel) {
        const auto subset = subset_of(texel);
        const auto &from = endpoints[2 * subset];
        const auto &to = endpoints[2 * subset + 1];
        // Color and alpha interpolate by the primary indices, except that with secondary indices one of them uses
        // those: color when the index selection bit is set, alpha otherwise.
        auto color_index = primary[texel], alpha_index = primary[texel];
        auto color_bits = mode.index_bits, alpha_bits = mode.index_bits;
        if (mode.secondary_index_bits) {
            if (selection) {
                color_index = secondary[texel];
                color_bits = mode.secondary_index_bits;
            } else {
                alpha_index = secondary[texel];
                alpha_bits = mode.secondary_index_bits;
            }
        }
        auto *out = &texels[4 * texel];
        for (unsigned channel = 0; channel < 3; ++channel)
            out[channel] = interpolate(from[channel], to[channel], weight(color_bits, color_index));
        out[3] = interpolate(from[3], to[3], weight(alpha_bits, alpha_index));
        // Rotation swaps alpha with red, green or blue.
        if (rotation)
            std::swap(out[3], out[rotation - 1]);
    }
    return texels;
}
std::uint32_t detail::full_mip_levels(std::uint32_t width, std::uint32_t height) {
    std::uint32_t levels = 1;
    for (auto edge = std::max(width, height); edge > 1; edge /= 2)
        ++levels;
    return levels;
}
std::size_t detail::bc7_level_bytes(std::uint32_t width, std::uint32_t height) {
    return (std::size_t{width} + 3) / 4 * ((std::size_t{height} + 3) / 4) * bc7_block_bytes;
}
std::size_t detail::bc7_image_bytes(std::uint32_t width, std::uint32_t height, std::uint32_t levels) {
    std::size_t bytes = 0;
    for (std::uint32_t level = 0; level < levels; ++level)
        bytes += bc7_level_bytes(std::max(width >> level, 1U), std::max(height >> level, 1U));
    return bytes;
}

std::vector<MipLevel> decode_image(const Image &image) {
    (void)detail::validate_image(&image, detail::Texels::required);
    if (image.format == ImageFormat::rgba8)
        return {{image.width, image.height, image.rgba}};
    std::vector<MipLevel> result;
    const auto *block = image.blocks.data();
    for (std::uint32_t level = 0; level < image.levels; ++level) {
        MipLevel decoded{std::max(image.width >> level, 1U), std::max(image.height >> level, 1U), {}};
        decoded.rgba.resize(std::size_t{decoded.width} * decoded.height * 4);
        // Blocks run row by row; texels past the level's edge in its last blocks are dropped.
        for (std::uint32_t top = 0; top < decoded.height; top += 4)
            for (std::uint32_t left = 0; left < decoded.width; left += 4, block += detail::bc7_block_bytes) {
                const auto texels = detail::decode_bc7_block(block);
                for (auto y = top; y < std::min(top + 4, decoded.height); ++y)
                    for (auto x = left; x < std::min(left + 4, decoded.width); ++x)
                        std::copy_n(&texels[((y - top) * 4 + (x - left)) * 4], 4,
                                    &decoded.rgba[(std::size_t{y} * decoded.width + x) * 4]);
            }
        result.push_back(std::move(decoded));
    }
    return result;
}
} // namespace anima
