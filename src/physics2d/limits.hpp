#pragma once

namespace anima::physics2d::detail {
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
