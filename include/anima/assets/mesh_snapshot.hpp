#pragma once
#include <anima/assets/asset.hpp>
#include <anima/mesh.hpp>
#include <iosfwd>

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
/// One draw range of a MeshSnapshot: the posed triangles of one SourcePrimitive, or of one copy of a MeshPrimitive.
struct SnapshotPrimitive {
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
    /// Index into MeshSnapshot::materials, or -1.
    int material_index = -1;
    /// Whether the primitive was visible when Scene::snapshot captured it; make_mesh_snapshot
    /// always sets true.
    bool visible = true;
};
/// Posed triangles of one or more assets, with their materials and statistics.
struct MeshSnapshot {
    /// Three vertices per triangle. Corners keep their source order, except that a triangle whose first corner is
    /// posed by a matrix with a negative determinant, such as a scale of (-1, 1, 1), swaps its last two. glTF
    /// specifies that such a matrix reverses winding, and normal() keeps each normal on its surface's outer side, so
    /// the swap keeps every triangle's source winding relative to its normals. A skinned triangle whose corners blend
    /// to matrices of opposite determinant signs follows its first corner, as VulkanRenderer does.
    std::vector<MeshVertex> vertices;
    std::vector<SnapshotPrimitive> primitives;
    /// Materials that SnapshotPrimitive::material_index refers to.
    std::vector<Material> materials;
    /// Textures that the materials refer to. They share their images with the Asset or Mesh they
    /// came from instead of copying them, including images without texels (see TexelRetention).
    std::vector<Texture> textures;
    /// Bounding box of the posed vertices' positions, invalid when there are none; Scene::snapshot counts the vertices
    /// of visible primitives only, so its bounds are invalid when no primitive is visible.
    RenderBounds bounds;
    /// Number of mesh-bearing nodes.
    std::size_t mesh_nodes{};
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
/// Normals follow normal(), so a joint scaled to zero is valid, and corners follow MeshSnapshot::vertices. Throws
/// `std::runtime_error` unless @p pose has one world matrix per node, and for a nonfinite result.
[[nodiscard]] MeshSnapshot make_mesh_snapshot(const Asset &asset, const Pose &pose);
/// Overwrites the vertices of @p asset's primitives in @p snapshot, starting at vertex
/// @p first_vertex, with @p asset posed by @p pose and then transformed by @p attachment.
///
/// Draw ranges of @p snapshot that start where one of the primitives is written get that primitive's
/// node matrix; topology and materials are unchanged. Normals and corners follow make_mesh_snapshot(). Throws
/// `std::runtime_error` unless @p pose has one world matrix per node and for a nonfinite result, and
/// `std::out_of_range` when the vertices do not fit. A failure can leave the range partly written.
void pose_mesh_snapshot(const Asset &asset, const Pose &pose, MeshSnapshot &snapshot, std::size_t first_vertex = 0,
                        const Mat4 &attachment = identity());
/// Imports the GLB at @p path with load_asset and snapshots its rest pose with make_mesh_snapshot(), throwing what
/// either throws. Needs no manifest or playback metadata.
[[nodiscard]] MeshSnapshot load_mesh_snapshot(const std::filesystem::path &path);
/// Writes a human-readable summary of @p snapshot to @p out: a line of its counts, a line of its bounds, which reads
/// `Bounds: none` when they are invalid, then a line per primitive, clip and notice. Uses @p out's formatting and
/// leaves a write failure in its state, throwing only as its exception mask asks.
void print_mesh_report(const MeshSnapshot &snapshot, std::ostream &out);
} // namespace anima
