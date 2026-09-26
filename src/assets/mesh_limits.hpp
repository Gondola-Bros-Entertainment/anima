#pragma once
#include <cstddef>

namespace anima::mesh_limits {
inline constexpr std::size_t maximum_skin_joints = 512;
inline constexpr float bind_pose_tolerance = 2e-5F;
inline constexpr float skin_weight_tolerance = 1e-4F;
// Two rest poses match when every element of every world matrix is within this.
inline constexpr float rest_pose_tolerance = 1e-5F;
} // namespace anima::mesh_limits
