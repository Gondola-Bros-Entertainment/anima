#include <anima/assets/mesh_preparation.hpp>
#include <stdexcept>

namespace anima {
MeshPreparation::MeshPreparation(std::shared_ptr<const Mesh> asset) : asset_(std::move(asset)) {
    if (!asset_)
        throw std::invalid_argument("Cannot prepare a null render asset");
    const auto &source = *asset_->materials();
    plan_ = material_texture_plan(source.material_data, source.textures);
    images_.reserve(plan_.images.size());
    const Texture white{1, 1, {255, 255, 255, 255}, {}};
    for (const auto &image : plan_.images) {
        const auto &texture = image.source < 0 ? white : source.textures.at(image.source);
        images_.push_back(texture.sampler.mipmapped
                              ? texture_mips(texture, image.mips)
                              : std::vector<MipLevel>{{texture.width, texture.height, texture.rgba}});
    }
}
} // namespace anima
