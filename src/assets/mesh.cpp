#include <anima/scene.hpp>
#include <limits>
#include <map>
#include <stdexcept>

namespace anima {
std::vector<std::shared_ptr<const Mesh>> Mesh::compile_static(const Asset &source, MeshCompileOptions options) {
    const auto vertices_per_resource = options.max_vertices;
    const auto texture_edge = options.max_texture_edge;
    if (vertices_per_resource && vertices_per_resource < 3)
        throw std::invalid_argument("Mesh vertex limit must fit at least one triangle");
    for (const auto &primitive : source.primitives)
        if (primitive.vertices.empty() || primitive.vertices.size() % 3)
            throw std::invalid_argument("Static mesh must contain complete triangles");
    std::vector<std::shared_ptr<const Mesh>> result;
    if (!source.skins.empty() || !source.animations.empty())
        throw std::runtime_error("Static mesh preparation requires static geometry.");
    if (!vertices_per_resource && !texture_edge)
        result.push_back(Mesh::compile(source));
    else {
        const auto vertex_limit =
            vertices_per_resource ? vertices_per_resource : std::numeric_limits<std::size_t>::max();
        // Bound individual uploads while preserving geometry and placement.
        // Limits are supplied by the consumer; the immutable source is preserved.
        Asset chunk;
        chunk.nodes = source.nodes;
        chunk.materials = source.materials;
        chunk.textures = source.textures;
        for (std::size_t texture_id = 0; texture_id < chunk.textures.size(); ++texture_id) {
            auto &texture = chunk.textures[texture_id];
            if (!texture_edge || std::max(texture.width, texture.height) <= texture_edge)
                continue;
            TextureMipOptions mip_options;
            for (const auto &material : chunk.materials)
                if (material.texture == static_cast<int>(texture_id) && material.alpha_mode == AlphaMode::mask &&
                    material.alpha > 0)
                    mip_options.alpha_coverage_cutoff = std::clamp(material.alpha_cutoff / material.alpha, .001F, 1.F);
            auto mips = texture_mips(texture, mip_options);
            const auto found = std::find_if(mips.begin(), mips.end(),
                                            [&](const auto &m) { return std::max(m.width, m.height) <= texture_edge; });
            if (found != mips.end()) {
                texture.width = found->width;
                texture.height = found->height;
                texture.rgba = std::move(found->rgba);
            }
        }
        std::size_t vertices = 0;
        const auto flush = [&] {
            if (chunk.primitives.empty())
                return;
            Asset batch;
            batch.nodes = chunk.nodes;
            batch.primitives = std::move(chunk.primitives);
            std::map<int, int> materials, textures;
            const auto texture = [&](int &id) {
                if (id < 0)
                    return;
                auto [it, inserted] = textures.emplace(id, static_cast<int>(batch.textures.size()));
                if (inserted)
                    batch.textures.push_back(chunk.textures.at(id));
                id = it->second;
            };
            for (auto &primitive : batch.primitives) {
                if (primitive.material < 0)
                    continue;
                auto [it, inserted] = materials.emplace(primitive.material, static_cast<int>(batch.materials.size()));
                if (inserted) {
                    auto material = chunk.materials.at(primitive.material);
                    texture(material.texture);
                    texture(material.normal_texture);
                    texture(material.metallic_roughness_texture);
                    texture(material.emissive_texture);
                    texture(material.occlusion_texture);
                    batch.materials.push_back(std::move(material));
                }
                primitive.material = it->second;
            }
            result.push_back(Mesh::compile(batch));
            chunk.primitives.clear();
            vertices = 0;
        };
        for (const auto &primitive : source.primitives) {
            // Material changes split only under a vertex limit; without one the geometry stays in one Mesh.
            if (vertices_per_resource && !chunk.primitives.empty() &&
                chunk.primitives.back().material != primitive.material)
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
                chunk.primitives.push_back(std::move(part));
                vertices += count;
                first += count;
            }
        }
        flush();
        // A source without primitives still compiles, as compile() does, into one Mesh of its nodes.
        if (result.empty()) {
            Asset nodes;
            nodes.nodes = source.nodes;
            result.push_back(Mesh::compile(nodes));
        }
    }
    return result;
}
} // namespace anima
