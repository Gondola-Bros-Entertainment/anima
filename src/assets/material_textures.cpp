#include "alpha_coverage.hpp"
#include <anima/assets/material_textures.hpp>
#include <anima/assets/scene_validation.hpp>
#include <map>

namespace anima {
MaterialTexturePlan material_texture_plan(std::span<const Material> materials, std::span<const Texture> textures) {
    MaterialTexturePlan plan;
    plan.images.push_back({});
    plan.bindings.push_back({});
    using Key = std::pair<int, detail::MipKey>;
    std::map<Key, std::size_t> images{{Key{no_index, detail::mip_key({})}, 0}};
    for (const auto &material : materials) {
        validate_material(material, textures);
        const int sources[]{material.texture, material.normal_texture, material.metallic_roughness_texture,
                            material.emissive_texture, material.occlusion_texture};
        std::array<std::size_t, material_texture_count> bindings{};
        for (std::size_t binding = 0; binding < bindings.size(); ++binding) {
            const auto source = sources[binding];
            TextureMipOptions options;
            // Only a mip chain built here needs its own filtering: an unmipmapped image uploads its base level alone,
            // and a block-compressed one its stored levels.
            const auto *image = source >= 0 ? textures[source].image.get() : nullptr;
            if (binding == 0 && image && image->format == ImageFormat::rgba8 && textures[source].sampler.mipmapped)
                options = detail::base_color_mip_options(material);
            const auto [entry, inserted] =
                images.try_emplace(Key{source, detail::mip_key(options)}, plan.images.size());
            if (inserted)
                plan.images.push_back({source, options});
            bindings[binding] = entry->second;
        }
        plan.bindings.push_back(bindings);
    }
    return plan;
}
} // namespace anima
