#pragma once
#include <cstddef>
#include <stdexcept>

namespace anima {
// Per-scene expanded vertex storage allowance, in bytes. This is a resource
// policy, not a hardware capability or a total process/GPU memory guarantee.
// CPU staging, source assets, textures and a retiring scene consume extra memory.
struct SceneGeometryBudget {
    std::size_t vertex_bytes = 256 * 1024 * 1024;
};

class SceneCapacityError : public std::length_error {
  public:
    SceneCapacityError(std::size_t requested, std::size_t budget);
    const std::size_t requested_bytes, budget_bytes;
};

// Check draw addressability and byte arithmetic before checking resource policy.
// Returns the byte count; makes no allocation. A zero budget permits empty scenes.
[[nodiscard]] std::size_t validate_scene_geometry(std::size_t vertices, SceneGeometryBudget budget = {});
} // namespace anima
