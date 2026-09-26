#pragma once
#include <anima/core/math.hpp>
#include <optional>
#include <span>
#include <vector>

/// @file
/// Segment and sphere sweeps against static triangle surfaces. Part of the `anima::core` target;
/// no renderer, physics world or collision policy is required.

namespace anima {
/// One triangle of a TriangleQuery surface, in either winding.
struct QueryTriangle {
    Vec3 a{}, b{}, c{};
};
/// First contact found by TriangleQuery::intersect.
struct TriangleHit {
    double fraction{};      ///< Position of first contact along the segment, in [0, 1] inclusive.
    std::size_t triangle{}; ///< Index in the triangle array given to the TriangleQuery constructor.
};
/// Work counters filled by TriangleQuery::intersect.
struct TriangleQueryStats {
    std::size_t bounds_tested{}, triangles_tested{};
};
/// How TriangleQuery::intersect treats a triangle already within the radius at the segment start.
enum class InitialOverlap {
    report, ///< Reports contact at fraction 0.
    ignore  ///< Skips that triangle for the whole query; other triangles of the same mesh still count.
};
/// Options for TriangleQuery::intersect.
struct TriangleQueryOptions {
    /// Radius of the swept sphere, finite and nonnegative; 0 casts the segment itself.
    float radius = 0;
    InitialOverlap initial_overlap = InitialOverlap::report;
};

/// Immutable bounding volume hierarchy over a copy of static, double-sided triangles.
///
/// Closed meshes are surfaces, not solid volumes: a query wholly inside one need not hit its
/// boundary. intersect() does not modify the query, so concurrent queries are safe when each uses
/// its own TriangleQueryStats.
class TriangleQuery {
  public:
    /// Copies @p triangles and builds the hierarchy; an empty set never reports a hit. Throws
    /// `std::invalid_argument` for a nonfinite vertex.
    explicit TriangleQuery(std::span<const QueryTriangle> triangles);
    /// First contact of the segment from @p from to @p to, swept by a sphere of the options' radius
    /// against triangle faces, edges and vertices. Ties go to the lower triangle index.
    ///
    /// When @p stats is not null it is reset and filled. Throws `std::invalid_argument` for
    /// nonfinite endpoints, a nonfinite or negative radius, or an unknown InitialOverlap value.
    [[nodiscard]] std::optional<TriangleHit> intersect(Vec3 from, Vec3 to, TriangleQueryOptions options = {},
                                                       TriangleQueryStats *stats = nullptr) const;
    [[nodiscard]] std::size_t triangle_count() const { return triangles_.size(); }

  private:
    struct Node {
        Vec3 minimum{}, maximum{};
        std::size_t first{}, count{}, left{}, right{};
    };
    std::size_t build(std::size_t first, std::size_t count);
    std::vector<QueryTriangle> triangles_;
    std::vector<std::size_t> order_;
    std::vector<Node> nodes_;
};
} // namespace anima
