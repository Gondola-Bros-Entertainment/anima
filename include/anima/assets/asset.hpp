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

namespace anima {
struct AssetNode {
    std::string name;
    int parent = -1;
    Transform rest;
    bool has_matrix{};
    Mat4 rest_matrix = identity();
};
struct AssetSkin {
    std::vector<std::size_t> joints;
    std::vector<Mat4> inverse_bind;
};
struct SourceVertex {
    Vec3 position{}, normal{}, color{1, 1, 1};
    std::array<float, 2> uv{};
    std::array<std::uint32_t, 4> joints{};
    std::array<float, 4> weights{};
    // glTF tangent and handedness. w=0 selects a derivative frame when absent.
    std::array<float, 4> tangent{};
    float alpha = 1;
};
struct SourcePrimitive {
    std::string mesh_name;
    std::size_t node{};
    int skin = -1, material = -1;
    std::vector<SourceVertex> vertices; // Expanded triangles; source positions are never posed in place.
};
enum class Filter { nearest, linear };
enum class Wrap { repeat, clamp, mirror };
enum class TextureEncoding { srgb, linear };
struct Sampler {
    Filter mag = Filter::linear, min = Filter::linear, mip = Filter::linear;
    Wrap u = Wrap::repeat, v = Wrap::repeat;
    bool mipmapped = true;
};
struct Texture {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> rgba;
    Sampler sampler;
    TextureEncoding encoding = TextureEncoding::srgb;
};
struct MipLevel {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> rgba;
};
[[nodiscard]] std::vector<MipLevel> base_color_mips(const Texture &texture);
// Uses the texture's declared encoding; data maps are averaged without sRGB conversion.
[[nodiscard]] std::vector<MipLevel> texture_mips(const Texture &texture);
struct TextureMipOptions {
    // Texture-space cutoff after a constant material alpha factor, in (0, 1].
    // Preserves approximate texel coverage and uses alpha-weighted colour filtering.
    std::optional<float> alpha_coverage_cutoff;
    bool operator==(const TextureMipOptions &) const = default;
};
[[nodiscard]] std::vector<MipLevel> texture_mips(const Texture &texture, TextureMipOptions options);
enum class AlphaMode { opaque, mask };
struct Material {
    std::string name;
    Vec3 factor{1, 1, 1};
    int texture = -1;
    // Linear scalar factors. Programmatic geometry defaults to a matte
    // dielectric; the importer supplies glTF's metallic=1 default explicitly.
    float metallic = 0, roughness = 1;
    int normal_texture = -1, metallic_roughness_texture = -1, emissive_texture = -1, occlusion_texture = -1;
    Vec3 emissive{};
    float normal_scale = 1, occlusion_strength = 1, alpha = 1, alpha_cutoff = .5F;
    AlphaMode alpha_mode = AlphaMode::opaque;
    bool unlit = false;
};
enum class ChannelPath { translation, rotation, scale };
enum class Interpolation { linear, step };
struct AnimationChannel {
    std::size_t node{};
    ChannelPath path{};
    Interpolation interpolation{};
    std::vector<double> times;
    std::vector<std::array<float, 4>> values;
};
struct Animation {
    std::string name;
    double duration{};
    std::vector<AnimationChannel> channels;
};
struct Asset {
    std::vector<AssetNode> nodes;
    std::vector<AssetSkin> skins;
    std::vector<SourcePrimitive> primitives;
    std::vector<Material> materials;
    std::vector<Texture> textures;
    std::vector<Animation> animations;
    std::size_t mesh_nodes{};
    std::vector<std::string> notices;
};
struct Pose {
    std::vector<Transform> local;
    std::vector<Mat4> world;
};
[[nodiscard]] std::shared_ptr<const Asset> load_asset(const std::filesystem::path &path);
// Parses an immutable embedded GLB snapshot; no file access or borrowed buffers survive the call.
[[nodiscard]] std::shared_ptr<const Asset> load_asset(std::span<const std::byte> bytes);
[[nodiscard]] std::shared_ptr<const Asset> load_motion_asset(std::span<const std::byte> bytes);
// Independent skeletal motion: nodes and animation channels, without render
// geometry, skins, textures or materials. Binding to a rig is checked by the consumer.
[[nodiscard]] std::shared_ptr<const Asset> load_motion_asset(const std::filesystem::path &path);
[[nodiscard]] Pose sample_pose(const Asset &asset, const Animation *animation = nullptr, double time = 0);
// Bind local transforms to an asset and rebuild all descendants, including
// nodes without animation channels. Matrix nodes retain their authored bind.
[[nodiscard]] Pose pose_from_local(const Asset &asset, std::span<const Transform> local);
// Blend local translation/scale and shortest-arc joint rotations, then rebuild
// the hierarchy. Poses must belong to the supplied asset; world matrices are
// derived rather than interpolated, preserving rigid joint rotations.
[[nodiscard]] Pose blend_pose(const Asset &asset, const Pose &from, const Pose &to, float weight);
// The returned animation borrows from asset; the lookup name is not retained.
[[nodiscard]] const Animation &find_animation(const Asset &asset, std::string_view name);
[[nodiscard]] std::size_t unique_node(const Asset &asset, const std::string &name);
} // namespace anima
