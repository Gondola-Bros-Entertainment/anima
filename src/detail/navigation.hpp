#pragma once
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace anima::detail {
inline constexpr std::size_t maximum_navigation_nodes = 1'000'000;
inline constexpr std::size_t maximum_navigation_edges = 8'000'000;
inline constexpr float maximum_navigation_speed = 10'000;
inline constexpr float maximum_arrival_distance = 10'000;
inline constexpr double minimum_navigation_step = 1e-6;
inline constexpr double maximum_navigation_step = .1;

inline void navigation_settings(float speed, float arrival_distance) {
    if (!std::isfinite(speed) || speed < 0 || speed > maximum_navigation_speed || !std::isfinite(arrival_distance) ||
        arrival_distance < 0 || arrival_distance > maximum_arrival_distance)
        throw std::invalid_argument("Invalid navigation speed/arrival distance");
}
inline void navigation_step(double seconds) {
    if (!std::isfinite(seconds) || seconds < minimum_navigation_step || seconds > maximum_navigation_step)
        throw std::invalid_argument("Navigation step must be in [0.000001, 0.1] seconds");
}
} // namespace anima::detail
