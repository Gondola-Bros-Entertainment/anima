#pragma once
#include <anima/assets/mesh_snapshot.hpp>

/// @file
/// Immutable compiled meshes, shared by anima::Scene instances and cached on the GPU by
/// anima::VulkanRenderer. Part of the `anima::assets` target.

namespace anima {
/// Axis-aligned bounding box; Scene reports instance and part bounds in world space.
struct RenderBounds {
    Vec3 minimum{};
    Vec3 maximum{};
    /// False when the box is empty or unknown. Culling never rejects an invalid box.
    bool valid{};
};
/// One indexed triangle-list draw of a Mesh, compiled from one source primitive. Its position in
/// Mesh::draws() is the primitive index that Scene::set_primitive_visible takes.
struct IndexedDraw {
    /// Offset of the draw's first index in Mesh::indices().
    std::uint32_t first_index{};
    /// Number of indices, three per triangle.
    std::uint32_t index_count{};
    /// First palette matrix the draw uses: its node's matrix when rigid, its skin's first joint matrix when
    /// skinned. See Mesh::palette_size().
    std::uint32_t palette_offset{};
    /// Whether each vertex blends up to four joint matrices, with SourceVertex::joints counted from
    /// #palette_offset, instead of using its node's matrix.
    bool skinned{};
    /// Index into the material data of Mesh::materials(), which Scene material factor overrides also use, or
    /// -1 for a default Material.
    int material = -1;
    /// Source node index; #node_name and #mesh_name name the source node and mesh.
    std::uint32_t node{};
    std::string node_name;
    std::string mesh_name;
};

/// Limits for Mesh::compile_static; with #max_vertices zero the source compiles into one Mesh.
struct MeshCompileOptions {
    /// Maximum triangle corners, and so vertices and indices, in each resulting Mesh; zero for no limit, or
    /// at least 3. A nonzero limit also gives each Mesh the primitives of only one material.
    std::size_t max_vertices{};
    /// Maximum texture width and height. A larger texture is replaced by its first mip level that fits, from
    /// texture_mips(). A masked base color keeps its alpha coverage at the cutoff `alpha_cutoff / alpha` when
    /// that is in (0, 1], the rule material_texture_plan() applies to mip chains, and uses that need different
    /// cutoffs, or none, get separate copies. Zero keeps authored sizes.
    unsigned max_texture_edge{};
};

/// Immutable compiled render mesh: indexed geometry, draws, materials, textures and skins.
///
/// Compile once and share the pointer: each Scene instance keeps its own pose, material factors and
/// visibility, while VulkanRenderer caches GPU geometry and textures per Mesh object. To change geometry,
/// textures or material constants, compile a new Mesh. Nothing changes after compilation, so a Mesh may be
/// read from several threads.
class Mesh {
  public:
    /// Imports the GLB file at @p path with load_asset() and compiles it; throws what either throws.
    [[nodiscard]] static std::shared_ptr<const Mesh> load(const std::filesystem::path &path) {
        return compile(*load_asset(path));
    }
    /// Validates @p source and copies it into a new Mesh; @p source may change or be destroyed afterwards.
    ///
    /// Each source primitive becomes one IndexedDraw, in order. Within a primitive, vertices whose attributes
    /// are all bit-identical are merged, so seams and triangle order are preserved and nothing is simplified.
    /// Throws `std::invalid_argument` for invalid content, for example nonfinite attributes, vertex alpha
    /// outside [0, 1], a tangent `w` other than -1, 0 or 1, skin weights that are negative or do not sum to 1
    /// within `0.0001`, a skin with more than 512 joints, material factors outside [0, 1], a texture whose
    /// encoding does not suit its use, or a transform that is not affine; anima::MathError for a rest rotation
    /// that cannot be normalized; and `std::runtime_error` for a cyclic, dangling or nonfinite node hierarchy.
    [[nodiscard]] static std::shared_ptr<const Mesh> compile(const Asset &source);
    /// Compiles a static @p source into one or more meshes that together draw its geometry with the same node
    /// placement, to bound individual uploads.
    ///
    /// With both limits in @p options zero this returns `{compile(source)}`. Otherwise it first shrinks
    /// oversized textures. Without a vertex limit it then returns one Mesh; with one, it starts a new Mesh at
    /// every material change between consecutive primitives and whenever MeshCompileOptions::max_vertices would
    /// be exceeded, splitting primitives between whole triangles. Each result keeps every node but only the
    /// materials and textures it uses; a source without primitives gives one Mesh with only its nodes. Throws
    /// `std::invalid_argument` for a `max_vertices` of 1 or 2 or a primitive that is not a nonempty list of
    /// whole triangles, `std::runtime_error` when @p source has skins or animations, and what compile() throws.
    [[nodiscard]] static std::vector<std::shared_ptr<const Mesh>> compile_static(const Asset &source,
                                                                                 MeshCompileOptions options = {});
    /// Vertices that indices() refers to.
    [[nodiscard]] std::span<const SourceVertex> vertices() const { return vertices_; }
    /// Triangle-list indices into vertices(), in source order.
    [[nodiscard]] std::span<const std::uint32_t> indices() const { return indices_; }
    /// One draw per source primitive, in source order.
    [[nodiscard]] std::span<const IndexedDraw> draws() const { return draws_; }
    /// Source materials, textures and metadata; its vertices and primitives are empty.
    [[nodiscard]] const std::shared_ptr<const MeshSnapshot> &materials() const { return materials_; }
    /// Pose from the source's rest transforms, used by instances without an explicit pose.
    [[nodiscard]] const Pose &rest_pose() const { return rest_; }
    /// Matrices in each instance palette: one per source node, then one per joint of each skin, in order.
    [[nodiscard]] std::size_t palette_size() const { return palette_size_; }
    /// Whether @p source can animate this mesh: the same node names and parents in the same order, and rest
    /// world transforms within `0.00001` per element. Animator requires it. Throws as sample_pose() does when
    /// the rest pose of @p source cannot be evaluated.
    [[nodiscard]] bool accepts_animation_source(const Asset &source) const;

  private:
    friend class Scene;
    struct BoundPart {
        std::uint32_t palette;
        RenderBounds bound;
    };
    std::vector<SourceVertex> vertices_;
    std::vector<std::uint32_t> indices_;
    std::vector<IndexedDraw> draws_;
    std::shared_ptr<const MeshSnapshot> materials_;
    Pose rest_;
    std::vector<std::pair<std::string, int>> nodes_;
    std::vector<AssetSkin> skins_;
    std::vector<std::vector<BoundPart>> bounds_;
    std::size_t palette_size_{};
};

} // namespace anima
