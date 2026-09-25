#pragma once
#include <anima/assets/asset.hpp>

namespace anima {
struct MeshVertex {
    Vec3 position, normal, color;
    std::array<float, 2> uv{};
    std::array<float, 4> tangent{};
    float alpha = 1;
};
struct Primitive {
    std::string node_name, mesh_name, material_name;
    Mat4 node_world{};
    std::uint32_t first_vertex{}, vertex_count{};
    int material_index = -1;
    bool visible = true;
};
struct MeshSnapshot {
    std::vector<MeshVertex> vertices;
    std::vector<Primitive> primitives;
    std::vector<Material> material_data;
    std::vector<Texture> textures;
    Vec3 minimum{}, maximum{};
    std::size_t mesh_nodes{}, materials{}, skins{}, joints{}, skinned_vertices{};
    std::vector<std::string> clips, notices;
    float bind_deviation{};
    bool default_is_bind_pose = true;
};
[[nodiscard]] MeshSnapshot make_mesh_snapshot(const Asset &asset, const Pose &pose);
// Writes a pose into an existing scene range without changing its topology/materials.
void pose_mesh_snapshot(const Asset &asset, const Pose &pose, MeshSnapshot &scene, std::size_t first_vertex = 0,
                        const Mat4 &attachment = identity());
// Static preview stays available independently of playback/equipment metadata.
[[nodiscard]] MeshSnapshot load_glb(const std::filesystem::path &path);
void print_mesh_report(const MeshSnapshot &scene);
} // namespace anima
