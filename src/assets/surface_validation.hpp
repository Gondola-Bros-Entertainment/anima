#pragma once
#include <anima/assets/asset.hpp>
#include <cstddef>
#include <span>

namespace anima::detail {
// Whether validation requires each texture's image to have texels, as compiling does, or accepts images that only
// describe a texture, as validate_scene() does.
enum class Texels { optional, required };
// validate_scene()'s checks of every material and then every texture, with the texel rule that @p texels selects.
// Mesh::compile runs them over its source, and Mesh::compile_static over a whole source before splitting it, since
// each piece's compile() sees only the materials and textures that piece uses.
void validate_surfaces(std::span<const Material> materials, std::span<const Texture> textures, Texels texels);
// Checks @p image as validate_scene() checks a texture's image, with the texel rule that @p texels selects, and
// returns the bytes of its texels, whether it has them or not.
std::size_t validate_image(const Image *image, Texels texels);
} // namespace anima::detail
