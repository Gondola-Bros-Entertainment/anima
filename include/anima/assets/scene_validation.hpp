#pragma once
#include <anima/assets/mesh_snapshot.hpp>
#include <anima/assets/scene_budget.hpp>
#include <span>

/// @file
/// Validation of materials and MeshSnapshot contents. Part of the `anima::assets` target;
/// failures throw `std::invalid_argument` unless stated.

namespace anima {
/// Checks @p material against @p textures.
///
/// The base-color factor, metallic, roughness, alpha and occlusion strength must be in [0, 1];
/// the emissive factor finite and nonnegative; the normal scale finite; the alpha cutoff finite
/// and nonnegative; and the alpha mode valid. Each texture index must be -1 or in range, naming an
/// sRGB texture for the base-color and emissive maps and a linear one for the other maps.
void validate_material(const Material &material, std::span<const Texture> textures);
/// Checks @p scene and its storage budget.
///
/// Applies validate_scene_geometry with @p budget, which can throw SceneCapacityError, and
/// requires fewer than `INT_MAX` materials and textures, finite bounds, finite vertices with alpha
/// in [0, 1] and tangent w of 0, 1 or -1, draws of whole triangles inside the vertex array with a
/// valid material index and finite node matrix, valid materials, and textures whose byte counts
/// match their dimensions and whose samplers and encodings are valid. Rendering consumes immutable
/// Mesh resources; snapshots are for inspection and reference work.
void validate_scene(const MeshSnapshot &scene, SceneGeometryBudget budget = {});
} // namespace anima
