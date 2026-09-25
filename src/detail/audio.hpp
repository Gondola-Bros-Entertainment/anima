#pragma once
#include <anima/core/math.hpp>
#include <cmath>
#include <stdexcept>

namespace anima::detail {
inline constexpr float maximum_audio_gain = 16;
inline constexpr float minimum_audio_pitch = .01F;
inline constexpr float maximum_audio_pitch = 8;
inline constexpr float maximum_audio_distance = 1e9F;
inline constexpr float audio_direction_epsilon = 1e-6F;

inline void audio_gain(float value) {
    if (!std::isfinite(value) || value < 0 || value > maximum_audio_gain)
        throw std::invalid_argument("Audio gain must be in [0, 16]");
}
inline void audio_pitch(float value) {
    if (!std::isfinite(value) || value < minimum_audio_pitch || value > maximum_audio_pitch)
        throw std::invalid_argument("Audio pitch must be in [0.01, 8]");
}
inline void audio_pan(float value) {
    if (!std::isfinite(value) || std::abs(value) > 1)
        throw std::invalid_argument("Audio pan must be in [-1, 1]");
}
inline void audio_attenuation(float minimum, float maximum) {
    if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum < 0 || maximum <= minimum ||
        maximum > maximum_audio_distance)
        throw std::invalid_argument("Invalid audio attenuation distances");
}
inline void audio_location(Vec3 value) {
    for (auto coordinate : {value.x, value.y, value.z})
        if (!std::isfinite(coordinate) || std::abs(coordinate) > maximum_audio_distance)
            throw std::invalid_argument("Audio coordinates must be finite and within one billion units");
}
inline Vec3 audio_right(Vec3 forward, Vec3 up) {
    audio_location(forward);
    audio_location(up);
    const auto f = length(forward), u = length(up);
    if (f < audio_direction_epsilon || u < audio_direction_epsilon)
        throw std::invalid_argument("Invalid audio listener orientation");
    const auto right = cross(up * (1 / u), forward * (1 / f));
    if (length(right) < audio_direction_epsilon)
        throw std::invalid_argument("Parallel audio listener orientation vectors");
    return normalized(right);
}
} // namespace anima::detail
