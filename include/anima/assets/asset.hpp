#pragma once
#include <anima/assets/staging.hpp>
#include <anima/core/transform.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// CPU asset data: GLB import, node poses, clips and texture mip chains.
///
/// Part of the `anima::assets` target (`ANIMA_BUILD_ASSETS`, on by default); the glTF and image
/// decoders are private. Coordinates follow glTF: right-handed and +Y up, in the file's units
/// (meters in glTF). Rotations are XYZW quaternions, local transforms compose as `T * R * S`,
/// matrices are column-major and times are in seconds. Imported assets are shared as
/// `std::shared_ptr<const Asset>` and own all their data; decoded images are shared through
/// Texture::image.

namespace anima {
/// The value of an `int` index field that refers to nothing, such as AssetNode::parent of a root
/// or SourcePrimitive::material of a primitive without a material. Valid indices are at least 0.
inline constexpr int no_index = -1;
/// One glTF node.
struct AssetNode {
    /// Node name, or `(unnamed)` when the file gives none. Names need not be unique; see
    /// find_node.
    std::string name;
    /// Index of the parent node, or no_index for a root.
    int parent = no_index;
    /// Local rest translation, rotation and scale. For a node authored as a matrix, the matrix's
    /// decomposition, which glTF requires to exist: a mirrored matrix has a negative X scale, and one
    /// with a zero-scale axis keeps the identity rotation. Pose world matrices of such a node use
    /// #rest_matrix; motion transfer and other readers of local TRS use this decomposition.
    Transform rest;
    /// Whether the node was authored as a matrix. Such nodes keep #rest_matrix in every pose and
    /// cannot be animated.
    bool has_matrix{};
    /// Local matrix used instead of #rest when #has_matrix is set.
    Mat4 rest_matrix = identity();
};
/// A glTF skin: the joint palette that skinned vertices index.
struct AssetSkin {
    /// Node index of each palette joint, in file order.
    std::vector<std::size_t> joints;
    /// Inverse bind matrix of each joint, parallel to #joints; identity when the file has none.
    std::vector<Mat4> inverse_bind;
};
/// One vertex of an expanded source triangle, in the space of the node that instances the mesh.
struct SourceVertex {
    /// Position in mesh space.
    Vec3 position{};
    /// Normal from the file, or the triangle's face normal when the primitive has none.
    Vec3 normal{};
    /// Linear `COLOR_0` RGB, white when absent.
    Vec3 color{1, 1, 1};
    /// Texture coordinates from the UV set that the material's maps share; zero when the material
    /// uses no maps.
    std::array<float, 2> uv{};
    /// Up to four indices into the primitive's skin joint palette (`JOINTS_0`); zero when unskinned.
    std::array<std::uint32_t, 4> joints{};
    /// Weights of #joints (`WEIGHTS_0`), normalized to sum to 1; zero when unskinned.
    std::array<float, 4> weights{};
    /// glTF tangent xyz and handedness w (1 or -1), kept through skinning and mirroring transforms.
    /// When the file has no tangent, or no normals, whose generated flat normals make glTF ignore
    /// supplied tangents, w is 0 and the renderer derives a frame from screen-space derivatives,
    /// leaving the normal unmapped where the UVs are degenerate.
    std::array<float, 4> tangent{};
    /// `COLOR_0` alpha, 1 when absent or RGB only.
    float alpha = 1;
};
/// One glTF primitive as an unindexed triangle list.
struct SourcePrimitive {
    /// Name of the glTF mesh that holds the primitive.
    std::string mesh_name;
    /// Index of the node that instances the mesh.
    std::size_t node{};
    /// Index into Asset::skins, or no_index when unskinned.
    int skin = no_index;
    /// Index into Asset::materials, or no_index for none. Imported primitives without a material use an
    /// appended `glTF default` material (metallic 1, roughness 1).
    int material = no_index;
    /// Three vertices per triangle, in file order. Posing never modifies them.
    std::vector<SourceVertex> vertices;
};
/// Texture filter.
enum class Filter {
    nearest, ///< Nearest texel, or nearest mip level.
    linear   ///< Linear interpolation between texels or mip levels.
};
/// Texture coordinate wrap mode.
enum class Wrap {
    repeat, ///< glTF `REPEAT`.
    clamp,  ///< glTF `CLAMP_TO_EDGE`.
    mirror  ///< glTF `MIRRORED_REPEAT`.
};
/// Color encoding of a texture's RGB channels; alpha is always linear.
enum class TextureEncoding {
    srgb,  ///< Color data, decoded from sRGB when sampled and when filtering mips.
    linear ///< Data maps, sampled and filtered as stored.
};
/// Texture sampling state, from the glTF sampler.
struct Sampler {
    /// Magnification filter.
    Filter mag = Filter::linear;
    /// Minification filter.
    Filter min = Filter::linear;
    /// Filter between mip levels.
    Filter mip = Filter::linear;
    /// Wrap mode along U.
    Wrap u = Wrap::repeat;
    /// Wrap mode along V.
    Wrap v = Wrap::repeat;
    /// Whether the texture is sampled through a mip chain; false when the glTF minification filter
    /// is `NEAREST` or `LINEAR`.
    bool mipmapped = true;
};
/// How an Image stores its texels.
enum class ImageFormat {
    /// 8-bit RGBA in Image::rgba, one level; texture_mips() builds a mip chain from it. load_asset()
    /// decodes PNG and JPEG images to it.
    rgba8,
    /// BC7 blocks in Image::blocks, with every mip level that the image stores. Each block holds 4x4
    /// texels in 16 bytes, one byte per texel, as the Khronos Data Format Specification defines BC7
    /// (BPTC); a texture's encoding decides whether its RGB is sRGB or linear. The blocks upload as
    /// they are, and no filtering changes them: an application's content pipeline encodes them, and
    /// their mip levels, beforehand. load_ktx2() reads them from KTX 2.0 files.
    bc7
};
/// Most texels along each edge of an image that load_asset() decodes from a GLB or load_ktx2()
/// reads.
inline constexpr std::uint32_t max_image_edge = 8192;
/// Most texels in an image that load_asset() decodes from a GLB or load_ktx2() reads, 16,777,216:
/// 4096 by 4096, or 2048 across an image max_image_edge long.
inline constexpr std::size_t max_image_texels = std::size_t{16} * 1024 * 1024;
/// Texels that textures share by identity.
///
/// Textures hold an image through `std::shared_ptr<const Image>`, and the pointer is its identity:
/// copying a Texture, an Asset or a MeshSnapshot, or compiling a Mesh with TexelRetention::keep,
/// shares the texels instead of copying them, and the image lives until its last holder releases
/// it. Texels must not change once a Texture refers to them, even through another pointer to the
/// same Image: meshes compiled from the texture read them without synchronization, from any
/// thread. Make a new Image to change them.
///
/// An image whose #rgba and #blocks are both empty has no texels: it only describes a texture's
/// dimensions, format and levels. The textures of a Mesh or CustomMaterial compiled with
/// TexelRetention::until_upload refer to such images, and validate_scene() accepts them, but
/// compiling a Mesh or CustomMaterial, texture_mips(), decode_image() and uploads need texels.
struct Image {
    /// Width in texels.
    std::uint32_t width{};
    /// Height in texels.
    std::uint32_t height{};
    /// For ImageFormat::rgba8, `width * height * 4` bytes of 8-bit RGBA, row by row, starting at
    /// texture coordinate `v = 0`, or none; empty for other formats.
    std::vector<std::uint8_t> rgba;
    /// How the texels are stored.
    ImageFormat format = ImageFormat::rgba8;
    /// Mip levels stored, the first at #width by #height and each after it half the size of the one
    /// before, rounding down to at least 1: 1 for ImageFormat::rgba8, and from 1 to the full chain
    /// down to 1x1 for ImageFormat::bc7.
    std::uint32_t levels = 1;
    /// For ImageFormat::bc7, the blocks of each of #levels, the base level first: a level of `w` by
    /// `h` texels holds `ceil(w / 4) * ceil(h / 4)` blocks of 16 bytes, row by row from `v = 0`, and
    /// texels past the level's edge in its last blocks are unused. Empty for ImageFormat::rgba8 and
    /// for an image without texels.
    std::vector<std::uint8_t> blocks{};
};
/// How long a compiled Mesh or CustomMaterial holds the texels of its textures' images.
///
/// The texels are needed only until a renderer has uploaded them, but a Mesh or CustomMaterial
/// lives as long as the scenes that draw it. With TexelRetention::until_upload it lets them go once
/// uploaded, as a texture whose Read/Write setting is off does in Unity and a texture without CPU
/// access does in Unreal: the images are then freed once nothing else holds them. The application
/// keeps an image readable by keeping it, for example in the Asset that the Mesh was compiled from.
///
/// Objects that keep a whole Asset, such as Animator, MotionRuntime and a FittedLibrary's body,
/// keep its images too. To let them go, give such objects a copy of the Asset whose textures are
/// those of a Mesh compiled from it with TexelRetention::until_upload, which have no texels.
enum class TexelRetention {
    /// For its whole lifetime: its textures share its source's images, texels included.
    keep,
    /// Until a VulkanRenderer has uploaded it.
    ///
    /// Its textures refer to images without texels, one for each image of its source, that describe
    /// their dimensions, format and levels, and it holds the source's images privately. Once a
    /// VulkanRenderer has uploaded it, it holds them only weakly: they stay readable through
    /// Mesh::texel_images() or CustomMaterial::texel_images() while anything else holds them, such
    /// as the source Asset or another Mesh not uploaded yet, and are freed with their last holder.
    /// Uploading it again, in another VulkanRenderer or in one created after a RendererFatalError,
    /// and preparing it with MeshPreparation, then need those images, and throw
    /// `std::logic_error` once they are gone.
    until_upload
};
namespace detail {
class TexelHold;
// The hold of a Mesh or CustomMaterial on the images of TexelRetention::until_upload, empty for
// TexelRetention::keep. A copy holds the same images on its own, as the copied object then does.
class TexelHoldPtr {
  public:
    TexelHoldPtr() noexcept;
    explicit TexelHoldPtr(std::unique_ptr<TexelHold> hold) noexcept;
    TexelHoldPtr(const TexelHoldPtr &other);
    TexelHoldPtr &operator=(const TexelHoldPtr &other);
    TexelHoldPtr(TexelHoldPtr &&other) noexcept;
    TexelHoldPtr &operator=(TexelHoldPtr &&other) noexcept;
    ~TexelHoldPtr();
    [[nodiscard]] TexelHold *get() const noexcept { return hold_.get(); }

  private:
    std::unique_ptr<TexelHold> hold_;
};
} // namespace detail
/// A shared decoded image with its sampler and encoding.
struct Texture {
    /// The image, shared with every texture that uses it; see Image for images without texels.
    /// Textures that share an image may sample and encode it differently. Validation rejects a null
    /// image as it rejects a zero dimension.
    std::shared_ptr<const Image> image;
    /// Filtering, wrapping and mip use when the image is sampled.
    Sampler sampler;
    /// The importer sets TextureEncoding::srgb for base-color and emissive maps and
    /// TextureEncoding::linear for the others.
    TextureEncoding encoding = TextureEncoding::srgb;
};
/// One level of a mip chain.
struct MipLevel {
    /// Width in texels.
    std::uint32_t width{};
    /// Height in texels.
    std::uint32_t height{};
    /// `width * height * 4` bytes of 8-bit RGBA.
    std::vector<std::uint8_t> rgba;
};
/// The texels of each level that @p image stores, base level first, as 8-bit RGBA: an
/// ImageFormat::rgba8 image's one level as it is, and each level of an ImageFormat::bc7 image
/// decoded as the Khronos Data Format Specification defines BC7, with a block of the reserved mode 8
/// decoding to zero in all four channels. VulkanRenderer decodes BC7 images this way for devices that
/// cannot sample them. Throws `std::invalid_argument` for an image without texels, or one that
/// validate_scene() rejects.
[[nodiscard]] std::vector<MipLevel> decode_image(const Image &image);
/// The first @p levels levels of @p image, decoded as decode_image(const Image &) decodes them, without decoding
/// the rest. Throws `std::invalid_argument` as that overload does, and when @p levels is 0 or more than
/// Image::levels.
[[nodiscard]] std::vector<MipLevel> decode_image(const Image &image, std::uint32_t levels);
/// Mip chain of a base-color texture, as texture_mips(const Texture &) builds it. Throws
/// `std::invalid_argument` unless @p texture uses TextureEncoding::srgb.
[[nodiscard]] std::vector<MipLevel> base_color_mips(const Texture &texture);
/// Full mip chain of @p texture, from a copy of its image's texels down to 1x1.
///
/// Each level halves both dimensions, rounding down to at least 1, and box-filters every source
/// texel, including odd edges. RGB is averaged in the texture's encoding: sRGB color in linear
/// light, data maps as stored; alpha is averaged as stored. Throws `std::invalid_argument` for a
/// null image, one that is not ImageFormat::rgba8 ("Mip filtering requires an RGBA8 image"), a zero
/// dimension, a byte count other than `width * height * 4`, including an image without texels, or
/// an invalid encoding.
[[nodiscard]] std::vector<MipLevel> texture_mips(const Texture &texture);
/// Options for texture_mips(const Texture &, TextureMipOptions).
struct TextureMipOptions {
    /// Alpha-test cutoff in texture space (the material cutoff divided by its constant alpha
    /// factor), in (0, 1].
    ///
    /// When set, color is averaged weighted by alpha. The base level is kept, and the alpha of each
    /// smaller level of the uncorrected chain is rescaled independently toward the base level's
    /// fraction of texels at or above the cutoff: the nearest coverage that 8-bit alpha can reach,
    /// preferring the smallest change on ties. Coverage counts texel centers, not filtered screen
    /// area, and ignores vertex alpha; small levels only approximate it.
    std::optional<float> alpha_coverage_cutoff{};
    /// Averages color weighted by alpha, without correcting coverage, as blended base-color maps need.
    ///
    /// Each smaller level's premultiplied color, its alpha times its color as texture_mips() averages it (sRGB
    /// color in linear light, data maps as stored), is then the mean of the premultiplied colors of the texels of
    /// the previous level that it averages, within 8-bit rounding, so texels with zero alpha add no color. A texel
    /// whose source texels all have zero alpha takes their unweighted average color instead, so color that an image
    /// spreads into its transparent texels, as bake_impostor() spreads it, still reaches the edges that filtering
    /// blends them with at every level; transparent texels that hold black stay black. Alpha is averaged as stored,
    /// and the base level is kept. #alpha_coverage_cutoff weights color this way too.
    bool alpha_weighted_color = false;
    /// Compares both fields, the cutoffs with `float` `==`.
    bool operator==(const TextureMipOptions &) const = default;
};
/// texture_mips(const Texture &) with @p options. Throws `std::invalid_argument` also for a cutoff
/// that is not finite or not in (0, 1].
[[nodiscard]] std::vector<MipLevel> texture_mips(const Texture &texture, TextureMipOptions options);
/// How a material uses its alpha: the base-color texture's alpha times Material::alpha times vertex alpha.
enum class AlphaMode {
    /// Alpha is ignored, in color and shadow passes alike (glTF `OPAQUE`).
    opaque,
    /// Discards fragments whose alpha is below Material::alpha_cutoff, in color and shadow passes alike (glTF
    /// `MASK`).
    mask,
    /// Composites the shaded color, emission included, over what lies behind it with the "over" operator,
    /// weighted by alpha (glTF `BLEND`). Blended surfaces draw after opaque and masked ones and write no depth;
    /// VulkanRenderer states their order and its limits. They cast no shadows, as unlit materials do, since
    /// shadow maps hold depth only; lit blended surfaces receive shadows as other lit surfaces do.
    blend
};
/// Metallic-roughness material with glTF semantics. validate_material states the accepted values.
struct Material {
    /// Name that reports such as SnapshotPrimitive::material_name show, any text: the importer gives the file's name,
    /// `(unnamed)` when the file has none, and `glTF default` to the material it adds for primitives without one.
    std::string name;
    /// Linear base-color RGB factor, each channel in [0, 1].
    Vec3 factor{1, 1, 1};
    /// Base-color texture index, or no_index; the texture must be sRGB.
    int texture = no_index;
    /// Linear metallic factor in [0, 1], applied to the map's blue channel. Programmatic
    /// materials default to a matte dielectric; the importer supplies glTF's default of 1.
    float metallic = 0;
    /// Linear roughness factor in [0, 1], applied to the map's green channel.
    float roughness = 1;
    /// Normal map index, or no_index; the texture must be linear.
    int normal_texture = no_index;
    /// Metallic-roughness map index, or no_index; the texture must be linear.
    int metallic_roughness_texture = no_index;
    /// Emissive map index, or no_index; the texture must be sRGB.
    int emissive_texture = no_index;
    /// Occlusion map index, or no_index; the texture must be linear, and its red channel darkens ambient
    /// light only.
    int occlusion_texture = no_index;
    /// Linear emissive RGB factor, each channel finite and at least 0.
    Vec3 emissive{};
    /// Scale of the normal map's XY; finite.
    float normal_scale = 1;
    /// Occlusion strength in [0, 1].
    float occlusion_strength = 1;
    /// Base-color alpha factor in [0, 1].
    float alpha = 1;
    /// Alpha-test threshold for AlphaMode::mask, which the other modes ignore; finite and at least 0 in every
    /// mode.
    float alpha_cutoff = .5F;
    /// How the material uses its alpha; a value outside AlphaMode is rejected.
    AlphaMode alpha_mode = AlphaMode::opaque;
    /// Whether a face also renders when its back is toward the viewer; when false, that side is not
    /// drawn. Shadows are cast from both sides either way. Programmatic materials render both
    /// sides; the importer supplies glTF `doubleSided`, whose default is false.
    bool double_sided = true;
    /// Renders base color without lighting or shadow casting; fog still applies (glTF
    /// `KHR_materials_unlit`).
    bool unlit = false;
};
/// Node property that an AnimationChannel drives.
enum class ChannelPath {
    translation, ///< Transform::translation, from the first three components of each value.
    rotation,    ///< Transform::rotation, from each value as an XYZW quaternion, which sampling normalizes.
    scale        ///< Transform::scale, from the first three components of each value.
};
/// Keyframe interpolation. The importer rejects glTF `CUBICSPLINE`.
enum class Interpolation {
    linear, ///< Linear for translation and scale; shortest-arc spherical for rotation.
    step    ///< Holds each key until the next.
};
/// Keyframes for one property of one node.
struct AnimationChannel {
    /// Target node index; the node must not have AssetNode::has_matrix set.
    std::size_t node{};
    /// Property of the node that the values set.
    ChannelPath path{};
    /// How sample_pose finds the value between two keys.
    Interpolation interpolation{};
    /// Key times in seconds, nonnegative and strictly increasing. The importer checks this;
    /// sample_pose assumes it.
    std::vector<double> times;
    /// One value per key: XYZ in the first three components, or an XYZW quaternion for rotation.
    std::vector<std::array<float, 4>> values;
};
/// A named clip.
struct Animation {
    /// Clip name from the file, or `(unnamed)` when it gives none. Names need not be unique; see find_animation.
    std::string name;
    /// Latest key time over all channels, in seconds; zero for a clip whose keys all sit at time 0.
    double duration{};
    /// The clip's channels; the importer gives at most one per node and ChannelPath and drops those without a target
    /// node.
    std::vector<AnimationChannel> channels;
};
/// An imported GLB: nodes, skins, geometry, materials, textures and clips.
struct Asset {
    /// Every node in the file, including nodes outside the selected scene.
    std::vector<AssetNode> nodes;
    /// One per glTF skin, in file order; SourcePrimitive::skin indexes them.
    std::vector<AssetSkin> skins;
    /// Primitives reachable from the selected scene, in depth-first node order.
    std::vector<SourcePrimitive> primitives;
    /// One per glTF material, then `glTF default` when a primitive has no material.
    std::vector<Material> materials;
    /// One per glTF texture, then a copy of each texture used both as color and as data, so that
    /// each entry has one encoding. Textures made from one glTF image, copies included, share one
    /// Image.
    std::vector<Texture> textures;
    /// One per glTF animation, in file order.
    std::vector<Animation> animations;
    /// Number of mesh-bearing nodes in the selected scene.
    std::size_t mesh_nodes{};
    /// Human-readable import notes.
    std::vector<std::string> notices;
};
/// Per-node transforms of one asset.
struct Pose {
    /// Local transform per node, or empty in a world-only pose such as EvaluationRig::render_pose
    /// returns.
    std::vector<Transform> local;
    /// Model-space matrix per node: the parent's world matrix times the node's local matrix.
    std::vector<Mat4> world;
    /// Compares #local and #world element by element with `float` `==`, so `-0` equals `0` and a NaN element
    /// equals nothing.
    bool operator==(const Pose &) const = default;
};
/// Reads the GLB file at @p path, 1 byte to 64 MiB, and imports it as
/// load_asset(std::span<const std::byte>, const StagingOptions &) does. The read is one more step of
/// @p options, checked before it starts. Throws `std::runtime_error` also when the file cannot be read.
[[nodiscard]] std::shared_ptr<const Asset> load_asset(const std::filesystem::path &path,
                                                      const StagingOptions &options = {});
/// Imports a binary glTF 2.0 (GLB) snapshot of 1 byte to 64 MiB. The result owns its data; no file
/// is opened and @p bytes is not retained.
///
/// The file needs exactly one embedded buffer and may embed PNG or JPEG images of at most
/// max_image_edge texels per edge and max_image_texels texels. Each image that a texture uses is
/// decoded once, into an Image that every texture made from it shares, and together those images
/// may decode to at most 1 GiB (268,435,456 texels), which is checked from their headers before any
/// is decoded. Limits: 4096 nodes, 4096 materials, 4096 textures before the copies that give a
/// texture a second encoding, 4096 images, 1 to Mesh::max_skin_joints joints per skin, 2,000,000
/// elements per accessor, 2,000,000 expanded vertices in total, 8,000,000 animation keys in total
/// and a node depth of 256. Geometry comes from the default scene, else the first scene, else every
/// root node; it must be triangle lists, reach no node twice and contain at least one triangle.
/// Skinned primitives need `JOINTS_0` and `WEIGHTS_0`; further joint sets are rejected. Clips
/// animate translation, rotation or scale with `LINEAR` or `STEP` keys and cannot target matrix
/// nodes; channels without a target node are ignored, and a clip whose keys all sit at time 0 is a
/// pose of zero duration. Every other channel counts its keys toward the key limit, even when it
/// shares its sampler's accessors with other channels, and the limit is checked before any key is
/// read.
///
/// Each material's `alphaMode` becomes the AlphaMode of the same name, with its `alphaCutoff` and
/// the alpha of its base-color factor. `KHR_materials_unlit` is the only extension that may be
/// required. A texture with an optional Basis or WebP source uses its PNG or JPEG source instead,
/// and needs one. External files, sparse accessors, compressed geometry, texture transforms, maps
/// on different UV sets, morph targets and GPU instancing are rejected.
///
/// Calls may run concurrently on any thread. Each reads @p bytes, which must not change during the
/// call, and the C locale, which the glTF parser reads to convert numbers and which must not change
/// during the call; it shares no other mutable state than the StagingProgress in @p options, and calls
/// no application code. @p options is checked before the parse, before each image and each primitive,
/// and before the call returns. Its steps are the parse, each decoded image and each imported
/// primitive; one image decodes in a single step that a stop cannot interrupt.
///
/// Throws `std::runtime_error` for unsupported or malformed content, including content beyond
/// these limits; `std::invalid_argument` for material values that validate_material rejects,
/// including metallic or roughness factors outside [0, 1]; anima::MathError, a
/// `std::invalid_argument`, for a rotation that cannot be normalized; and StagingCancelled when
/// @p options reports a stop.
[[nodiscard]] std::shared_ptr<const Asset> load_asset(std::span<const std::byte> bytes,
                                                      const StagingOptions &options = {});
/// Imports a GLB holding only nodes and clips, for motion bound to a separate model through
/// MotionRuntime. As load_asset(std::span<const std::byte>, const StagingOptions &), except that the
/// file needs at least one clip and no geometry, skins, materials or textures; `std::runtime_error`
/// otherwise.
[[nodiscard]] std::shared_ptr<const Asset> load_motion_asset(std::span<const std::byte> bytes,
                                                             const StagingOptions &options = {});
/// Reads the GLB file at @p path, 1 byte to 64 MiB, and imports it as
/// load_motion_asset(std::span<const std::byte>, const StagingOptions &) does, with the read as one more
/// step of @p options.
[[nodiscard]] std::shared_ptr<const Asset> load_motion_asset(const std::filesystem::path &path,
                                                             const StagingOptions &options = {});
/// Samples @p animation at @p time seconds over the rest pose, or returns the rest pose when
/// @p animation is null.
///
/// Unkeyed properties keep their rest values. Times before the first key or after the last hold
/// the end keys; nothing loops. Throws `std::invalid_argument` for a negative or nonfinite
/// @p time, `std::runtime_error` for an empty, mismatched or out-of-range channel, a channel on a
/// matrix node, a cyclic or invalid hierarchy or a nonfinite result, and anima::MathError for a
/// rotation that cannot be normalized.
[[nodiscard]] Pose sample_pose(const Asset &asset, const Animation *animation = nullptr, double time = 0);
/// Pose with the given local transform for each node and every world matrix rebuilt, including
/// nodes without clips. Matrix nodes keep AssetNode::rest_matrix. Throws `std::invalid_argument`
/// unless @p local has one entry per node, and otherwise as sample_pose does.
[[nodiscard]] Pose pose_from_local(const Asset &asset, std::span<const Transform> local);
/// Blends the local transforms of two poses of @p asset by @p weight in [0, 1] and rebuilds the
/// world matrices.
///
/// Translation and scale interpolate linearly and rotation along the shortest arc; matrix nodes
/// keep their rest transforms. World matrices of the inputs are ignored, so world-only poses are
/// rejected. The caller ensures both poses belong to @p asset. Throws `std::invalid_argument` for
/// an invalid weight or a pose without one local transform per node, and otherwise as sample_pose
/// does.
[[nodiscard]] Pose blend_pose(const Asset &asset, const Pose &from, const Pose &to, float weight);
/// The one clip of @p asset named @p name, borrowed from @p asset. Throws `std::out_of_range`
/// when no clip has that name and `std::invalid_argument` when several do.
[[nodiscard]] const Animation &find_animation(const Asset &asset, std::string_view name);
/// Index of the one node of @p asset named @p name. Throws `std::out_of_range` when no node has
/// that name and `std::invalid_argument` when several do.
[[nodiscard]] std::size_t find_node(const Asset &asset, std::string_view name);
} // namespace anima
