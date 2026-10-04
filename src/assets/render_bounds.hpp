#pragma once
#include "mesh_limits.hpp"
#include <algorithm>
#include <anima/mesh.hpp>
#include <cmath>

namespace anima::detail {
// Widens each axis of valid @p bounds by posed_bounds_margin times the larger of 1 and the axis's largest magnitude,
// as Scene pads posed bounds and MeshPlacements pads placed ones. Leaves invalid bounds alone. A corner of bounds
// near the float range can become infinite, so callers that need finite bounds check them after.
inline void pad_posed_bounds(RenderBounds &bounds) {
    if (!bounds.valid)
        return;
    const auto margin = [](float lo, float hi) {
        return std::max({1.F, std::abs(lo), std::abs(hi)}) * mesh_limits::posed_bounds_margin;
    };
    const Vec3 m{margin(bounds.minimum.x, bounds.maximum.x), margin(bounds.minimum.y, bounds.maximum.y),
                 margin(bounds.minimum.z, bounds.maximum.z)};
    bounds.minimum = bounds.minimum - m;
    bounds.maximum = bounds.maximum + m;
}
} // namespace anima::detail
