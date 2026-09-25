#pragma once
#include <anima/core/math.hpp>
#include <optional>
#include <span>
#include <vector>

namespace anima {
struct QueryTriangle {
    Vec3 a{}, b{}, c{};
};
struct TriangleHit {
    double fraction{};      // First contact along the finite segment, including endpoints.
    std::size_t triangle{}; // Index in the caller's original triangle array.
};
struct TriangleQueryStats {
    std::size_t bounds_tested{}, triangles_tested{};
};
enum class InitialOverlap { report, ignore };
struct TriangleQueryOptions {
    float radius = 0;
    InitialOverlap initial_overlap = InitialOverlap::report;
};

// Immutable, owning BVH for static, double-sided triangle surfaces. Shared by
// callers/instances; no renderer, physics world or game collision policy required.
// A positive radius sweeps a sphere including triangle faces, edges and vertices.
// Initial surface overlap returns zero by default. The ignore policy skips only
// initially overlapping triangles, not other surfaces of the same mesh.
// Closed meshes are surfaces, not solid volumes: a query wholly inside one need
// not hit its boundary.
class TriangleQuery {
  public:
    explicit TriangleQuery(std::span<const QueryTriangle> triangles);
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
