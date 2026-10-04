#pragma once
#include <anima/navigation.hpp>
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
inline constexpr auto unknown_steer_plane = "Unknown navigation steer plane";

inline void navigation_settings(const navigation::SteerSettings &settings) {
    if (!std::isfinite(settings.speed) || settings.speed < 0 || settings.speed > maximum_navigation_speed ||
        !std::isfinite(settings.arrival_distance) || settings.arrival_distance < 0 ||
        settings.arrival_distance > maximum_arrival_distance)
        throw std::invalid_argument("Invalid navigation speed/arrival distance");
    if (settings.plane != navigation::SteerPlane::xyz && settings.plane != navigation::SteerPlane::xz)
        throw std::invalid_argument(unknown_steer_plane);
}
inline void navigation_step(double seconds) {
    if (!std::isfinite(seconds) || seconds < minimum_navigation_step || seconds > maximum_navigation_step)
        throw std::invalid_argument("Navigation step must be in [0.000001, 0.1] seconds");
}
} // namespace anima::detail
