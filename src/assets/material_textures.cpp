#include "alpha_coverage.hpp"
#include <anima/assets/material_textures.hpp>
#include <anima/assets/scene_validation.hpp>
#include <map>

namespace anima {
MaterialTexturePlan material_texture_plan(std::span<const Material> materials, std::span<const Texture> textures) {
    MaterialTexturePlan plan;
    plan.images.push_back({});
    plan.bindings.push_back({});
    using Key = std::pair<int, std::optional<float>>;
    std::map<Key, std::size_t> images{{Key{-1, std::nullopt}, 0}};
    for (const auto &material : materials) {
        validate_material(material, textures);
        const int sources[]{material.texture, material.normal_texture, material.metallic_roughness_texture,
                            material.emissive_texture, material.occlusion_texture};
        std::array<std::size_t, material_texture_count> bindings{};
        for (std::size_t binding = 0; binding < bindings.size(); ++binding) {
            const auto source = sources[binding];
            TextureMipOptions options;
            // Only a mip chain needs correcting; an unmipmapped image uploads its base level alone.
            if (binding == 0 && source >= 0 && textures[source].sampler.mipmapped)
                options.alpha_coverage_cutoff = detail::alpha_coverage_cutoff(material);
            const auto [entry, inserted] =
                images.try_emplace(Key{source, options.alpha_coverage_cutoff}, plan.images.size());
            if (inserted)
                plan.images.push_back({source, options});
            bindings[binding] = entry->second;
        }
        plan.bindings.push_back(bindings);
    }
    return plan;
}
} // namespace anima
