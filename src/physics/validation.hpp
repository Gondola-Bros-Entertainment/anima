#pragma once
#include "../detail/physics_limits.hpp"
#include <cstddef>

namespace anima::physics::detail {
// Shared runtime/codec budgets. These are input limits, not precision guarantees.
using anima::detail::all_layers;
using anima::detail::collision_layer_count;
using anima::detail::maximum_step_seconds;
using anima::detail::maximum_vector_component;
using anima::detail::minimum_squared_displacement;
using anima::detail::minimum_step_seconds;
inline constexpr std::size_t maximum_mesh_vertices = 1'000'000;
inline constexpr std::size_t maximum_mesh_indices = 3'000'000;
inline constexpr std::size_t maximum_hull_points = 256;
inline constexpr std::size_t maximum_compound_children = 64;
inline constexpr std::size_t maximum_component_bytes = 16 * 1024 * 1024;
} // namespace anima::physics::detail
