#pragma once
#include <cstdint>

namespace anima::detail {
// Input limits that the 3D and 2D physics modules share. They bound validation, not precision.
inline constexpr float maximum_vector_component = 1'000'000.F;
inline constexpr double minimum_step_seconds = .000001;
inline constexpr double maximum_step_seconds = .1;
// Ray and sweep displacements whose squared length is at or below this are rejected as zero.
inline constexpr float minimum_squared_displacement = 1e-12F;
// Bodies take a layer in [0, collision_layer_count), and each layer's collision mask holds one bit per layer.
inline constexpr unsigned collision_layer_count = 16;
inline constexpr auto all_layers = static_cast<std::uint16_t>((1U << collision_layer_count) - 1);
} // namespace anima::detail
