#pragma once
#include "image_limits.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace anima::detail {
// The characters of each segment of a name that UiContext::set_image() registers.
inline constexpr std::string_view memory_image_characters =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.";
// Whether @p name is a name that UiContext::set_image() registers: segments of memory_image_characters joined by '/',
// none empty, "." or "..". VectorAtlas applies the same rule to its names, so that each can name a UI image.
inline bool memory_image_name(std::string_view name) {
    for (std::size_t start = 0;;) {
        const auto end = name.find('/', start);
        const auto segment = name.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == ".." ||
            segment.find_first_not_of(memory_image_characters) != std::string_view::npos)
            return false;
        if (end == std::string_view::npos)
            return true;
        start = end + 1;
    }
}
// The whole dp that documents measure @p pixels of a UiImage drawn at @p scale pixels per dp as: the nearest, halves
// away from zero, and at least 1; or 0 when @p scale is not finite and positive or the result exceeds the image edge
// limit, which UiContext::set_image() rejects. set_ui_images() checks its images with it before registering any.
inline int measured_dp(std::uint32_t pixels, float scale) {
    if (!std::isfinite(scale) || !(scale > 0))
        return 0;
    const auto dp = std::max(1., std::round(double(pixels) / double(scale)));
    return dp <= double(maximum_image_edge) ? int(dp) : 0;
}
} // namespace anima::detail
