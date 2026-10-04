#include "srgb.hpp"
#include <anima/assets/asset.hpp>
#include <limits>
#include <stdexcept>

namespace anima {
namespace {
constexpr unsigned alpha_channel = 3;
constexpr unsigned channel_count = 4;
constexpr unsigned channel_max = std::numeric_limits<std::uint8_t>::max();
using AlphaHistogram = std::array<std::size_t, channel_max + 1>;

std::uint8_t byte(double value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, double(channel_max))));
}
AlphaHistogram alpha_histogram(const MipLevel &level) {
    AlphaHistogram histogram{};
    for (std::size_t i = alpha_channel; i < level.rgba.size(); i += channel_count)
        ++histogram[level.rgba[i]];
    return histogram;
}
std::size_t covered_texels(const AlphaHistogram &histogram, float cutoff, double scale = 1) {
    std::size_t covered = 0;
    for (unsigned alpha = 0; alpha <= channel_max; ++alpha)
        if (float(byte(alpha * scale)) / channel_max >= cutoff)
            covered += histogram[alpha];
    return covered;
}
double coverage_scale(const AlphaHistogram &histogram, float cutoff, double target_texels) {
    double best_scale = 1;
    double best_error = std::abs(double(covered_texels(histogram, cutoff)) - target_texels);
    const auto consider = [&](double scale) {
        const auto error = std::abs(double(covered_texels(histogram, cutoff, scale)) - target_texels);
        if (error < best_error || (error == best_error && std::abs(scale - 1) < std::abs(best_scale - 1))) {
            best_error = error;
            best_scale = scale;
        }
    };
    consider(0);
    unsigned first_visible = 1;
    while (float(first_visible) / channel_max < cutoff)
        ++first_visible;
    // RGBA8 has only 255 positive alpha values. Test both sides of each occupied
    // bucket's crossing: reduction is closest to one just below a crossing,
    // expansion just above. Prefer no change when coverage errors tie.
    for (unsigned alpha = 1; alpha <= channel_max; ++alpha)
        if (histogram[alpha]) {
            const auto crossing = (first_visible - 0.5) / alpha;
            for (const bool visible : {false, true}) {
                const auto direction = visible ? std::numeric_limits<double>::infinity() : 0.0;
                auto scale = crossing;
                // Division/multiplication can round back onto the half-byte
                // boundary; step until the stored alpha is on the intended side.
                do {
                    scale = std::nextafter(scale, direction);
                } while ((byte(alpha * scale) >= first_visible) != visible);
                consider(scale);
            }
        }
    return best_scale;
}
MipLevel downsample(const MipLevel &previous, TextureEncoding encoding, bool weight_colour_by_alpha) {
    MipLevel next{std::max(1U, previous.width / 2), std::max(1U, previous.height / 2), {}};
    next.rgba.resize(std::size_t(next.width) * next.height * channel_count);
    for (std::uint32_t y = 0; y < next.height; ++y)
        for (std::uint32_t x = 0; x < next.width; ++x) {
            // Include odd edge texels; widen products before coordinate division.
            const auto x0 = std::uint64_t(x) * previous.width / next.width;
            const auto x1 = (std::uint64_t(x) + 1) * previous.width / next.width;
            const auto y0 = std::uint64_t(y) * previous.height / next.height;
            const auto y1 = (std::uint64_t(y) + 1) * previous.height / next.height;
            std::array<double, channel_count> sums{};
            // Unweighted colour, which a texel whose sources all have zero alpha takes, so that colour spread into
            // transparent texels still reaches the edges that filtering blends with them.
            std::array<double, alpha_channel> plain{};
            double colour_weight = 0;
            for (auto sy = y0; sy < y1; ++sy)
                for (auto sx = x0; sx < x1; ++sx) {
                    const auto offset = (std::size_t(sy) * previous.width + sx) * channel_count;
                    const auto alpha = previous.rgba[offset + alpha_channel] / double(channel_max);
                    const auto weight = weight_colour_by_alpha ? alpha : 1.0;
                    colour_weight += weight;
                    for (unsigned c = 0; c < alpha_channel; ++c) {
                        auto value = previous.rgba[offset + c] / double(channel_max);
                        value = encoding == TextureEncoding::srgb ? detail::decode_srgb(value) : value;
                        sums[c] += value * weight;
                        plain[c] += value;
                    }
                    sums[alpha_channel] += alpha;
                }
            const auto texels = double((x1 - x0) * (y1 - y0));
            for (unsigned c = 0; c < channel_count; ++c) {
                const auto weight = c == alpha_channel ? texels : colour_weight;
                auto average = weight > 0 ? sums[c] / weight : plain[c] / texels;
                if (c < alpha_channel && encoding == TextureEncoding::srgb)
                    average = detail::encode_srgb(average);
                next.rgba[(std::size_t(y) * next.width + x) * channel_count + c] = byte(average * channel_max);
            }
        }
    return next;
}
} // namespace

std::vector<MipLevel> texture_mips(const Texture &texture, TextureMipOptions options) {
    // A texture without an image has no dimensions.
    const auto *image = texture.image.get();
    if (image && image->format != ImageFormat::rgba8)
        throw std::invalid_argument("Mip filtering requires an RGBA8 image");
    if (!image || !image->width || !image->height ||
        image->width > std::numeric_limits<std::size_t>::max() / channel_count / image->height ||
        image->rgba.size() != std::size_t(image->width) * image->height * channel_count)
        throw std::invalid_argument("Invalid texture dimensions or byte count");
    if (texture.encoding != TextureEncoding::srgb && texture.encoding != TextureEncoding::linear)
        throw std::invalid_argument("Invalid texture encoding");
    if (options.alpha_coverage_cutoff && (!std::isfinite(*options.alpha_coverage_cutoff) ||
                                          *options.alpha_coverage_cutoff <= 0 || *options.alpha_coverage_cutoff > 1))
        throw std::invalid_argument("Alpha coverage cutoff must be finite and in (0, 1]");
    std::vector<MipLevel> result{{image->width, image->height, image->rgba}};
    while (result.back().width > 1 || result.back().height > 1)
        result.push_back(downsample(result.back(), texture.encoding,
                                    options.alpha_weighted_color || options.alpha_coverage_cutoff.has_value()));
    if (options.alpha_coverage_cutoff) {
        const auto cutoff = *options.alpha_coverage_cutoff;
        const auto coverage = double(covered_texels(alpha_histogram(result.front()), cutoff)) /
                              (std::size_t(image->width) * image->height);
        // Correct independent, unscaled mip levels; scaling a parent before
        // filtering its children would accumulate coverage/colour distortion.
        for (std::size_t level = 1; level < result.size(); ++level) {
            auto &mip = result[level];
            const auto scale =
                coverage_scale(alpha_histogram(mip), cutoff, coverage * (std::size_t(mip.width) * mip.height));
            for (std::size_t i = alpha_channel; i < mip.rgba.size(); i += channel_count)
                mip.rgba[i] = byte(mip.rgba[i] * scale);
        }
    }
    return result;
}
std::vector<MipLevel> texture_mips(const Texture &texture) { return texture_mips(texture, {}); }
std::vector<MipLevel> base_color_mips(const Texture &texture) {
    if (texture.encoding != TextureEncoding::srgb)
        throw std::invalid_argument("Base-color mipmaps require sRGB encoding");
    return texture_mips(texture);
}
} // namespace anima
