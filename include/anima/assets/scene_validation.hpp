#pragma once
#include <anima/assets/mesh_snapshot.hpp>
#include <anima/assets/scene_budget.hpp>
#include <span>

namespace anima {
void validate_material(const Material &material, std::span<const Texture> textures);
// Validate an explicit CPU snapshot and its storage budget. Rendering consumes
// immutable Mesh resources; snapshots are for inspection and reference work.
void validate_scene(const MeshSnapshot &scene, SceneGeometryBudget budget = {});
} // namespace anima
