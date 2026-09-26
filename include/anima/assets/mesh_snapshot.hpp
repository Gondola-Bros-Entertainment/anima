#pragma once
#include <anima/assets/asset.hpp>

/// @file
/// CPU-posed geometry snapshots for inspection, validation and reference output.
///
/// Part of the `anima::assets` target. Rendering uses immutable Mesh resources instead; snapshots
/// expand every triangle corner and pose it on the CPU.

namespace anima {
/// One posed vertex.
struct MeshVertex {
    /// Posed position.
    Vec3 position;
    /// Posed unit normal.
    Vec3 normal;
    /// Vertex color multiplied by the material's base-color factor.
    Vec3 color;
    std::array<float, 2> uv{};
    /// Posed tangent direction and handedness w, negated by mirroring transforms; all zero when the
    /// source has no tangent.
    std::array<float, 4> tangent{};
    /// Vertex alpha; the material alpha is not applied.
    float alpha = 1;
};
/// One draw range of a MeshSnapshot.
struct Primitive {
    /// Name of the node that instances the mesh.
    std::string node_name;
    /// Name of the glTF mesh.
    std::string mesh_name;
    /// Material name, or `default` when the primitive has none.
    std::string material_name;
    /// World matrix of the instancing node in the snapshot's pose.
    Mat4 node_world{};
    /// Index of the first vertex in MeshSnapshot::vertices.
    std::uint32_t first_vertex{};
    /// Number of vertices, a positive multiple of 3.
    std::uint32_t vertex_count{};
    /// Index into MeshSnapshot::material_data, or -1.
    int material_index = -1;
    /// Whether the primitive was visible when Scene::snapshot captured it; make_mesh_snapshot
    /// always sets true.
    bool visible = true;
};
/// Posed triangles of one or more assets, with their materials and statistics.
struct MeshSnapshot {
    /// Three vertices per triangle.
    std::vector<MeshVertex> vertices;
    std::vector<Primitive> primitives;
    /// Materials that Primitive::material_index refers to.
    std::vector<Material> material_data;
    /// Textures that the materials refer to.
    std::vector<Texture> textures;
    /// Minimum corner of the posed vertices' bounding box; Scene::snapshot counts visible
    /// primitives only.
    Vec3 minimum{};
    /// Maximum corner of the bounding box; see #minimum.
    Vec3 maximum{};
    /// Number of mesh-bearing nodes.
    std::size_t mesh_nodes{};
    /// Number of materials.
    std::size_t materials{};
    /// Number of skins.
    std::size_t skins{};
    /// Total joints over all skins.
    std::size_t joints{};
    /// Number of vertices in skinned primitives.
    std::size_t skinned_vertices{};
    /// Clip names.
    std::vector<std::string> clips;
    /// Import notices.
    std::vector<std::string> notices;
    /// Largest absolute element difference from identity of any joint's rest world matrix times
    /// its inverse bind matrix.
    float bind_deviation{};
    /// Whether #bind_deviation is below `2e-5`, so the rest pose is the bind pose.
    bool default_is_bind_pose = true;
};
/// Snapshot of @p asset posed by @p pose on the CPU, skinning with up to four joints per vertex.
/// Throws `std::runtime_error` unless @p pose has one world matrix per node, and for a singular
/// transform or nonfinite result.
[[nodiscard]] MeshSnapshot make_mesh_snapshot(const Asset &asset, const Pose &pose);
/// Overwrites the vertices of @p asset's primitives in @p scene, starting at vertex
/// @p first_vertex, with @p asset posed by @p pose and then transformed by @p attachment.
///
/// Draw ranges of @p scene that start where one of the primitives is written get that primitive's
/// node matrix; topology and materials are unchanged. Throws `std::runtime_error` unless @p pose
/// has one world matrix per node and for a singular transform or nonfinite result, and
/// `std::out_of_range` when the vertices do not fit. A failure can leave the range partly written.
void pose_mesh_snapshot(const Asset &asset, const Pose &pose, MeshSnapshot &scene, std::size_t first_vertex = 0,
                        const Mat4 &attachment = identity());
/// Imports the GLB at @p path with load_asset and snapshots its rest pose. Needs no manifest or
/// playback metadata.
[[nodiscard]] MeshSnapshot load_glb(const std::filesystem::path &path);
/// Writes a human-readable summary of @p scene to `std::cout`.
void print_mesh_report(const MeshSnapshot &scene);
} // namespace anima
