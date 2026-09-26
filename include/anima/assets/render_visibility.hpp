#pragma once
#include <anima/scene.hpp>

/// @file
/// Conservative frustum culling in Vulkan clip space. Part of the `anima::assets` target.

namespace anima {
/// Conservative test of world-space bounding boxes against the six Vulkan clip inequalities:
/// `-w <= x, y <= w` and `0 <= z <= w`.
///
/// Planes come straight from the column-major view-projection matrix, without dividing by `w` or
/// normalizing, so perspective, orthographic, reversed-depth and infinite-far projections all
/// work. The depth range matches the renderer, which clips depth.
class RenderFrustum {
  public:
    /// An unset frustum that retains every box until a camera is supplied.
    RenderFrustum() = default;
    /// Frustum of @p view_projection. Throws `std::invalid_argument` unless every element is finite.
    explicit RenderFrustum(const Mat4 &view_projection);
    /// Whether @p bounds may be visible.
    ///
    /// Each plane tests the box's support corner in double precision with a float rounding margin
    /// scaled to the camera matrix. Invalid, nonfinite or inverted bounds are retained, and a box
    /// touching a plane or containing the eye counts as visible. This is conservative rejection,
    /// not an exact overlap test: some offscreen boxes are retained, but a box that intersects the
    /// frustum is never culled, even with no corner inside.
    [[nodiscard]] bool intersects(const RenderBounds &bounds) const noexcept;

  private:
    std::array<std::array<double, 4>, 6> planes_{};
    // Absolute camera rows before plane extraction: cancellation must not
    // erase the error allowance for separate float clip-coordinate evaluation.
    std::array<std::array<double, 4>, 6> error_magnitudes_{};
};
} // namespace anima
