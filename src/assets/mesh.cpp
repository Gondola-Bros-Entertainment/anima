#include "alpha_coverage.hpp"
#include "surface_validation.hpp"
#include <algorithm>
#include <anima/scene.hpp>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace anima {
std::vector<std::shared_ptr<const Mesh>> Mesh::compile_static(const Asset &source, MeshCompileOptions options) {
    const auto vertices_per_resource = options.max_vertices;
    const auto texture_edge = options.max_texture_edge;
    const auto retention = options.texel_retention;
    if (retention != TexelRetention::keep && retention != TexelRetention::until_upload)
        throw std::invalid_argument("Unknown texel retention");
    if (vertices_per_resource && vertices_per_resource < 3)
        throw std::invalid_argument("Mesh vertex limit must fit at least one triangle");
    for (const auto &primitive : source.primitives)
        if (primitive.vertices.empty() || primitive.vertices.size() % 3)
            throw std::invalid_argument("Static mesh must contain complete triangles");
    std::vector<std::shared_ptr<const Mesh>> result;
    if (!source.skins.empty() || !source.animations.empty())
        throw std::runtime_error("Static mesh preparation requires static geometry.");
    if (!vertices_per_resource && !texture_edge)
        result.push_back(Mesh::compile(source, retention));
    else {
        const auto vertex_limit =
            vertices_per_resource ? vertices_per_resource : std::numeric_limits<std::size_t>::max();
        // Bound individual uploads while preserving geometry and placement.
        // Limits are supplied by the consumer; the immutable source is preserved.
        // Every piece keeps each node and the import metadata that compile() records.
        Asset piece;
        piece.nodes = source.nodes;
        piece.mesh_nodes = source.mesh_nodes;
        piece.notices = source.notices;
        // Fail as compile() would, in its order. Compiling the nodes alone checks the hierarchy and transforms;
        // every material and texture is checked next, used or not, before any is indexed or shrunk; and each
        // piece's compile() then checks its primitives whole, in source order.
        auto nodes_only = Mesh::compile(piece, retention);
        detail::validate_surfaces(source.materials, source.textures, detail::Texels::required);
        std::vector<SourcePrimitive> pending;
        const auto oversized = [&](const Texture &texture) {
            return texture_edge && std::max(texture.image->width, texture.image->height) > texture_edge;
        };
        // A texture and the mip options of one of its uses. An oversized texture shrinks once per set of options
        // its uses need, so each shrunk level keeps the coverage and color weighting that its upload's mips use.
        using TextureUse = std::pair<int, detail::MipKey>;
        // Mips depend only on the image, the encoding and the options, so textures that share an image and an
        // encoding also share each shrunk image. Keys hold the shared pointer, which orders by identity.
        using ShrunkImage = std::tuple<std::shared_ptr<const Image>, TextureEncoding, detail::MipKey>;
        std::map<ShrunkImage, std::shared_ptr<const Image>> shrunk;
        const auto fitted = [&](int id, const TextureMipOptions &mip_options) {
            auto texture = source.textures.at(id);
            if (!oversized(texture))
                return texture;
            auto &image = shrunk[{texture.image, texture.encoding, detail::mip_key(mip_options)}];
            if (!image) {
                auto mips = texture_mips(texture, mip_options);
                const auto found = std::find_if(mips.begin(), mips.end(), [&](const auto &m) {
                    return std::max(m.width, m.height) <= texture_edge;
                });
                if (found == mips.end())
                    image = texture.image;
                else
                    image = std::make_shared<Image>(Image{found->width, found->height, std::move(found->rgba)});
            }
            texture.image = image;
            return texture;
        };
        std::size_t vertices = 0;
        const auto flush = [&] {
            if (pending.empty())
                return;
            auto batch = piece;
            batch.primitives = std::move(pending);
            std::map<int, int> materials;
            std::map<TextureUse, int> textures;
            const auto texture = [&](int &id, TextureMipOptions mip_options = {}) {
                if (id < 0)
                    return;
                if (!oversized(source.textures.at(id)))
                    mip_options = {}; // Kept as authored, so every use shares one copy.
                auto [it, inserted] = textures.emplace(TextureUse{id, detail::mip_key(mip_options)},
                                                       static_cast<int>(batch.textures.size()));
                if (inserted)
                    batch.textures.push_back(fitted(id, mip_options));
                id = it->second;
            };
            for (auto &primitive : batch.primitives) {
                // An index past the source's materials is also past the piece's, which compile() rejects.
                if (primitive.material < 0 || std::size_t(primitive.material) >= source.materials.size())
                    continue;
                auto [it, inserted] = materials.emplace(primitive.material, static_cast<int>(batch.materials.size()));
                if (inserted) {
                    auto material = source.materials.at(primitive.material);
                    texture(material.texture, detail::base_color_mip_options(material));
                    texture(material.normal_texture);
                    texture(material.metallic_roughness_texture);
                    texture(material.emissive_texture);
                    texture(material.occlusion_texture);
                    batch.materials.push_back(std::move(material));
                }
                primitive.material = it->second;
            }
            result.push_back(Mesh::compile(batch, retention));
            pending.clear();
            vertices = 0;
        };
        for (const auto &primitive : source.primitives) {
            // Material changes split only under a vertex limit; without one the geometry stays in one Mesh.
            if (vertices_per_resource && !pending.empty() && pending.back().material != primitive.material)
                flush();
            for (std::size_t first = 0; first < primitive.vertices.size();) {
                if (vertices > vertex_limit - 3)
                    flush();
                const auto count = std::min(primitive.vertices.size() - first, (vertex_limit - vertices) / 3 * 3);
                SourcePrimitive part;
                part.mesh_name = primitive.mesh_name;
                part.node = primitive.node;
                part.skin = primitive.skin;
                part.material = primitive.material;
                part.vertices.assign(primitive.vertices.begin() + static_cast<std::ptrdiff_t>(first),
                                     primitive.vertices.begin() + static_cast<std::ptrdiff_t>(first + count));
                pending.push_back(std::move(part));
                vertices += count;
                first += count;
            }
        }
        flush();
        // A source without primitives still compiles, as compile() does, into one Mesh of its nodes.
        if (result.empty())
            result.push_back(std::move(nodes_only));
    }
    return result;
}
} // namespace anima
