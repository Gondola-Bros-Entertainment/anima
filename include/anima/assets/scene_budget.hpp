#pragma once
#include <cstddef>
#include <stdexcept>

/// @file
/// Byte budget for the expanded vertices of a MeshSnapshot. Part of the `anima::assets` target.

namespace anima {
/// Allowance for the expanded MeshVertex storage of one Scene::snapshot or validate_scene call, in
/// bytes.
///
/// This is a resource policy, not a hardware capability or a bound on total process or GPU
/// memory: source assets, textures, metadata, staging copies and GPU residency are outside it, and
/// it does not limit the renderer.
struct SceneGeometryBudget {
    /// Maximum bytes of MeshVertex storage; 256 MiB by default.
    std::size_t vertex_bytes = 256 * 1024 * 1024;
};

/// Thrown when snapshot geometry exceeds its SceneGeometryBudget; Scene::snapshot throws it before
/// allocating.
class SceneCapacityError : public std::length_error {
  public:
    SceneCapacityError(std::size_t requested, std::size_t budget);
    /// Bytes the geometry needs.
    const std::size_t requested_bytes;
    /// SceneGeometryBudget::vertex_bytes that was exceeded.
    const std::size_t budget_bytes;
};

/// Returns the bytes that @p vertices MeshVertex values occupy, without allocating.
///
/// Checks draw addressing and byte arithmetic before the budget: throws `std::invalid_argument`
/// when @p vertices exceeds `UINT32_MAX` or the byte count overflows, then SceneCapacityError when
/// the bytes exceed @p budget. A zero budget admits only empty geometry.
[[nodiscard]] std::size_t validate_scene_geometry(std::size_t vertices, SceneGeometryBudget budget = {});
} // namespace anima
