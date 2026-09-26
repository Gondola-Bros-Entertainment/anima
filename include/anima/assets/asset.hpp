#pragma once
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
/// `std::shared_ptr<const Asset>` and own all their data.

namespace anima {
/// One glTF node.
struct AssetNode {
    /// Node name, or `(unnamed)` when the file gives none. Names need not be unique; see
    /// unique_node.
    std::string name;
    /// Index of the parent node, or -1 for a root.
    int parent = -1;
    /// Local rest translation, rotation and scale. Unused when #has_matrix is set.
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
    /// When the file has no tangent, w is 0 and the renderer derives a frame from screen-space
    /// derivatives, leaving the normal unmapped where the UVs are degenerate.
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
    /// Index into Asset::skins, or -1 when unskinned.
    int skin = -1;
    /// Index into Asset::materials, or -1 for none. Imported primitives without a material use an
    /// appended `glTF default` material (metallic 1, roughness 1).
    int material = -1;
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
/// Decoded RGBA8 image with its sampler and encoding.
struct Texture {
    /// Width in texels.
    std::uint32_t width{};
    /// Height in texels.
    std::uint32_t height{};
    /// `width * height * 4` bytes of 8-bit RGBA, row by row, starting at texture coordinate
    /// `v = 0`.
    std::vector<std::uint8_t> rgba;
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
/// Mip chain of a base-color texture, as texture_mips(const Texture &) builds it. Throws
/// `std::invalid_argument` unless @p texture uses TextureEncoding::srgb.
[[nodiscard]] std::vector<MipLevel> base_color_mips(const Texture &texture);
/// Full mip chain of @p texture, from a copy of its texels down to 1x1.
///
/// Each level halves both dimensions, rounding down to at least 1, and box-filters every source
/// texel, including odd edges. RGB is averaged in the texture's encoding: sRGB color in linear
/// light, data maps as stored; alpha is averaged as stored. Throws `std::invalid_argument` for a
/// zero dimension, a byte count other than `width * height * 4` or an invalid encoding.
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
    std::optional<float> alpha_coverage_cutoff;
    bool operator==(const TextureMipOptions &) const = default;
};
/// texture_mips(const Texture &) with @p options. Throws `std::invalid_argument` also for a cutoff
/// that is not finite or not in (0, 1].
[[nodiscard]] std::vector<MipLevel> texture_mips(const Texture &texture, TextureMipOptions options);
/// How a material uses alpha, in color and shadow passes alike. The importer rejects glTF `BLEND`.
enum class AlphaMode {
    opaque, ///< Alpha is ignored.
    mask    ///< Discards fragments whose texture, material and vertex alpha product is below the cutoff.
};
/// Metallic-roughness material with glTF semantics. validate_material states the accepted values.
struct Material {
    std::string name;
    /// Linear base-color RGB factor, each channel in [0, 1].
    Vec3 factor{1, 1, 1};
    /// Base-color texture index, or -1; the texture must be sRGB.
    int texture = -1;
    /// Linear metallic factor in [0, 1], applied to the map's blue channel. Programmatic
    /// materials default to a matte dielectric; the importer supplies glTF's default of 1.
    float metallic = 0;
    /// Linear roughness factor in [0, 1], applied to the map's green channel.
    float roughness = 1;
    /// Normal map index, or -1; the texture must be linear.
    int normal_texture = -1;
    /// Metallic-roughness map index, or -1; the texture must be linear.
    int metallic_roughness_texture = -1;
    /// Emissive map index, or -1; the texture must be sRGB.
    int emissive_texture = -1;
    /// Occlusion map index, or -1; the texture must be linear, and its red channel darkens ambient
    /// light only.
    int occlusion_texture = -1;
    /// Linear emissive RGB factor, each channel finite and at least 0.
    Vec3 emissive{};
    /// Scale of the normal map's XY; finite.
    float normal_scale = 1;
    /// Occlusion strength in [0, 1].
    float occlusion_strength = 1;
    /// Base-color alpha factor in [0, 1].
    float alpha = 1;
    /// Alpha-test threshold for AlphaMode::mask; finite and at least 0.
    float alpha_cutoff = .5F;
    AlphaMode alpha_mode = AlphaMode::opaque;
    /// Renders base color without lighting or shadow casting; fog still applies (glTF
    /// `KHR_materials_unlit`).
    bool unlit = false;
};
/// Node property that an AnimationChannel drives.
enum class ChannelPath { translation, rotation, scale };
/// Keyframe interpolation. The importer rejects glTF `CUBICSPLINE`.
enum class Interpolation {
    linear, ///< Linear for translation and scale; shortest-arc spherical for rotation.
    step    ///< Holds each key until the next.
};
/// Keyframes for one property of one node.
struct AnimationChannel {
    /// Target node index; the node must not have AssetNode::has_matrix set.
    std::size_t node{};
    ChannelPath path{};
    Interpolation interpolation{};
    /// Key times in seconds, nonnegative and strictly increasing. The importer checks this;
    /// sample_pose assumes it.
    std::vector<double> times;
    /// One value per key: XYZ in the first three components, or an XYZW quaternion for rotation.
    std::vector<std::array<float, 4>> values;
};
/// A named clip.
struct Animation {
    std::string name;
    /// Latest key time over all channels, in seconds; positive for imported clips.
    double duration{};
    std::vector<AnimationChannel> channels;
};
/// An imported GLB: nodes, skins, geometry, materials, textures and clips.
struct Asset {
    /// Every node in the file, including nodes outside the selected scene.
    std::vector<AssetNode> nodes;
    std::vector<AssetSkin> skins;
    /// Primitives reachable from the selected scene, in depth-first node order.
    std::vector<SourcePrimitive> primitives;
    /// One per glTF material, then `glTF default` when a primitive has no material.
    std::vector<Material> materials;
    /// One per glTF texture, then a copy of each texture used both as color and as data, so that
    /// each entry has one encoding.
    std::vector<Texture> textures;
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
};
/// Reads the GLB file at @p path, 1 byte to 64 MiB, and imports it as
/// load_asset(std::span<const std::byte>) does. Throws `std::runtime_error` also when the file
/// cannot be read.
[[nodiscard]] std::shared_ptr<const Asset> load_asset(const std::filesystem::path &path);
/// Imports a binary glTF 2.0 (GLB) snapshot of 1 byte to 64 MiB. The result owns its data; no file
/// is opened and @p bytes is not retained.
///
/// The file needs exactly one embedded buffer and may embed PNG or JPEG images of at most 8192
/// texels per edge and 16,777,216 texels. Limits: 4096 nodes, 4096 materials, 1 to 512 joints per
/// skin, 2,000,000 elements per accessor, 2,000,000 expanded vertices in total and a node depth of
/// 256. Geometry comes from the default scene, else the first scene, else every root node; it
/// must be triangle lists, reach no node twice and contain at least one triangle. Skinned
/// primitives need `JOINTS_0` and `WEIGHTS_0`; further joint sets are rejected. Clips animate
/// translation, rotation or scale with `LINEAR` or `STEP` keys and cannot target matrix nodes.
///
/// `KHR_materials_unlit` is the only extension that may be required. External files, sparse
/// accessors, compressed geometry, Basis and WebP textures, texture transforms, `BLEND`
/// materials, maps on different UV sets, morph targets and GPU instancing are rejected.
///
/// Throws `std::runtime_error` for unsupported or malformed content, including metallic or
/// roughness factors outside [0, 1]; `std::invalid_argument` for other material values that
/// validate_material rejects; and anima::MathError, a `std::invalid_argument`, for a rotation that
/// cannot be normalized.
[[nodiscard]] std::shared_ptr<const Asset> load_asset(std::span<const std::byte> bytes);
/// Imports a GLB holding only nodes and clips, for motion bound to a separate model through
/// MotionRuntime. As load_asset(std::span<const std::byte>), except that the file needs at least
/// one clip and no geometry, skins, materials or textures; `std::runtime_error` otherwise.
[[nodiscard]] std::shared_ptr<const Asset> load_motion_asset(std::span<const std::byte> bytes);
/// Reads the GLB file at @p path, 1 byte to 64 MiB, and imports it as
/// load_motion_asset(std::span<const std::byte>) does.
[[nodiscard]] std::shared_ptr<const Asset> load_motion_asset(const std::filesystem::path &path);
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
/// The one clip of @p asset named @p name, borrowed from @p asset. Throws `std::runtime_error`
/// when no clip or several clips have that name.
[[nodiscard]] const Animation &find_animation(const Asset &asset, std::string_view name);
/// Index of the one node of @p asset named @p name. Throws `std::runtime_error` when no node or
/// several nodes have that name.
[[nodiscard]] std::size_t unique_node(const Asset &asset, const std::string &name);
} // namespace anima
