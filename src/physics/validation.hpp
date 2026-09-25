#pragma once
#include <cstddef>

namespace anima::physics::detail {
// Shared runtime/codec budgets. These are input limits, not precision guarantees.
inline constexpr float maximum_vector_component = 1'000'000.F;
inline constexpr double minimum_step_seconds = .000001;
inline constexpr double maximum_step_seconds = .1;
inline constexpr unsigned collision_layer_count = 16;
inline constexpr std::size_t maximum_mesh_vertices = 1'000'000;
inline constexpr std::size_t maximum_mesh_indices = 3'000'000;
inline constexpr std::size_t maximum_hull_points = 256;
inline constexpr std::size_t maximum_compound_children = 64;
inline constexpr std::size_t maximum_component_bytes = 16 * 1024 * 1024;
} // namespace anima::physics::detail
