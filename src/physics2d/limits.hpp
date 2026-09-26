#pragma once
#include "../detail/physics_limits.hpp"

namespace anima::physics2d::detail {
using anima::detail::all_layers;
using anima::detail::collision_layer_count;
using anima::detail::maximum_step_seconds;
using anima::detail::maximum_vector_component;
using anima::detail::minimum_squared_displacement;
using anima::detail::minimum_step_seconds;
// A planar transform's axes are unit length and orthogonal, and its Z axis unchanged, within this.
inline constexpr float planar_tolerance = 1e-4F;
// A fixed-rotation kinematic body may change its angle by at most this, in radians.
inline constexpr float fixed_rotation_tolerance = 1e-6F;
inline constexpr std::uint32_t maximum_world_bodies = 1'000'000;
inline constexpr unsigned maximum_substeps = 16;
inline constexpr float minimum_density = .001F;
inline constexpr float maximum_density = 10'000;

// Box2D asserts that every bounding box stays strictly within B2_HUGE (100,000 m of the origin) and pads
// its tree boxes by B2_AABB_MARGIN (0.1 m). A collider reaches at most twice the largest dimension from its
// body's origin (a capsule's half height plus radius), so body origins stay within the limit minus that
// reach, with 1 m to spare for the margin and rounding.
inline constexpr float minimum_collider_dimension = .01F;
inline constexpr float maximum_collider_dimension = 10'000;
inline constexpr float box2d_world_limit = 100'000;
inline constexpr float maximum_body_reach = 2 * maximum_collider_dimension;
inline constexpr float maximum_position = box2d_world_limit - maximum_body_reach - 1;
} // namespace anima::physics2d::detail
