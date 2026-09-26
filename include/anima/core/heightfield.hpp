#pragma once
#include <anima/core/math.hpp>
#include <optional>
#include <span>
#include <vector>

/// @file
/// Terrain height queries over a regular grid of samples. Part of the `anima::core` target; no
/// renderer, asset parser or game policy is required.
///
/// Sample (column `c`, row `r`) lies at world X `origin_x + c * spacing_x` and Z
/// `origin_z + r * spacing_z`, with height (world Y) `heights[r * columns + c]`. Each cell splits
/// into two triangles along its diagonal from sample `(c, r)` to `(c + 1, r + 1)`. Queries
/// interpolate on those triangles' planes and report their face normals, and the terrain is solid
/// below them; a render mesh of the same terrain must split cells along the same diagonal, since
/// bilinear interpolation describes a different surface. The field is single-valued (no caves,
/// overhangs or bridges), with no streaming, level of detail, seams or origin rebasing.
///
/// Queries check the grid layout but only the heights they touch, take time proportional to the
/// cells they cross, never allocate and change no state; call validate_heightfield once to check
/// every height. Invalid arguments throw `std::invalid_argument`.

namespace anima {
/// Borrowed, immutable grid of heights. The owner keeps the samples alive and unchanged while
/// queries use the view.
struct HeightfieldView {
    /// Samples along +X, at least 2.
    std::size_t columns{};
    /// Samples along +Z, at least 2.
    std::size_t rows{};
    /// World X of column 0.
    float origin_x{};
    /// World Z of row 0.
    float origin_z{};
    /// Positive distance between columns.
    float spacing_x = 1;
    /// Positive distance between rows.
    float spacing_z = 1;
    /// `columns * rows` heights, row by row.
    std::span<const float> heights;
};
/// Owning grid with the same fields as HeightfieldView.
struct Heightfield {
    std::size_t columns{}, rows{};
    float origin_x{}, origin_z{}, spacing_x = 1, spacing_z = 1;
    std::vector<float> heights;
    /// View that borrows #heights until they are reallocated or this grid is destroyed.
    [[nodiscard]] HeightfieldView view() const {
        return {columns, rows, origin_x, origin_z, spacing_x, spacing_z, heights};
    }
};
/// Terrain surface at one X/Z position.
struct HeightfieldSample {
    /// Surface height (world Y).
    float height{};
    /// Unit upward normal of the triangle containing the position.
    Vec3 normal{0, 1, 0};
};
/// First contact of a segment with the terrain.
struct HeightfieldHit {
    /// Position along the whole segment, in [0, 1].
    double fraction{};
    /// Segment point at #fraction.
    Vec3 position{};
    /// Unit upward normal of the triangle hit.
    Vec3 normal{};
};
/// Result of move_on_heightfield.
struct HeightfieldMotion {
    /// Final position on the surface.
    Vec3 position{};
    /// Progress along the X/Z segment, in [0, 1].
    double fraction{};
    /// Surface distance travelled.
    double distance{};
    /// Whether motion stopped at a triangle whose normal Y is below the requested minimum.
    bool blocked{};
};
/// Checks the whole grid, for use when accepting it. Throws `std::invalid_argument` unless
/// @p field has at least 2 columns and rows, exactly `columns * rows` heights, a finite origin,
/// finite positive spacing, a far edge that is a finite `float` beyond the origin on each axis,
/// and only finite heights.
void validate_heightfield(HeightfieldView field);
/// Surface height and normal at world (@p x, @p z), or empty when the position is not finite or
/// lies outside the grid; the grid's edges count as inside. Throws `std::invalid_argument` for an
/// invalid layout or a nonfinite height in the containing cell.
[[nodiscard]] std::optional<HeightfieldSample> sample_heightfield(HeightfieldView field, float x, float z);
/// First point of the segment from @p from to @p to that is at or below the surface raised by
/// @p clearance, or empty when there is none over the grid.
///
/// A segment that starts under the surface hits where it starts, or where it enters the grid;
/// parts of the segment outside the grid are ignored. @p clearance, finite and nonnegative, offsets
/// the surface vertically; this is not a sphere or capsule sweep. Fractions use double precision
/// and positions are rounded to `float`. Throws `std::invalid_argument` for an invalid layout,
/// nonfinite endpoints, an invalid @p clearance or a nonfinite height the segment crosses.
[[nodiscard]] std::optional<HeightfieldHit> intersect_heightfield(HeightfieldView field, Vec3 from, Vec3 to,
                                                                  float clearance = 0);
/// Follows the surface along the X/Z segment from @p from to @p to until @p distance_budget of
/// surface distance is used or a triangle is steeper than @p minimum_up allows.
///
/// Only the X and Z of @p from and @p to steer the motion, and both must lie on the grid; their
/// Y must still be finite and is replaced by the surface height. The path visits every triangle
/// the segment crosses, so narrow ridges cannot be skipped. Distance includes height change, and
/// an exhausted @p distance_budget (finite and nonnegative) stops partway across a triangle. The
/// path stops before a triangle whose normal Y is below @p minimum_up, in [0, 1]; for a maximum
/// slope angle `theta`, pass `cos(theta)`. That stop lies on the steep triangle's edge, where a
/// later point query may select the steep triangle. Distance and fraction use double precision
/// and the position is rounded to `float`. Throws `std::invalid_argument` for invalid arguments,
/// an endpoint off the grid or a nonfinite height the path crosses.
[[nodiscard]] HeightfieldMotion move_on_heightfield(HeightfieldView field, Vec3 from, Vec3 to, double distance_budget,
                                                    float minimum_up = 0);
} // namespace anima
