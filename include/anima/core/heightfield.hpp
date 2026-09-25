#pragma once
#include <anima/core/math.hpp>
#include <optional>
#include <span>
#include <vector>

namespace anima {
// Borrowed, immutable grid in world X/Z. Heights are world Y, row-major (+Z),
// with +X columns. Each cell uses the (0,0) -> (1,1) diagonal. The owner keeps
// samples alive and unchanged for the lifetime of queries. No renderer required.
struct HeightfieldView {
    std::size_t columns{}, rows{};
    float origin_x{}, origin_z{}, spacing_x = 1, spacing_z = 1;
    std::span<const float> heights;
};
struct Heightfield {
    std::size_t columns{}, rows{};
    float origin_x{}, origin_z{}, spacing_x = 1, spacing_z = 1;
    std::vector<float> heights;
    [[nodiscard]] HeightfieldView view() const {
        return {columns, rows, origin_x, origin_z, spacing_x, spacing_z, heights};
    }
};
struct HeightfieldSample {
    float height{};
    Vec3 normal{0, 1, 0};
};
struct HeightfieldHit {
    double fraction{};
    Vec3 position{}, normal{};
};
struct HeightfieldMotion {
    Vec3 position{};
    double fraction{}, distance{};
    bool blocked{}; // Encountered a triangle below the requested normal.y.
};
// Full validation at asset acceptance. Subsequent queries inspect dimensions
// and touched samples only, with constant-time height lookup and no allocation.
void validate_heightfield(HeightfieldView field);
[[nodiscard]] std::optional<HeightfieldSample> sample_heightfield(HeightfieldView field, float x, float z);
// First contact with the solid below the heightfield over a finite segment.
// clearance offsets terrain vertically; this is not a sphere/capsule sweep.
// Traversal and fractions use double intermediates; returned positions use float.
[[nodiscard]] std::optional<HeightfieldHit> intersect_heightfield(HeightfieldView field, Vec3 from, Vec3 to,
                                                                  float clearance = 0);
// Follow the grounded X/Z segment within a surface-distance budget. Both
// endpoints must be in the grid. minimum_up is a caller-owned slope rule [0,1].
// Traversal visits every crossed triangle, so narrow ridges cannot be skipped.
// Distance and fraction precede rounding the returned position to float.
[[nodiscard]] HeightfieldMotion move_on_heightfield(HeightfieldView field, Vec3 from, Vec3 to, double distance_budget,
                                                    float minimum_up = 0);
} // namespace anima
