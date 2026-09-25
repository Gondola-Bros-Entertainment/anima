#pragma once
#include <anima/scene.hpp>

namespace anima {
// Conservative world-space AABB test for Vulkan clip coordinates:
// -w <= x,y <= w and 0 <= z <= w. Supports perspective/orthographic,
// reversed depth and infinite far projections without normalizing planes.
class RenderFrustum {
  public:
    // An unset frustum retains everything until a camera is supplied.
    RenderFrustum() = default;
    explicit RenderFrustum(const Mat4 &view_projection);
    // Unknown/invalid bounds are retained. Touching a plane is visible; this
    // may retain some offscreen boxes, but never requires a corner inside.
    [[nodiscard]] bool intersects(const RenderBounds &bounds) const noexcept;

  private:
    std::array<std::array<double, 4>, 6> planes_{};
    // Absolute camera rows before plane extraction: cancellation must not
    // erase the error allowance for separate float clip-coordinate evaluation.
    std::array<std::array<double, 4>, 6> error_magnitudes_{};
};
} // namespace anima
