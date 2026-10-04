#include <anima/assets/mesh_preparation.hpp>
#include <stdexcept>

namespace anima {
MeshPreparation::MeshPreparation(std::shared_ptr<const Mesh> asset) : asset_(std::move(asset)) {
    if (!asset_)
        throw std::invalid_argument("Cannot prepare a null render asset");
    const auto &source = *asset_->description();
    plan_ = material_texture_plan(source.materials, source.textures);
    const auto texels = asset_->texel_images();
    images_.reserve(plan_.images.size());
    compressed_images_.reserve(plan_.images.size());
    const Texture white{std::make_shared<Image>(Image{1, 1, {255, 255, 255, 255}}), {}};
    for (const auto &image : plan_.images) {
        auto texture = white;
        if (image.source >= 0) {
            // The texture's sampler and encoding, with the image that holds its texels.
            texture = source.textures.at(image.source);
            texture.image = texels.at(image.source);
        }
        const auto &pixels = *texture.image;
        // A block-compressed image uploads the levels it stores, so it has nothing to filter; keep the image.
        if (pixels.format != ImageFormat::rgba8) {
            images_.emplace_back();
            compressed_images_.push_back(texture.image);
        } else {
            images_.push_back(texture.sampler.mipmapped
                                  ? texture_mips(texture, image.mips)
                                  : std::vector<MipLevel>{{pixels.width, pixels.height, pixels.rgba}});
            compressed_images_.emplace_back();
        }
    }
}
} // namespace anima
