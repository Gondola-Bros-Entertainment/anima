#include "bc7.hpp"
#include "surface_validation.hpp"
#include <anima/assets/scene_validation.hpp>
#include <limits>
#include <stdexcept>
#include <string>

namespace anima {
namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::invalid_argument(message);
}
bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool valid(Filter filter) { return filter == Filter::nearest || filter == Filter::linear; }
bool valid(Wrap wrap) { return wrap == Wrap::repeat || wrap == Wrap::clamp || wrap == Wrap::mirror; }
} // namespace
void validate_material(const Material &m, std::span<const Texture> textures) {
    const auto unit = [](float x) { return std::isfinite(x) && x >= 0 && x <= 1; };
    require(unit(m.factor.x) && unit(m.factor.y) && unit(m.factor.z) && unit(m.metallic) && unit(m.roughness) &&
                unit(m.alpha) && unit(m.occlusion_strength),
            "Invalid material factors");
    require(finite(m.emissive) && m.emissive.x >= 0 && m.emissive.y >= 0 && m.emissive.z >= 0 &&
                std::isfinite(m.normal_scale) && std::isfinite(m.alpha_cutoff) && m.alpha_cutoff >= 0,
            "Invalid material surface parameters");
    require(m.alpha_mode == AlphaMode::opaque || m.alpha_mode == AlphaMode::mask || m.alpha_mode == AlphaMode::blend,
            "Invalid alpha mode");
    struct TextureUse {
        int index;
        TextureEncoding encoding;
    };
    const TextureUse uses[]{{m.texture, TextureEncoding::srgb},
                            {m.normal_texture, TextureEncoding::linear},
                            {m.metallic_roughness_texture, TextureEncoding::linear},
                            {m.emissive_texture, TextureEncoding::srgb},
                            {m.occlusion_texture, TextureEncoding::linear}};
    for (const auto [index, encoding] : uses) {
        require(index >= -1 && (index < 0 || std::size_t(index) < textures.size()),
                "Invalid material texture reference");
        if (index >= 0)
            require(textures[index].encoding == encoding, "Material texture encoding does not match its usage");
    }
}
SceneCapacityError::SceneCapacityError(std::size_t requested, std::size_t budget)
    : std::length_error("MeshSnapshot geometry needs " + std::to_string(requested) + " bytes; budget is " +
                        std::to_string(budget) + " bytes"),
      requested_bytes(requested), budget_bytes(budget) {}
std::size_t validate_scene_geometry(std::size_t vertices, SceneGeometryBudget budget) {
    require(vertices <= std::numeric_limits<std::uint32_t>::max(), "MeshSnapshot draw address overflow");
    require(vertices <= std::numeric_limits<std::size_t>::max() / sizeof(MeshVertex),
            "MeshSnapshot vertex byte count overflow");
    const auto bytes = vertices * sizeof(MeshVertex);
    if (bytes > budget.vertex_bytes)
        throw SceneCapacityError(bytes, budget.vertex_bytes);
    return bytes;
}
void validate_scene(const MeshSnapshot &scene, SceneGeometryBudget budget) {
    detail::validate_scene(scene, budget, detail::Texels::optional);
}
void detail::validate_scene(const MeshSnapshot &scene, SceneGeometryBudget budget, Texels texels) {
    (void)validate_scene_geometry(scene.vertices.size(), budget);
    require(scene.material_data.size() < static_cast<std::size_t>(std::numeric_limits<int>::max()) &&
                scene.textures.size() < static_cast<std::size_t>(std::numeric_limits<int>::max()),
            "MeshSnapshot descriptor index overflow");
    require(finite(scene.minimum) && finite(scene.maximum), "Non-finite scene bounds");
    for (const auto &vertex : scene.vertices)
        require(
            finite(vertex.position) && finite(vertex.normal) && finite(vertex.color) && std::isfinite(vertex.uv[0]) &&
                std::isfinite(vertex.uv[1]) && std::isfinite(vertex.alpha) && vertex.alpha >= 0 && vertex.alpha <= 1 &&
                std::all_of(vertex.tangent.begin(), vertex.tangent.end(), [](float x) { return std::isfinite(x); }) &&
                (vertex.tangent[3] == 0 || vertex.tangent[3] == -1 || vertex.tangent[3] == 1),
            "Invalid scene vertex");
    for (const auto &draw : scene.primitives) {
        require(draw.vertex_count && draw.vertex_count % 3 == 0, "MeshSnapshot draw must contain complete triangles");
        require(draw.first_vertex <= scene.vertices.size() &&
                    draw.vertex_count <= scene.vertices.size() - draw.first_vertex,
                "MeshSnapshot draw exceeds vertex range");
        require(draw.material_index >= -1 && (draw.material_index < 0 || static_cast<std::size_t>(draw.material_index) <
                                                                             scene.material_data.size()),
                "MeshSnapshot draw references an invalid material");
        for (float value : draw.node_world)
            require(std::isfinite(value), "Non-finite scene node transform");
    }
    validate_surfaces(scene.material_data, scene.textures, texels);
}
std::size_t detail::validate_image(const Image *image, Texels texels) {
    // A texture without an image has no dimensions.
    require(image && image->width && image->height &&
                image->width <= std::numeric_limits<std::size_t>::max() / 4 / image->height,
            "Invalid scene texture dimensions");
    // An image without texels only describes a texture; see TexelRetention.
    const auto has_texels = !image->rgba.empty() || !image->blocks.empty();
    require(has_texels || texels == Texels::optional, "Texture image has no texels");
    switch (image->format) {
    case ImageFormat::rgba8: {
        require(image->levels == 1 && image->blocks.empty(), "RGBA8 texture image must store one level and no blocks");
        const auto bytes = std::size_t(image->width) * image->height * 4;
        require(!has_texels || image->rgba.size() == bytes,
                "MeshSnapshot texture byte count does not match dimensions");
        return bytes;
    }
    case ImageFormat::bc7: {
        require(image->rgba.empty(), "BC7 texture image must not store RGBA8 texels");
        require(image->levels >= 1 && image->levels <= full_mip_levels(image->width, image->height),
                "BC7 texture image must store 1 to all of its mip levels");
        const auto bytes = bc7_image_bytes(image->width, image->height, image->levels);
        require(!has_texels || image->blocks.size() == bytes,
                "BC7 texture image byte count does not match its dimensions and levels");
        return bytes;
    }
    }
    throw std::invalid_argument("Unknown texture image format");
}
void detail::validate_surfaces(std::span<const Material> materials, std::span<const Texture> textures, Texels texels) {
    for (const auto &material : materials)
        validate_material(material, textures);
    std::size_t texture_bytes = 0;
    for (const auto &texture : textures) {
        const auto bytes = validate_image(texture.image.get(), texels);
        // Leave room for mip levels and staging-size arithmetic.
        require(bytes <= std::numeric_limits<std::size_t>::max() / 2 - texture_bytes,
                "MeshSnapshot texture byte count overflow");
        texture_bytes += bytes;
        const auto &s = texture.sampler;
        require(valid(s.mag) && valid(s.min) && valid(s.mip) && valid(s.u) && valid(s.v), "Invalid scene sampler");
        require(texture.encoding == TextureEncoding::srgb || texture.encoding == TextureEncoding::linear,
                "Invalid texture encoding");
    }
}
} // namespace anima
