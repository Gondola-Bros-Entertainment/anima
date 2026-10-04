#pragma once
#include <anima/assets/asset.hpp>
#include <cmath>
#include <cstdint>
#include <optional>

/// @file
/// Immutable compiled meshes, shared by anima::Scene instances and cached on the GPU by
/// anima::VulkanRenderer. Part of the `anima::assets` target.

namespace anima {
/// Axis-aligned bounding box; Scene reports instance and part bounds in world space. center(), extents(), encapsulate()
/// and transformed() operate on it.
struct RenderBounds {
    /// Corner with the smallest coordinates.
    Vec3 minimum{};
    /// Corner with the largest coordinates.
    Vec3 maximum{};
    /// False when the box is empty or unknown. Culling never rejects an invalid box.
    bool valid{};
};
/// Midpoint of the corners of @p bounds, or the origin when @p bounds is invalid. It halves each corner before adding
/// them, so it is finite whenever they are.
[[nodiscard]] inline Vec3 center(const RenderBounds &bounds) noexcept {
    return bounds.valid ? bounds.minimum * .5F + bounds.maximum * .5F : Vec3{};
}
/// Half the size of @p bounds along each axis, so that the box spans center() plus and minus it; zero when @p bounds is
/// invalid. It halves each corner before subtracting them, so it is finite whenever they are.
[[nodiscard]] inline Vec3 extents(const RenderBounds &bounds) noexcept {
    return bounds.valid ? bounds.maximum * .5F - bounds.minimum * .5F : Vec3{};
}
/// Grows @p bounds to the smallest box that holds it and @p position; an invalid @p bounds becomes the valid box of
/// @p position alone.
///
/// Checks no finiteness, so that each caller can reject the finished box with its own message: once @p position has a
/// coordinate that is not finite, the box keeps a corner coordinate on that axis that is not finite through every later
/// call. An infinite coordinate extends the corner on its side to infinity, and a NaN one makes the coordinate of both
/// corners NaN.
inline void encapsulate(RenderBounds &bounds, Vec3 position) noexcept {
    if (!bounds.valid) {
        bounds = {position, position, true};
        return;
    }
    // Each takes a NaN value, and keeps a NaN current coordinate, which no comparison replaces.
    const auto low = [](float current, float value) { return value < current || std::isnan(value) ? value : current; };
    const auto high = [](float current, float value) { return value > current || std::isnan(value) ? value : current; };
    bounds.minimum = {low(bounds.minimum.x, position.x), low(bounds.minimum.y, position.y),
                      low(bounds.minimum.z, position.z)};
    bounds.maximum = {high(bounds.maximum.x, position.x), high(bounds.maximum.y, position.y),
                      high(bounds.maximum.z, position.z)};
}
/// Grows @p bounds to hold @p other as well, encapsulating each of its two corners as encapsulate() does a point; does
/// nothing when @p other is invalid.
inline void encapsulate(RenderBounds &bounds, const RenderBounds &other) noexcept {
    if (!other.valid)
        return;
    encapsulate(bounds, other.minimum);
    encapsulate(bounds, other.maximum);
}
/// The smallest box that holds the eight corners of @p bounds, each transformed by @p matrix as an affine matrix, as
/// point() transforms it; invalid when @p bounds is. Checks no finiteness: a transformed corner with a coordinate that
/// is not finite leaves one in the result, as encapsulate() states.
[[nodiscard]] inline RenderBounds transformed(const RenderBounds &bounds, const Mat4 &matrix) noexcept {
    RenderBounds result;
    if (!bounds.valid)
        return result;
    for (unsigned corner = 0; corner < 8; ++corner)
        encapsulate(result, point(matrix, {corner & 1 ? bounds.maximum.x : bounds.minimum.x,
                                           corner & 2 ? bounds.maximum.y : bounds.minimum.y,
                                           corner & 4 ? bounds.maximum.z : bounds.minimum.z}));
    return result;
}
/// A simplified version of a MeshPrimitive's triangles: a subset of its vertices, joined into fewer triangles, which
/// VulkanRenderer draws in its place where the error it adds would cover at most its LOD threshold in pixels of the
/// scene targets (VulkanRenderer::set_lod_threshold).
struct PrimitiveLevel {
    /// Offset of the level's first index in Mesh::indices().
    std::uint32_t first_index{};
    /// Number of indices, three per triangle, fewer than the level before it has.
    std::uint32_t index_count{};
    /// The sum of the errors of the simplification steps that produced the level, each measured from the level it
    /// started from, in the units of the primitive's vertices before their node or joints place them, so at least the
    /// error of the level before it. It estimates how far the level lies from the primitive's own triangles; each
    /// step's error is itself an estimate, so a level may lie somewhat farther. A step's error combines how far it may
    /// move the surface it starts from with how much it changes normals and vertex colors, which the simplifier weighs
    /// as distance and clamps to the scale of the positional error.
    float error{};
};
/// One indexed triangle list of a Mesh, compiled from one source primitive. Its position in Mesh::primitives() is the
/// primitive index that Scene::set_primitive_visible takes.
struct MeshPrimitive {
    /// Offset of the primitive's first index in Mesh::indices().
    std::uint32_t first_index{};
    /// Number of indices, three per triangle.
    std::uint32_t index_count{};
    /// First palette matrix the primitive uses: its node's matrix when rigid, its skin's first joint matrix when
    /// skinned. See Mesh::palette_size().
    std::uint32_t palette_offset{};
    /// Palette matrices from #palette_offset that may place the primitive's vertices: 1 when rigid, its skin's joint
    /// count when skinned.
    std::uint32_t palette_count{};
    /// Whether each vertex blends up to four joint matrices, with SourceVertex::joints counted from
    /// #palette_offset, instead of using its node's matrix.
    bool skinned{};
    /// Index into MeshDescription::materials of Mesh::description(), which Scene material factor overrides also use,
    /// or -1 for a default Material.
    int material = -1;
    /// Source node index; #node_name and #mesh_name name the source node and mesh.
    std::uint32_t node{};
    std::string node_name;
    std::string mesh_name;
    /// Simplified levels of detail, each coarser than the one before (MeshLodOptions); empty draws only this one.
    std::vector<PrimitiveLevel> levels{};
};

/// Levels of detail that Mesh::compile generates for each primitive, as Godot generates them on import.
///
/// Each level simplifies the one before it, starting from the primitive, with meshoptimizer's quadric simplifier, which
/// collapses the edges that change the surface, normals and vertex colors least, toward half the triangles. It may
/// collapse across a hard edge, where normals or vertex colors split, charging the change to the step's error, so
/// faceted models simplify too, but never across a split in the attributes it does not weigh: texture coordinates,
/// tangent handedness, vertex alpha, and in a skinned primitive joints and weights. A level's triangles are ordered for
/// the vertex cache, except those of a primitive whose own material is AlphaMode::blend, which keep their source order,
/// since they composite in it; a blended custom material (Scene::set_custom_material) composites any other primitive's
/// levels in their cache order. Every level keeps the primitive's open border whole, so primitives that meet along it,
/// such as the material subsets of one mesh or the pieces of Mesh::compile_static, meet without cracks whichever
/// levels are drawn for each. Generation stops early when a level would keep more than 85% of the indices of the one
/// before it or when a step's error would exceed the primitive's extent. Primitives with a masked material
/// (AlphaMode::mask) get none: their cutout edges follow texture coordinates, which the simplifier does not weigh, so
/// foliage draws far away as an impostor instead (bake_impostor()). Each level takes its indices' memory and upload in
/// addition to the primitive's.
struct MeshLodOptions {
    /// Largest #levels.
    static constexpr std::uint32_t max_levels = 8;
    /// Most levels per primitive, from 0, the default, which generates none, to #max_levels.
    std::uint32_t levels{};
};

/// How Mesh::load() and Mesh::compile() compile a source, and how Mesh::compile_static() compiles each Mesh it
/// returns.
struct MeshOptions {
    /// How the Mesh holds its textures' texels, TexelRetention::keep or TexelRetention::until_upload.
    TexelRetention texel_retention = TexelRetention::keep;
    /// Levels of detail that each primitive of the Mesh gets.
    MeshLodOptions lods{};
};

/// The directions from which an impostor's frames view its mesh (ImpostorFrames).
enum class ImpostorLayout {
    /// Directions at or above the mesh's horizontal plane, on a hemi-octahedral grid, for objects seen from above or
    /// level, such as trees; a view from below shows the frames along the horizon.
    hemisphere,
    /// Every direction, on an octahedral grid.
    sphere
};

/// How an impostor's atlas holds its frames, in the space of its source mesh's vertices as the rest pose places them,
/// with +Y up. A persisted atlas keeps these values with its images.
///
/// The atlas holds `n` by `n` frames, where `n` is #frames_per_side, frame `(i, j)` in column `i` and row `j` counted
/// from texture coordinate `(0, 0)`. Frame `(i, j)` views the mesh orthographically from the unit direction `d` that
/// the grid point `(u, v) = (2i / (n - 1) - 1, 2j / (n - 1) - 1)` encodes: for ImpostorLayout::hemisphere,
/// `x = (u + v) / 2`, `z = (u - v) / 2` and `y = 1 - |x| - |z|`; for ImpostorLayout::sphere, `x = u`, `z = v` and
/// `y = 1 - |u| - |v|`, where a negative `y` folds `x` and `z` to `(1 - |z|) sign(x)` and `(1 - |x|) sign(z)`; `d` is
/// `(x, y, z)` normalized. The frame shows the square of side `2 radius` around #center on the plane through #center
/// perpendicular to `d`, with texture `u` along `right` and `v` along `up`: `right` is `(d.z, 0, -d.x)` normalized, or
/// `(1, 0, 0)` where `d.x` and `d.z` are both 0, and `up` is `cross(d, right)`.
struct ImpostorFrames {
    /// Fewest #frames_per_side.
    static constexpr std::uint32_t min_frames_per_side = 2;
    /// Most #frames_per_side.
    static constexpr std::uint32_t max_frames_per_side = 32;
    /// The directions that the frames cover.
    ImpostorLayout layout = ImpostorLayout::hemisphere;
    /// Frames along each side of the atlas, from #min_frames_per_side to #max_frames_per_side.
    std::uint32_t frames_per_side{};
    /// Center of the sphere that holds every vertex, the center of Mesh::rest_bounds().
    Vec3 center{};
    /// Radius of that sphere, finite and greater than 0.
    float radius{};
};
struct ImpostorAtlas;

/// What a Mesh keeps of its source besides geometry and nodes: its materials and textures, and the statistics and
/// import metadata that Scene::snapshot() adds up into a MeshSnapshot. Mesh::description() shares one, which never
/// changes.
struct MeshDescription {
    /// The source's materials, in order; MeshPrimitive::material and Scene's material slots index them.
    std::vector<Material> materials;
    /// The source's textures, in order, which the materials refer to. They share the source's images, or with
    /// TexelRetention::until_upload refer to images with no texels; Mesh::texel_images() returns the images that have
    /// them.
    std::vector<Texture> textures;
    /// Asset::mesh_nodes of the source.
    std::size_t mesh_nodes{};
    /// Number of skins.
    std::size_t skins{};
    /// Total joints over all skins.
    std::size_t joints{};
    /// Triangle corners of the skinned primitives, three per triangle, as many as the vertices a MeshSnapshot expands
    /// them into.
    std::size_t skinned_vertices{};
    /// Names of the source's clips, in order.
    std::vector<std::string> clips;
    /// Asset::notices of the source.
    std::vector<std::string> notices;
    /// Largest absolute element difference from identity of any joint's rest world matrix times its inverse bind
    /// matrix; 0 without skins.
    float bind_deviation{};
    /// Whether #bind_deviation is below `2e-5`, so the rest pose is the bind pose.
    bool default_is_bind_pose = true;
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
    std::uint32_t max_texture_edge{};
    /// How each resulting Mesh holds its textures' texels and which levels of detail it generates, as compile() does
    /// with these options. Shrunk images have no other holder, so with TexelRetention::until_upload they are freed
    /// once their Mesh is uploaded.
    MeshOptions mesh{};
};

/// Immutable compiled render mesh: indexed geometry, primitives, materials, textures and skins.
///
/// Compile once and share the pointer: each Scene instance keeps its own pose, material factors and visibility, while
/// VulkanRenderer caches GPU geometry and textures per Mesh object. To change geometry, textures or material constants,
/// compile a new Mesh. Nothing changes a Mesh but assignment, which the `std::shared_ptr<const Mesh>` that compiling
/// returns does not allow, and the hold of a Mesh compiled with TexelRetention::until_upload on its textures' texels,
/// which is synchronized, so a Mesh may be read from several threads. Only load(), compile(), compile_static(),
/// compile_impostor() and Terrain::compile() create a Mesh, apart from copying one.
class Mesh {
  public:
    /// Most joints in one skin: compile() and load_asset() reject a skin with more, and read_manifest() a larger
    /// joint count.
    static constexpr std::size_t max_skin_joints = 512;
    /// A distinct Mesh with @p other's content, which VulkanRenderer caches and uploads separately. With
    /// TexelRetention::until_upload it holds the images itself until its own release_texels() if @p other had not
    /// released them; otherwise it finds them, as @p other does, only while something else holds them. Only reads
    /// @p other, so it may run while other threads read it or call its release_texels(). Moving a Mesh copies it, so
    /// no Mesh is ever empty.
    Mesh(const Mesh &other) = default;
    /// Replaces this Mesh's content with a copy of @p other's, made as the copy constructor makes one. Whatever uses a
    /// Mesh, such as a Scene, MeshPlacements, MeshPreparation or VulkanRenderer, keeps what it derived from the content
    /// it found, so assign only to a Mesh that nothing else uses, on any thread.
    Mesh &operator=(const Mesh &other) = default;
    /// Imports the GLB file at @p path with load_asset() and compiles it with @p options as compile() does; throws
    /// what either throws, so it reads the file before it checks @p options. With TexelRetention::until_upload
    /// nothing else holds the imported images, so they are freed once the Mesh is uploaded. Calls may run
    /// concurrently on any thread, as calls of both functions may.
    [[nodiscard]] static std::shared_ptr<const Mesh> load(const std::filesystem::path &path, MeshOptions options = {}) {
        return compile(*load_asset(path), options);
    }
    /// Validates @p source and copies it into a new Mesh, which shares the images of its textures, or with
    /// TexelRetention::until_upload in MeshOptions::texel_retention holds them until it is uploaded; @p source may
    /// change or be destroyed afterwards, but the images must not (see Image).
    ///
    /// Each source primitive becomes one MeshPrimitive, in order. Within a primitive, vertices whose attributes are all
    /// bit-identical are merged, so seams and triangle order are preserved and the primitive's own triangles are not
    /// simplified; MeshOptions::lods then adds the simplified levels of detail that MeshLodOptions describes. Throws
    /// `std::invalid_argument` for more than MeshLodOptions::max_levels levels ("Mesh LOD levels must be from 0 to 8"),
    /// before anything else, then for an unknown MeshOptions::texel_retention ("Unknown texel retention"), and for
    /// invalid content, for example nonfinite attributes, vertex alpha outside [0, 1], a tangent `w` other than -1, 0
    /// or 1, skin weights that are negative or do not sum to 1 within `0.0001`, a skin with more than #max_skin_joints
    /// joints, material factors outside [0, 1], a texture whose encoding does not suit its use or whose image has no
    /// texels, or a transform that is not affine; anima::MathError for a rest rotation that cannot be normalized; and
    /// `std::runtime_error` for a cyclic, dangling or nonfinite node hierarchy.
    ///
    /// Calls may run concurrently on any thread. Each reads @p source, which must not change during the call,
    /// shares or holds its images through their atomic reference counts, and calls no application code.
    [[nodiscard]] static std::shared_ptr<const Mesh> compile(const Asset &source, MeshOptions options = {});
    /// Compiles a static @p source into one or more meshes that together draw its geometry with the same node
    /// placement, to bound individual uploads.
    ///
    /// With both limits in @p options zero this returns `{compile(source, options.mesh)}`. Otherwise it first shrinks
    /// oversized textures. Without a vertex limit it then returns one Mesh; with one, it starts a new Mesh at every
    /// material change between consecutive primitives and whenever MeshCompileOptions::max_vertices would be exceeded,
    /// splitting primitives between whole triangles; the pieces of a split blended primitive sort separately in
    /// VulkanRenderer. Each result keeps every node, Asset::mesh_nodes and Asset::notices, as compile() does, but only
    /// the materials and textures it uses; a source without primitives gives one Mesh with no materials or textures.
    /// Throws `std::invalid_argument` for an unknown MeshOptions::texel_retention in MeshCompileOptions::mesh, a
    /// `max_vertices` of 1 or 2 or a primitive that is not a nonempty list of whole triangles, and `std::runtime_error`
    /// when @p source has skins or animations, before anything else. Otherwise invalid content anywhere in @p source,
    /// including materials and textures that no primitive uses, fails as compile(source) would: its first defect in
    /// compile()'s order throws the same exception. Limits on the size of one Mesh apply to each result. The results
    /// share the images of @p source's textures, and each shrunk image among themselves, or hold them as
    /// MeshCompileOptions::mesh says. Each result generates the levels of detail that MeshCompileOptions::mesh asks
    /// for, as compile() does, and throws as compile() does for more than MeshLodOptions::max_levels. Calls may run
    /// concurrently on any thread, as compile() calls may.
    [[nodiscard]] static std::vector<std::shared_ptr<const Mesh>> compile_static(const Asset &source,
                                                                                 MeshCompileOptions options = {});
    /// Compiles @p atlas, from bake_impostor() or persisted from it, into a Mesh that VulkanRenderer draws as an
    /// impostor, and that scenes place, range, cull and shade as any Mesh.
    ///
    /// The Mesh has one node and one primitive, a quad of two triangles whose corners span the cube of side
    /// `2 ImpostorFrames::radius` around ImpostorFrames::center, so its rest bounds share their center with its source
    /// mesh's and a visibility range measures to the same point for both. Its one material is AlphaMode::mask with
    /// cutoff 0.5 and double-sided, with ImpostorAtlas::color as its base color, ImpostorAtlas::normal_depth as its
    /// normal map, ImpostorAtlas::surface as both its metallic-roughness and occlusion maps, and
    /// ImpostorAtlas::emissive, when present, as its emissive map with ImpostorAtlas::emission_scale as its factor.
    /// Every map samples with linear filtering within and between mip levels and clamps at its edges, whatever its
    /// Texture::sampler says, so that no frame wraps across the atlas; mips are sampled, built on the CPU for an
    /// ImageFormat::rgba8 map and read as stored for a block-compressed one. An ImageFormat::rgba8 base color's mips
    /// keep its alpha coverage at 0.5, as a masked material's do. impostor() returns @p atlas's frames.
    ///
    /// Throws `std::invalid_argument` for frames out of range ("Impostor frames must number from 2 to 32 per side, with
    /// a finite center and a positive finite radius"), for an unknown layout ("Unknown impostor layout"), and for
    /// images that are not square, equal in size and divisible into the frames, or whose textures use the wrong
    /// encoding ("Impostor images must be equal squares divisible into the frames, color and emission sRGB, normals
    /// and surface linear"), for an emission scale below 1 or not finite ("Impostor emission scale must be finite and
    /// at least 1"), then as compile() does for its textures, and for an unknown @p texel_retention. Calls may run
    /// concurrently on any thread.
    [[nodiscard]] static std::shared_ptr<const Mesh>
    compile_impostor(const ImpostorAtlas &atlas, TexelRetention texel_retention = TexelRetention::keep);
    /// Vertices that indices() refers to.
    [[nodiscard]] std::span<const SourceVertex> vertices() const { return vertices_; }
    /// Triangle-list indices into vertices(): every primitive's own, in source order, then after all of them each
    /// primitive's levels of detail (MeshPrimitive::levels), primitive by primitive and level by level.
    [[nodiscard]] std::span<const std::uint32_t> indices() const { return indices_; }
    /// One primitive per source primitive, in source order.
    [[nodiscard]] std::span<const MeshPrimitive> primitives() const { return primitives_; }
    /// The materials, textures and import metadata of the source, never null. With TexelRetention::until_upload its
    /// textures' images have no texels; texel_images() returns the images that have them. Copies of this Mesh share
    /// it.
    [[nodiscard]] const std::shared_ptr<const MeshDescription> &description() const noexcept { return description_; }
    /// How this Mesh holds its textures' texels.
    [[nodiscard]] TexelRetention texel_retention() const noexcept { return texel_retention_; }
    /// The image of each texture of description(), in order, with its texels.
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
    /// Bounds of every primitive, hidden or not, in the rest pose and the mesh's own space, before an object places it;
    /// invalid when no primitive has a vertex. VisibilityRange measures to its center.
    [[nodiscard]] const RenderBounds &rest_bounds() const noexcept { return rest_bounds_; }
    /// Whether @p source can animate this mesh: the same node names and parents in the same order, and rest
    /// world transforms within `0.00001` per element. Animator requires it. Throws as sample_pose() does when
    /// the rest pose of @p source cannot be evaluated.
    [[nodiscard]] bool accepts_animation_source(const Asset &source) const;
    /// The frames of a Mesh compiled with compile_impostor(), which VulkanRenderer draws as an impostor; null for any
    /// other Mesh.
    [[nodiscard]] const ImpostorFrames *impostor() const noexcept { return impostor_ ? &*impostor_ : nullptr; }

  private:
    friend class Scene;
    friend class MeshPlacements;
    friend class Terrain;
    // An empty Mesh, whose null description_ Scene and texel_images() would dereference; only compile_indexed() starts
    // from one.
    Mesh() = default;
    // compile(source, options), except that nonempty indices hold one triangle list per source primitive, indexing
    // that primitive's vertices, which are copied as they are instead of welded. Every vertex must be referenced,
    // since the bounds cover them all. Terrain compiles its samples, distinct by construction, this way.
    static std::shared_ptr<const Mesh>
    compile_indexed(const Asset &source, std::span<const std::vector<std::uint32_t>> indices, MeshOptions options);
    struct BoundPart {
        std::uint32_t palette;
        RenderBounds bound;
    };
    std::vector<SourceVertex> vertices_;
    std::vector<std::uint32_t> indices_;
    std::vector<MeshPrimitive> primitives_;
    std::shared_ptr<const MeshDescription> description_;
    Pose rest_;
    std::vector<std::pair<std::string, int>> nodes_;
    std::vector<AssetSkin> skins_;
    std::vector<std::vector<BoundPart>> bounds_;
    std::size_t palette_size_{};
    RenderBounds rest_bounds_;
    TexelRetention texel_retention_ = TexelRetention::keep;
    detail::TexelHoldPtr texels_;
    std::optional<ImpostorFrames> impostor_;
};

} // namespace anima
