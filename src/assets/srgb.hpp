#pragma once
#include <cmath>
#include <concepts>

namespace anima::detail {
// The sRGB transfer functions, in the precision of their argument: decode_srgb() maps an encoded value in [0, 1] to
// linear light and encode_srgb() maps linear light in [0, 1] back. Neither clamps.
template <std::floating_point T> T decode_srgb(T value) {
    return value <= T(.04045) ? value / T(12.92) : std::pow((value + T(.055)) / T(1.055), T(2.4));
}
template <std::floating_point T> T encode_srgb(T value) {
    return value <= T(.0031308) ? value * T(12.92) : T(1.055) * std::pow(value, 1 / T(2.4)) - T(.055);
}
} // namespace anima::detail
