#pragma once
#include <cstddef>

namespace anima::mesh_limits {
inline constexpr float bind_pose_tolerance = 2e-5F;
inline constexpr float skin_weight_tolerance = 1e-4F;
// Relative margin that pads posed and placed bounds: the accepted skin weight error, and an equal rounding margin in
// the convex influence union at the GPU boundary, which also covers composing a placement on the GPU in another order.
inline constexpr float posed_bounds_margin = 2 * skin_weight_tolerance;
// Two rest poses match when every element of every world matrix is within this.
inline constexpr float rest_pose_tolerance = 1e-5F;
} // namespace anima::mesh_limits
