#pragma once
#include <anima/assets/asset.hpp>
#include <span>

/// @file
/// Texture binding plan shared by synchronous and prepared GPU uploads. Part of the
/// `anima::assets` target.

namespace anima {
/// Texture bindings per material: base color, normal, metallic-roughness, emissive and occlusion.
inline constexpr std::uint32_t material_texture_count = 5;
/// One image to upload.
struct MaterialTextureImage {
    /// Source texture index, or -1 for the shared 1x1 white fallback.
    int source = -1;
    /// Mip options; set for a masked base-color map.
    TextureMipOptions mips;
};
/// Deduplicated images and the image each material binding uses.
struct MaterialTexturePlan {
    /// Images to upload; entry 0 is the white fallback.
    std::vector<MaterialTextureImage> images;
    /// Indices into #images in material_texture_count order: a fallback material first, then each
    /// input material.
    std::vector<std::array<std::size_t, material_texture_count>> bindings;
};
/// Plans the images that @p materials need, without decoding or uploading anything.
///
/// Uses of one texture with the same mip options share an image, unreferenced textures get none
/// and absent maps bind the white fallback. A masked base-color map on a mipmapped texture gets
/// its own image with the cutoff `alpha_cutoff / alpha` when that is in (0, 1]. Throws
/// `std::invalid_argument` for a material that validate_material rejects.
[[nodiscard]] MaterialTexturePlan material_texture_plan(std::span<const Material> materials,
                                                        std::span<const Texture> textures);
} // namespace anima
