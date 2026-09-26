#pragma once
#include <anima/assets/asset.hpp>
#include <span>

namespace anima::detail {
// validate_scene()'s checks of every material and then every texture, for data held outside a MeshSnapshot.
// Mesh::compile_static runs them over a whole source before splitting it, since each piece's compile() sees only
// the materials and textures that piece uses.
void validate_surfaces(std::span<const Material> materials, std::span<const Texture> textures);
} // namespace anima::detail
