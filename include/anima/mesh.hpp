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
/// A simplified version of an IndexedDraw's triangles: a subset of its vertices, joined into fewer triangles, which
/// VulkanRenderer draws in its place where the error it adds would cover less than its LOD threshold on screen
/// (RendererOptions::lod_threshold).
struct DrawLevel {
    /// Offset of the level's first index in Mesh::indices().
    std::uint32_t first_index{};
    /// Number of indices, three per triangle, fewer than the level before it has.
    std::uint32_t index_count{};
    /// The largest error of the simplification steps that produced the level, in the units of the draw's vertices
    /// before their node or joints place them, so at least the error of the level before it. A step's error combines
    /// how far it may move the surface it starts from with how much it changes normals and vertex colors, which the
    /// simplifier weighs as distance and clamps to the scale of the positional error.
    float error{};
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
    /// Simplified levels of detail, each coarser than the one before (MeshLodOptions); empty draws only this one.
    std::vector<DrawLevel> levels{};
};

/// Levels of detail that Mesh::compile generates for each draw, as Godot generates them on import.
///
/// Each level simplifies the one before it, starting from the draw, with meshoptimizer's quadric simplifier, which
/// collapses the edges that change the surface, normals and vertex colors least and keeps the seams where those
/// attributes split, toward half the triangles. Every level keeps the draw's open border whole, so draws that meet
/// along it, such as the material subsets of one mesh or the pieces of Mesh::compile_static, meet without cracks
/// whichever levels are drawn for each. Generation stops early when a level would keep more than 85% of the indices
/// of the one before it or when a step's error would exceed the draw's extent. Draws with a masked material
/// (AlphaMode::mask) get none: their cutout edges follow texture coordinates, which the simplifier does not weigh, so
/// foliage needs impostors instead. Each level takes its indices' memory and upload in addition to the draw's.
struct MeshLodOptions {
    /// Most levels per draw, from 0, the default, which generates none, to 8.
    std::size_t levels{};
};

/// Limits for Mesh::compile_static; with #max_vertices zero the source compiles into one Mesh.
struct MeshCompileOptions {
    /// Maximum triangle corners, and so vertices and indices, in each resulting Mesh; zero for no limit, or
    /// at least 3. A nonzero limit also gives each Mesh the primitives of only one material.
    std::size_t max_vertices{};
    /// Maximum texture width and height. A larger texture is replaced by its first mip level that fits, from
    /// texture_mips(). A masked base color keeps its alpha coverage at the cutoff `alpha_cutoff / alpha` when
    /// that is in (0, 1], and a blended one weights color by alpha, the rules material_texture_plan() applies to
    /// mip chains; uses that need different options, or none, get separate copies. A block-compressed image is
    /// replaced by its first stored level that fits, with the levels after it, whatever its uses, and
    /// Mesh::compile_static throws `std::invalid_argument` ("Block-compressed texture stores no mip level within
    /// the texture limit") when it stores none. Textures that share an image and an encoding share each shrunk
    /// image. Zero keeps authored sizes.
    unsigned max_texture_edge{};
    /// How long each resulting Mesh holds its textures' texels; shrunk images have no other holder, so with
    /// TexelRetention::until_upload they are freed once their Mesh is uploaded.
    TexelRetention texel_retention = TexelRetention::keep;
    /// Levels of detail that each resulting Mesh generates, as compile() does.
    MeshLodOptions lods{};
};

/// Immutable compiled render mesh: indexed geometry, draws, materials, textures and skins.
///
/// Compile once and share the pointer: each Scene instance keeps its own pose, material factors and visibility, while
/// VulkanRenderer caches GPU geometry and textures per Mesh object. To change geometry, textures or material constants,
/// compile a new Mesh. Nothing changes after compilation but the hold of a Mesh compiled with
/// TexelRetention::until_upload on its textures' texels, which is synchronized, so a Mesh may be read from several
/// threads.
class Mesh {
  public:
    /// Imports the GLB file at @p path with load_asset() and compiles it with @p texel_retention; throws what
    /// either throws. With TexelRetention::until_upload nothing else holds the imported images, so they are freed
    /// once the Mesh is uploaded. Calls may run concurrently on any thread, as calls of both functions may.
    [[nodiscard]] static std::shared_ptr<const Mesh> load(const std::filesystem::path &path,
                                                          TexelRetention texel_retention = TexelRetention::keep) {
        return compile(*load_asset(path), texel_retention);
    }
    /// Validates @p source and copies it into a new Mesh, which shares the images of its textures, or with
    /// TexelRetention::until_upload holds them until it is uploaded; @p source may change or be destroyed
    /// afterwards, but the images must not (see Image).
    ///
    /// Each source primitive becomes one IndexedDraw, in order. Within a primitive, vertices whose attributes are all
    /// bit-identical are merged, so seams and triangle order are preserved and nothing is simplified. Throws
    /// `std::invalid_argument` for an unknown @p texel_retention, before anything else, and for invalid content, for
    /// example nonfinite attributes, vertex alpha outside [0, 1], a tangent `w` other than -1, 0 or 1, skin weights
    /// that are negative or do not sum to 1 within `0.0001`, a skin with more than 512 joints, material factors outside
    /// [0, 1], a texture whose encoding does not suit its use or whose image has no texels, or a transform that is not
    /// affine; anima::MathError for a rest rotation that cannot be normalized; and `std::runtime_error` for a cyclic,
    /// dangling or nonfinite node hierarchy.
    ///
    /// Calls may run concurrently on any thread. Each reads @p source, which must not change during the call,
    /// shares or holds its images through their atomic reference counts, and calls no application code.
    [[nodiscard]] static std::shared_ptr<const Mesh> compile(const Asset &source,
                                                             TexelRetention texel_retention = TexelRetention::keep);
    /// Compiles @p source as compile(const Asset &, TexelRetention) does, then gives each draw the levels of detail
    /// that @p lods asks for. Throws what that overload throws, and `std::invalid_argument` for more than 8 levels
    /// ("Mesh LOD levels must be from 0 to 8") before anything else.
    [[nodiscard]] static std::shared_ptr<const Mesh> compile(const Asset &source, TexelRetention texel_retention,
                                                             MeshLodOptions lods);
    /// Compiles a static @p source into one or more meshes that together draw its geometry with the same node
    /// placement, to bound individual uploads.
    ///
    /// With both limits in @p options zero this returns `{compile(source)}`. Otherwise it first shrinks oversized
    /// textures. Without a vertex limit it then returns one Mesh; with one, it starts a new Mesh at every material
    /// change between consecutive primitives and whenever MeshCompileOptions::max_vertices would be exceeded, splitting
    /// primitives between whole triangles; the draws of a split blended primitive sort separately in VulkanRenderer.
    /// Each result keeps every node, Asset::mesh_nodes and Asset::notices, as compile() does, but only the materials
    /// and textures it uses; a source without primitives gives one Mesh with no materials or textures. Throws
    /// `std::invalid_argument` for an unknown MeshCompileOptions::texel_retention, a `max_vertices` of 1 or 2 or a
    /// primitive that is not a nonempty list of whole triangles, and `std::runtime_error` when @p source has skins or
    /// animations, before anything else. Otherwise invalid content anywhere in @p source, including materials and
    /// textures that no primitive uses, fails as compile(source) would: its first defect in compile()'s order throws
    /// the same exception. Limits on the size of one Mesh apply to each result. The results share the images of
    /// @p source's textures, and each shrunk image among themselves, or hold them as
    /// MeshCompileOptions::texel_retention says. Each result generates the levels of detail MeshCompileOptions::lods
    /// asks for, as compile() does, and throws as compile() does for more than 8. Calls may run concurrently on any
    /// thread, as compile() calls may.
    [[nodiscard]] static std::vector<std::shared_ptr<const Mesh>> compile_static(const Asset &source,
                                                                                 MeshCompileOptions options = {});
    /// Vertices that indices() refers to.
    [[nodiscard]] std::span<const SourceVertex> vertices() const { return vertices_; }
    /// Triangle-list indices into vertices(): every draw's own, in source order, then after all of them each draw's
    /// levels of detail (IndexedDraw::levels), draw by draw and level by level.
    [[nodiscard]] std::span<const std::uint32_t> indices() const { return indices_; }
    /// One draw per source primitive, in source order.
    [[nodiscard]] std::span<const IndexedDraw> draws() const { return draws_; }
    /// Source materials, textures and metadata; its vertices and primitives are empty. With
    /// TexelRetention::until_upload its textures' images have no texels; texel_images() returns the images that
    /// have them.
    [[nodiscard]] const std::shared_ptr<const MeshSnapshot> &materials() const { return materials_; }
    /// How this Mesh holds its textures' texels.
    [[nodiscard]] TexelRetention texel_retention() const noexcept { return texel_retention_; }
    /// The image of each texture of materials(), in order, with its texels.
    ///
    /// With TexelRetention::keep these are the textures' own images. With TexelRetention::until_upload they are
    /// the source's images, which this Mesh holds until release_texels() and afterwards finds only while
    /// something else holds them; throws `std::logic_error` ("Mesh texture texels were released after upload")
    /// once one is gone. Renderers and MeshPreparation read texels only through this function. Safe to call from
    /// any thread, also while another thread calls release_texels().
    [[nodiscard]] std::vector<std::shared_ptr<const Image>> texel_images() const;
    /// Ends this Mesh's own hold on the images of TexelRetention::until_upload, as VulkanRenderer does once it has
    /// uploaded the Mesh; does nothing for TexelRetention::keep. Idempotent, and safe to call from any thread.
    ///
    /// Call it only once every renderer that draws this Mesh has uploaded it, unless something else keeps the
    /// images; texel_images() states what later readers find.
    void release_texels() const noexcept;
    /// Pose from the source's rest transforms, used by instances without an explicit pose.
    [[nodiscard]] const Pose &rest_pose() const { return rest_; }
    /// Matrices in each instance palette: one per source node, then one per joint of each skin, in order.
    [[nodiscard]] std::size_t palette_size() const { return palette_size_; }
    /// Bounds of every draw, hidden or not, in the rest pose and the mesh's own space, before an object places it;
    /// invalid when no draw has a vertex. VisibilityRange measures to its center.
    [[nodiscard]] const RenderBounds &rest_bounds() const noexcept { return rest_bounds_; }
    /// Whether @p source can animate this mesh: the same node names and parents in the same order, and rest
    /// world transforms within `0.00001` per element. Animator requires it. Throws as sample_pose() does when
    /// the rest pose of @p source cannot be evaluated.
    [[nodiscard]] bool accepts_animation_source(const Asset &source) const;

  private:
    friend class Scene;
    friend class MeshPlacements;
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
    RenderBounds rest_bounds_;
    TexelRetention texel_retention_ = TexelRetention::keep;
    detail::TexelHoldPtr texels_;
};

} // namespace anima
