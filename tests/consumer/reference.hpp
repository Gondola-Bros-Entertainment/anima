#pragma once
#include <anima/scene.hpp>

// CPU-deformed diagnostic geometry enters the ordinary indexed renderer as a
// static asset. Expected poses come from make_mesh_snapshot/pose_mesh_snapshot, independently of
// Scene palettes and GPU skinning; no alternate renderer is involved.
namespace reference_test {
inline std::shared_ptr<anima::Scene> scene(std::span<const anima::MeshSnapshot> snapshots) {
    auto result = std::make_shared<anima::Scene>();
    for (const auto &snapshot : snapshots) {
        anima::Asset asset;
        asset.nodes.resize(1);
        asset.materials = snapshot.material_data;
        asset.textures = snapshot.textures;
        // CPU deformation already applied the material's RGB factor.
        for (auto &material : asset.materials)
            material.factor = {1, 1, 1};
        for (const auto &draw : snapshot.primitives) {
            anima::SourcePrimitive primitive;
            primitive.material = draw.material_index;
            primitive.mesh_name = draw.mesh_name;
            for (std::size_t i = draw.first_vertex; i < draw.first_vertex + draw.vertex_count; ++i) {
                const auto &input = snapshot.vertices.at(i);
                anima::SourceVertex vertex;
                vertex.position = input.position;
                vertex.normal = input.normal;
                vertex.color = input.color;
                vertex.uv = input.uv;
                vertex.tangent = input.tangent;
                vertex.alpha = input.alpha;
                primitive.vertices.push_back(vertex);
            }
            asset.primitives.push_back(std::move(primitive));
        }
        const auto id = result->add(anima::Mesh::compile(asset));
        for (std::size_t i = 0; i < snapshot.primitives.size(); ++i)
            result->set_primitive_visible(id, i, snapshot.primitives[i].visible);
    }
    return result;
}
inline std::shared_ptr<anima::Scene> scene(const anima::MeshSnapshot &snapshot) {
    return scene(std::span(&snapshot, 1));
}
} // namespace reference_test
