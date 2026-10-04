#pragma once
#include <anima/scene.hpp>
#include <cstddef>
#include <optional>
#include <span>

/// @file
/// Conservative frustum culling in Vulkan clip space, and the distances, visibility range culling and level of detail
/// choice that VulkanRenderer applies to each object and placement cluster. Part of the `anima::assets` target.

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
/// The distance from @p eye to the nearest point of @p bounds, in double: the length of the gaps along the three axes,
/// each the largest of 0, the amount by which @p eye lies below RenderBounds::minimum and the amount by which it lies
/// above RenderBounds::maximum, so 0 for bounds that hold the eye. Invalid bounds may hold it, so they measure 0. A NaN
/// coordinate makes the distance NaN, as does an axis on which the eye and a bound are the same infinity; other
/// infinite coordinates measure as they lie.
[[nodiscard]] double nearest_distance(Vec3 eye, const RenderBounds &bounds) noexcept;
/// The distance from @p eye to the farthest point of @p bounds, in double: the length of the reaches along the three
/// axes, each the larger of the distances from @p eye to RenderBounds::minimum and to RenderBounds::maximum. Invalid
/// bounds may hold any point, so they measure infinity. It is NaN where nearest_distance() is.
[[nodiscard]] double farthest_distance(Vec3 eye, const RenderBounds &bounds) noexcept;
/// Whether @p bounds may hold a point whose distance from @p eye lies in @p range, at least VisibilityRange::begin and
/// below VisibilityRange::end: false only for valid bounds whose nearest_distance() reaches the end or whose
/// farthest_distance() falls short of the begin, so a NaN distance or range bound never culls. Neither the margins nor
/// the range's validity (validate_visibility_range()) are consulted.
///
/// In a perspective view VulkanRenderer applies its view distance scale (VulkanRenderer::set_view_distance_scale()) to
/// @p range and culls a placement cluster that this rejects for the cluster's world bounds, and an object without
/// placements that it rejects for bounds holding only the point its range measures to (VisibilityRange).
[[nodiscard]] bool within_visibility_range(const VisibilityRange &range, Vec3 eye, const RenderBounds &bounds) noexcept;
/// The level of detail to draw from @p levels, a draw's IndexedDraw::levels, as its index; empty for the full draw.
///
/// A level fits when its DrawLevel::error, times @p scale and @p pixels_per_unit_error and, in a @p perspective view,
/// divided by @p distance, is at most @p threshold_pixels; a NaN product never fits. The result is the last of the
/// leading levels that fit, which for errors that never decrease, as Mesh::compile generates them, is the coarsest
/// level that fits. It is empty when @p levels is empty or its first level does not fit, when @p threshold_pixels is
/// not greater than 0, so that 0 always draws the full draw, and in a perspective view when @p distance is not greater
/// than 0, as from inside the bounds it was measured to. An orthographic view ignores @p distance.
///
/// VulkanRenderer passes the nearest_distance() from its eye to the draw's, or the placement cluster's, world bounds,
/// the largest axis scale among the matrices that place the draw, as VulkanRenderer::set_lod_threshold() describes, the
/// length of the first three elements of its view-projection matrix's second row times half the scene targets' height,
/// and its LOD threshold.
[[nodiscard]] std::optional<std::size_t> lod_level(std::span<const DrawLevel> levels, double distance, double scale,
                                                   double pixels_per_unit_error, bool perspective,
                                                   float threshold_pixels) noexcept;
} // namespace anima
