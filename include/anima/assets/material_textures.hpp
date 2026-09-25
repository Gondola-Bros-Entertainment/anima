#pragma once
#include <anima/assets/asset.hpp>
#include <span>

namespace anima {
inline constexpr std::uint32_t material_texture_count = 5;
struct MaterialTextureImage {
    int source = -1; // -1 is the shared white fallback.
    TextureMipOptions mips;
};
struct MaterialTexturePlan {
    std::vector<MaterialTextureImage> images;
    // Fallback material first, then each input material. Binding order is base
    // colour, normal, metallic/roughness, emissive, occlusion.
    std::vector<std::array<std::size_t, material_texture_count>> bindings;
};
// Pure preparation: deduplicate identical uses; isolate different alpha cutoffs
// and ordinary colour/data uses. Unreferenced source images need no GPU upload.
[[nodiscard]] MaterialTexturePlan material_texture_plan(std::span<const Material> materials,
                                                        std::span<const Texture> textures);
} // namespace anima
