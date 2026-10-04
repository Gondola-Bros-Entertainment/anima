// Impostors: a CPU rasterizer that views a Mesh from each frame of an octahedral grid, and the compilation of its atlas
// into a Mesh that VulkanRenderer draws as an impostor.
#include <algorithm>
#include <anima/impostor.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <vector>

namespace anima {
namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::invalid_argument(message);
}
constexpr char options_message[] =
    "Impostor options must give 2 to 32 frames of 8 to 1024 texels, at most 8192 in all, and 1 to 8 samples";
constexpr char frames_message[] =
    "Impostor frames must number from 2 to 32 per side, with a finite center and a positive finite radius";
constexpr char images_message[] = "Impostor images must be equal squares divisible into the frames, color and "
                                  "emission sRGB, normals and surface linear";
constexpr std::uint32_t minimum_frames = 2, maximum_frames = 32;
bool known(ImpostorLayout layout) { return layout == ImpostorLayout::hemisphere || layout == ImpostorLayout::sphere; }
bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// The unit direction from which frame (i, j) of a grid of @p count per side views, as ImpostorFrames defines it, from
// the grid point's offsets from the center in units of half a cell, which are integers, as shaders/impostor.glsl
// computes it: the documented point scaled by `2 (count - 1)` for a hemisphere and `count - 1` for a sphere, so that
// the zeros at a pole are exact on the CPU and every GPU.
Vec3 frame_direction(ImpostorLayout layout, std::uint32_t count, std::uint32_t i, std::uint32_t j) {
    const int span = int(count) - 1, u = 2 * int(i) - span, v = 2 * int(j) - span;
    int x = u, y = 0, z = v;
    if (layout == ImpostorLayout::hemisphere) {
        x = u + v;
        z = u - v;
        y = 2 * span - std::abs(x) - std::abs(z);
    } else {
        y = span - std::abs(u) - std::abs(v);
        if (y < 0) {
            x = (span - std::abs(v)) * (u < 0 ? -1 : 1);
            z = (span - std::abs(u)) * (v < 0 ? -1 : 1);
        }
    }
    return normalized({float(x), float(y), float(z)});
}
// A frame's texture axes and the direction it views from, as ImpostorFrames defines them.
struct FrameAxes {
    Vec3 right, up, direction;
};
FrameAxes frame_axes(Vec3 d) {
    const auto right = d.x == 0 && d.z == 0 ? Vec3{1, 0, 0} : normalized({d.z, 0, -d.x});
    return {right, cross(d, right), d};
}

float unit_byte(std::uint8_t value) { return float(value) / 255; }
std::uint8_t to_byte(float value) { return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.F, 1.F) * 255)); }
float decode_srgb(float value) { return value <= .04045F ? value / 12.92F : std::pow((value + .055F) / 1.055F, 2.4F); }
std::uint8_t encode_srgb(float value) {
    value = std::clamp(value, 0.F, 1.F);
    return to_byte(value <= .0031308F ? value * 12.92F : 1.055F * std::pow(value, 1 / 2.4F) - .055F);
}
// Linear light of each 8-bit sRGB value.
const std::array<float, 256> &srgb_table() {
    static const auto table = [] {
        std::array<float, 256> values{};
        for (std::size_t i = 0; i < values.size(); ++i)
            values[i] = decode_srgb(float(i) / 255);
        return values;
    }();
    return table;
}

using Rgba = std::array<float, 4>;
Rgba mix(const Rgba &a, const Rgba &b, float t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t, a[3] + (b[3] - a[3]) * t};
}
long wrap(long x, long size, Wrap mode) {
    switch (mode) {
    case Wrap::clamp:
        return std::clamp(x, 0L, size - 1);
    case Wrap::mirror: {
        const long period = 2 * size, m = ((x % period) + period) % period;
        return m < size ? m : period - 1 - m;
    }
    case Wrap::repeat:
        break;
    }
    return ((x % size) + size) % size;
}
// One texture's mip chain, decoded to 8-bit RGBA as the renderer uploads it, sampled as its sampler says with RGB in
// linear light.
class Sampled {
  public:
    Sampled(Texture texture, std::shared_ptr<const Image> texels, std::optional<float> coverage_cutoff)
        : sampler_(texture.sampler), srgb_(texture.encoding == TextureEncoding::srgb) {
        texture.image = std::move(texels);
        const auto &image = *texture.image;
        if (image.format == ImageFormat::bc7)
            levels_ = sampler_.mipmapped ? decode_image(image) : decode_image(image, 1);
        else if (sampler_.mipmapped)
            levels_ = texture_mips(texture, {coverage_cutoff, false});
        else
            levels_.push_back({image.width, image.height, image.rgba});
    }
    [[nodiscard]] float width() const { return float(levels_[0].width); }
    [[nodiscard]] float height() const { return float(levels_[0].height); }
    // The texture at (@p u, @p v) at level of detail @p lod, as a sampler with its filters and wrap modes filters it.
    [[nodiscard]] Rgba sample(float u, float v, float lod) const {
        const auto last = float(levels_.size() - 1);
        if (!sampler_.mipmapped || lod <= 0)
            return filtered(levels_[0], u, v, lod <= 0 ? sampler_.mag : sampler_.min);
        lod = std::min(lod, last);
        if (sampler_.mip == Filter::nearest)
            return filtered(levels_[std::size_t(std::lround(lod))], u, v, sampler_.min);
        const auto lower = std::size_t(lod), upper = std::min(lower + 1, levels_.size() - 1);
        return mix(filtered(levels_[lower], u, v, sampler_.min), filtered(levels_[upper], u, v, sampler_.min),
                   lod - float(lower));
    }

  private:
    [[nodiscard]] Rgba texel(const MipLevel &level, long x, long y) const {
        x = wrap(x, long(level.width), sampler_.u);
        y = wrap(y, long(level.height), sampler_.v);
        const auto *p = &level.rgba[(std::size_t(y) * level.width + std::size_t(x)) * 4];
        const auto &table = srgb_table();
        return srgb_ ? Rgba{table[p[0]], table[p[1]], table[p[2]], unit_byte(p[3])}
                     : Rgba{unit_byte(p[0]), unit_byte(p[1]), unit_byte(p[2]), unit_byte(p[3])};
    }
    [[nodiscard]] Rgba filtered(const MipLevel &level, float u, float v, Filter filter) const {
        const float x = u * float(level.width), y = v * float(level.height);
        if (filter == Filter::nearest)
            return texel(level, long(std::floor(x)), long(std::floor(y)));
        const float fx = x - .5F, fy = y - .5F;
        const auto x0 = long(std::floor(fx)), y0 = long(std::floor(fy));
        const float tx = fx - float(x0), ty = fy - float(y0);
        return mix(mix(texel(level, x0, y0), texel(level, x0 + 1, y0), tx),
                   mix(texel(level, x0, y0 + 1), texel(level, x0 + 1, y0 + 1), tx), ty);
    }
    std::vector<MipLevel> levels_;
    Sampler sampler_;
    bool srgb_{};
};

// A material with the textures it samples.
struct BakeMaterial {
    Material source;
    const Sampled *base{}, *normal{}, *metallic_roughness{}, *occlusion{}, *emissive{};
};
// A vertex in the mesh's space, as its node's rest matrix places it.
struct BakeVertex {
    Vec3 position, normal, color;
    std::array<float, 2> uv{};
    std::array<float, 4> tangent{};
    float alpha{};
};
struct BakeTriangle {
    std::array<std::uint32_t, 3> corners{};
    const BakeMaterial *material{};
    // Whether its node's rest matrix mirrors, which reverses its winding.
    bool mirrored{};
    // Twice its area in texture coordinates, which sets the mip level of its samples.
    float uv_area{};
    // Its tangent and bitangent from its positions and texture coordinates, for normal maps without vertex tangents;
    // zero where the texture coordinates are degenerate.
    Vec3 tangent{}, bitangent{};
};
// What a frame's texel holds of one surface, in linear light and the mesh's space.
struct Surface {
    Vec3 color, normal, emission;
    float height{}, occlusion{}, roughness{}, metallic{};
};
template <class T> T interpolate(const T &a, const T &b, const T &c, float w0, float w1, float w2) {
    return a * w0 + b * w1 + c * w2;
}
float interpolate(float a, float b, float c, float w0, float w1, float w2) { return a * w0 + b * w1 + c * w2; }
Vec3 multiply(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
Vec3 rgb(const Rgba &value) { return {value[0], value[1], value[2]}; }

class Baker {
  public:
    Baker(const Mesh &mesh, const ImpostorOptions &options) : options_(options) {
        const auto images = mesh.texel_images();
        const auto &description = *mesh.description();
        // Each texture once per set of mip options; a map keeps their addresses as it grows.
        const auto sampled = [&](int index, std::optional<float> cutoff) -> const Sampled * {
            if (index < 0)
                return nullptr;
            const auto key = std::pair{index, cutoff.value_or(-1)};
            auto found = samplers_.find(key);
            if (found == samplers_.end())
                found = samplers_
                            .emplace(key, Sampled(description.textures[std::size_t(index)], images[std::size_t(index)],
                                                  cutoff))
                            .first;
            return &found->second;
        };
        const auto &draws = mesh.draws();
        for (const auto &draw : draws)
            if (draw.index_count)
                require(!draw.skinned, "Impostors bake only rigid meshes");
        // The default Material for draws without one, then each material of the mesh.
        materials_.resize(description.materials.size() + 1);
        for (std::size_t m = 0; m < description.materials.size(); ++m)
            materials_[m + 1].source = description.materials[m];
        for (const auto &draw : draws) {
            if (!draw.index_count)
                continue;
            const auto &material = materials_[std::size_t(draw.material + 1)].source;
            require(!material.unlit && material.alpha_mode != AlphaMode::blend,
                    "Impostors bake only lit opaque and masked materials");
        }
        for (auto &material : materials_) {
            const auto &m = material.source;
            // A masked base color's mips keep its coverage at the cutoff, as material_texture_plan() plans them.
            std::optional<float> cutoff;
            if (m.alpha_mode == AlphaMode::mask && m.alpha > 0 && m.alpha_cutoff / m.alpha > 0 &&
                m.alpha_cutoff / m.alpha <= 1)
                cutoff = m.alpha_cutoff / m.alpha;
            material.base = sampled(m.texture, cutoff);
            material.normal = sampled(m.normal_texture, {});
            material.metallic_roughness = sampled(m.metallic_roughness_texture, {});
            material.occlusion = sampled(m.occlusion_texture, {});
            material.emissive = sampled(m.emissive_texture, {});
            emission_scale_ = std::max({emission_scale_, m.emissive.x, m.emissive.y, m.emissive.z});
            emits_ = emits_ || m.emissive.x > 0 || m.emissive.y > 0 || m.emissive.z > 0;
        }
        const auto &rest = mesh.rest_pose().world;
        const auto vertices = mesh.vertices();
        const auto indices = mesh.indices();
        for (const auto &draw : draws) {
            if (!draw.index_count)
                continue;
            const auto &m = rest[draw.node];
            const Vec3 a{m[0], m[1], m[2]}, b{m[4], m[5], m[6]}, c{m[8], m[9], m[10]};
            const bool mirrored = dot(a, cross(b, c)) < 0;
            const auto first = indices.begin() + draw.first_index, last = first + draw.index_count;
            const auto lowest = *std::min_element(first, last), highest = *std::max_element(first, last);
            const auto offset = static_cast<std::uint32_t>(vertices_.size()) - lowest;
            for (auto i = lowest; i <= highest; ++i) {
                const auto &v = vertices[i];
                vertices_.push_back(
                    {point(m, v.position), normal(m, v.normal), v.color, v.uv, tangent(m, v.tangent), v.alpha});
            }
            const auto *material = &materials_[std::size_t(draw.material + 1)];
            for (auto corner = first; corner != last; corner += 3) {
                BakeTriangle triangle;
                triangle.corners = {corner[0] + offset, corner[1] + offset, corner[2] + offset};
                triangle.material = material;
                triangle.mirrored = mirrored;
                const auto &v0 = vertices_[triangle.corners[0]], &v1 = vertices_[triangle.corners[1]],
                           &v2 = vertices_[triangle.corners[2]];
                const std::array<float, 2> du1{v1.uv[0] - v0.uv[0], v1.uv[1] - v0.uv[1]},
                    du2{v2.uv[0] - v0.uv[0], v2.uv[1] - v0.uv[1]};
                const float determinant = du1[0] * du2[1] - du1[1] * du2[0];
                triangle.uv_area = std::abs(determinant);
                // As the standard material's cotangent frame, from the triangle's edges instead of screen derivatives.
                if (std::abs(determinant) > 1e-10F) {
                    const auto dp1 = v1.position - v0.position, dp2 = v2.position - v0.position;
                    triangle.tangent = (dp1 * du2[1] - dp2 * du1[1]) * (1 / determinant);
                    triangle.bitangent = (dp2 * du1[0] - dp1 * du2[0]) * (1 / determinant);
                }
                triangles_.push_back(triangle);
            }
        }
        require(!triangles_.empty(), "An impostor requires a mesh with triangles");
        const auto &bounds = mesh.rest_bounds();
        center_ = (bounds.minimum + bounds.maximum) * .5F;
        for (const auto &v : vertices_)
            radius_ = std::max(radius_, length(v.position - center_));
        require(radius_ > 0 && std::isfinite(radius_), "An impostor requires a mesh with triangles");
    }

    ImpostorAtlas bake() {
        const auto count = options_.frames, size = options_.frame_size, side = count * size;
        const auto texels = std::size_t(side) * side * 4;
        std::vector<std::uint8_t> color(texels), normal_depth(texels), surface(texels), emissive(emits_ ? texels : 0);
        for (std::size_t i = 3; i < texels; i += 4) {
            surface[i] = 255;
            if (emits_)
                emissive[i] = 255;
        }
        Images images{color, normal_depth, surface, emissive};
        for (std::uint32_t j = 0; j < count; ++j)
            for (std::uint32_t i = 0; i < count; ++i)
                bake_frame(i, j, images);
        const auto image = [&](std::vector<std::uint8_t> &rgba) {
            auto value = std::make_shared<Image>();
            value->width = value->height = side;
            value->rgba = std::move(rgba);
            return std::shared_ptr<const Image>(std::move(value));
        };
        const Sampler sampler{Filter::linear, Filter::linear, Filter::linear, Wrap::clamp, Wrap::clamp, true};
        ImpostorAtlas atlas;
        atlas.frames = {options_.layout, count, center_, radius_};
        atlas.color = {image(color), sampler, TextureEncoding::srgb};
        atlas.normal_depth = {image(normal_depth), sampler, TextureEncoding::linear};
        atlas.surface = {image(surface), sampler, TextureEncoding::linear};
        if (emits_)
            atlas.emissive = Texture{image(emissive), sampler, TextureEncoding::srgb};
        atlas.emission_scale = emission_scale_;
        return atlas;
    }

  private:
    struct Images {
        std::vector<std::uint8_t> &color, &normal_depth, &surface, &emissive;
    };
    // A vertex across a frame, in samples along its right and up axes and in fixed point with subsamples per sample,
    // and its height toward the frame's viewpoint.
    struct Projected {
        float x, y, height;
        std::int64_t fx, fy;
    };
    static constexpr std::int64_t subsamples = 256;
    // The mip level for a texture of @p sampled's size across a triangle whose texture coordinates span @p uv_area for
    // each unit of the frame's texels that it spans: half the log of the texels per frame texel.
    static float level(const Sampled *sampled, float texels_per_texel) {
        return sampled ? .5F * std::log2(std::max(texels_per_texel * sampled->width() * sampled->height(), 1e-30F)) : 0;
    }
    // The base-color alpha of @p triangle at barycentric weights @p w1 and @p w2, which a masked material tests.
    float alpha(const BakeTriangle &triangle, float w1, float w2, float lod_scale) const {
        const auto &material = *triangle.material;
        const auto &v0 = vertices_[triangle.corners[0]], &v1 = vertices_[triangle.corners[1]],
                   &v2 = vertices_[triangle.corners[2]];
        const float w0 = 1 - w1 - w2;
        float value = material.source.alpha * interpolate(v0.alpha, v1.alpha, v2.alpha, w0, w1, w2);
        if (material.base) {
            const float u = interpolate(v0.uv[0], v1.uv[0], v2.uv[0], w0, w1, w2),
                        v = interpolate(v0.uv[1], v1.uv[1], v2.uv[1], w0, w1, w2);
            value *= material.base->sample(u, v, level(material.base, lod_scale))[3];
        }
        return value;
    }
    // What @p triangle shows at barycentric weights @p w1 and @p w2 seen from @p axes, from its @p front or back.
    Surface shade(const BakeTriangle &triangle, float w1, float w2, float lod_scale, const FrameAxes &axes,
                  bool front) const {
        const auto &material = *triangle.material;
        const auto &m = material.source;
        const auto &v0 = vertices_[triangle.corners[0]], &v1 = vertices_[triangle.corners[1]],
                   &v2 = vertices_[triangle.corners[2]];
        const float w0 = 1 - w1 - w2;
        const float u = interpolate(v0.uv[0], v1.uv[0], v2.uv[0], w0, w1, w2),
                    v = interpolate(v0.uv[1], v1.uv[1], v2.uv[1], w0, w1, w2);
        const auto sample = [&](const Sampled *sampled) {
            return sampled ? sampled->sample(u, v, level(sampled, lod_scale)) : Rgba{1, 1, 1, 1};
        };
        Surface result;
        const auto position = interpolate(v0.position, v1.position, v2.position, w0, w1, w2);
        result.height = dot(position - center_, axes.direction);
        const auto base = sample(material.base);
        const auto color =
            multiply(multiply(rgb(base), m.factor), interpolate(v0.color, v1.color, v2.color, w0, w1, w2));
        result.color = {std::clamp(color.x, 0.F, 1.F), std::clamp(color.y, 0.F, 1.F), std::clamp(color.z, 0.F, 1.F)};
        auto n = normalized(interpolate(v0.normal, v1.normal, v2.normal, w0, w1, w2));
        if (material.normal) {
            Vec3 t, b;
            bool valid = true;
            const std::array<float, 4> vertex_tangent{
                interpolate(v0.tangent[0], v1.tangent[0], v2.tangent[0], w0, w1, w2),
                interpolate(v0.tangent[1], v1.tangent[1], v2.tangent[1], w0, w1, w2),
                interpolate(v0.tangent[2], v1.tangent[2], v2.tangent[2], w0, w1, w2),
                interpolate(v0.tangent[3], v1.tangent[3], v2.tangent[3], w0, w1, w2)};
            // As the standard material: an authored tangent frame, or the triangle's cotangent frame.
            if (std::abs(vertex_tangent[3]) > .5F) {
                const Vec3 authored{vertex_tangent[0], vertex_tangent[1], vertex_tangent[2]};
                t = authored - n * dot(n, authored);
                valid = dot(t, t) > 1e-12F;
                t = normalized(t);
                b = cross(n, t) * (vertex_tangent[3] < 0 ? -1.F : 1.F);
            } else if (dot(triangle.tangent, triangle.tangent) > 0) {
                t = normalized(triangle.tangent - n * dot(n, triangle.tangent));
                b = cross(n, t) * (dot(cross(n, t), triangle.bitangent) < 0 ? -1.F : 1.F);
            } else
                valid = false;
            if (valid) {
                const auto mapped = sample(material.normal);
                const float x = (mapped[0] * 2 - 1) * m.normal_scale, y = (mapped[1] * 2 - 1) * m.normal_scale,
                            z = mapped[2] * 2 - 1;
                n = normalized(t * x + b * y + n * z);
            }
        }
        result.normal = front ? n : -n;
        const auto occlusion = sample(material.occlusion)[0];
        result.occlusion = material.occlusion ? 1 + m.occlusion_strength * (occlusion - 1) : 1;
        const auto metallic_roughness = sample(material.metallic_roughness);
        result.roughness = std::clamp(m.roughness * metallic_roughness[1], 0.F, 1.F);
        result.metallic = std::clamp(m.metallic * metallic_roughness[2], 0.F, 1.F);
        result.emission = multiply(m.emissive, rgb(sample(material.emissive)));
        return result;
    }
    void bake_frame(std::uint32_t i, std::uint32_t j, Images &images) {
        const auto axes = frame_axes(frame_direction(options_.layout, options_.frames, i, j));
        const auto size = options_.frame_size, samples = options_.samples, side = size * samples;
        // Samples across the frame per unit of the mesh's space, and the frame's texels per sample.
        const float scale = float(side) / (2 * radius_);
        const auto samples_per_texel = float(samples * samples);
        projected_.resize(vertices_.size());
        for (std::size_t v = 0; v < vertices_.size(); ++v) {
            const auto offset = vertices_[v].position - center_;
            const float x = (dot(offset, axes.right) + radius_) * scale, y = (dot(offset, axes.up) + radius_) * scale;
            projected_[v] = {x, y, dot(offset, axes.direction), std::llround(double(x) * subsamples),
                             std::llround(double(y) * subsamples)};
        }
        // Nearest first, so that the depth test rejects most hidden samples before their alpha is looked up.
        order_.resize(triangles_.size());
        std::iota(order_.begin(), order_.end(), 0U);
        nearest_.resize(triangles_.size());
        for (std::size_t t = 0; t < triangles_.size(); ++t) {
            const auto &c = triangles_[t].corners;
            nearest_[t] = std::max({projected_[c[0]].height, projected_[c[1]].height, projected_[c[2]].height});
        }
        std::stable_sort(order_.begin(), order_.end(),
                         [&](std::uint32_t a, std::uint32_t b) { return nearest_[a] > nearest_[b]; });
        const auto count = std::size_t(side) * side;
        depth_.assign(count, -std::numeric_limits<float>::infinity());
        hit_.assign(count, 0);
        weights_.resize(count);
        front_.assign(triangles_.size(), 0);
        footprint_.assign(triangles_.size(), 0);
        for (const auto t : order_) {
            const auto &triangle = triangles_[t];
            const auto &p0 = projected_[triangle.corners[0]], &p1 = projected_[triangle.corners[1]],
                       &p2 = projected_[triangle.corners[2]];
            // Edge functions in fixed point, exact in 64-bit integers as a GPU's are, so that a sample on an edge that
            // two triangles share falls in at least one of them, however the compiler rounds or fuses products.
            const auto edge = [](std::int64_t ax, std::int64_t ay, std::int64_t bx, std::int64_t by, std::int64_t px,
                                 std::int64_t py) { return (px - ax) * (by - ay) - (bx - ax) * (py - ay); };
            const std::int64_t area = edge(p0.fx, p0.fy, p2.fx, p2.fy, p1.fx, p1.fy);
            if (!area)
                continue;
            // Counterclockwise along right and up faces the frame's viewpoint, unless a mirroring matrix placed it.
            const bool front = (area > 0) != triangle.mirrored;
            if (!front && !triangle.material->source.double_sided)
                continue;
            front_[t] = front;
            // Texture coordinates covered per texel of the frame, which chooses mip levels as a GPU would.
            footprint_[t] =
                triangle.uv_area / (float(std::abs(area)) / float(subsamples * subsamples)) * samples_per_texel;
            const bool masked = triangle.material->source.alpha_mode == AlphaMode::mask;
            const double inverse = 1 / double(area);
            const auto low = [&](float a, float b, float c) {
                return std::clamp(long(std::ceil(std::min({a, b, c}) - .5F)), 0L, long(side));
            };
            const auto high = [&](float a, float b, float c) {
                return std::clamp(long(std::floor(std::max({a, b, c}) - .5F)), -1L, long(side) - 1);
            };
            const auto x0 = low(p0.x, p1.x, p2.x), x1 = high(p0.x, p1.x, p2.x), y0 = low(p0.y, p1.y, p2.y),
                       y1 = high(p0.y, p1.y, p2.y);
            for (auto y = y0; y <= y1; ++y)
                for (auto x = x0; x <= x1; ++x) {
                    const std::int64_t px = x * subsamples + subsamples / 2, py = y * subsamples + subsamples / 2;
                    // Each weight is the edge function opposite its corner, whose sign matches the area's inside.
                    const auto e0 = edge(p2.fx, p2.fy, p1.fx, p1.fy, px, py),
                               e1 = edge(p0.fx, p0.fy, p2.fx, p2.fy, px, py),
                               e2 = edge(p1.fx, p1.fy, p0.fx, p0.fy, px, py);
                    if (area > 0 ? e0 < 0 || e1 < 0 || e2 < 0 : e0 > 0 || e1 > 0 || e2 > 0)
                        continue;
                    const auto w1 = float(double(e1) * inverse), w2 = float(double(e2) * inverse);
                    const float w0 = 1 - w1 - w2;
                    const auto s = std::size_t(y) * side + std::size_t(x);
                    const float height = w0 * p0.height + w1 * p1.height + w2 * p2.height;
                    if (hit_[s] && height <= depth_[s])
                        continue;
                    if (masked && alpha(triangle, w1, w2, footprint_[t]) < triangle.material->source.alpha_cutoff)
                        continue;
                    depth_[s] = height;
                    hit_[s] = t + 1;
                    weights_[s] = {w1, w2};
                }
        }
        resolve(i, j, axes, images);
    }
    // Shades each texel of frame (i, j) once for each triangle that covers its samples, at their mean barycentric
    // weights, weighted by how many it covers, as multisampling resolves; then fills the uncovered texels.
    void resolve(std::uint32_t i, std::uint32_t j, const FrameAxes &axes, Images &images) {
        const auto size = options_.frame_size, samples = options_.samples, side = size * samples;
        const auto atlas = options_.frames * size;
        struct Group {
            std::uint32_t triangle{}, count{};
            float w1{}, w2{};
        };
        std::vector<Group> groups;
        covered_.assign(std::size_t(size) * size, 0);
        for (std::uint32_t t = 0; t < size; ++t)
            for (std::uint32_t s = 0; s < size; ++s) {
                groups.clear();
                std::uint32_t covered = 0;
                for (std::uint32_t y = t * samples; y < (t + 1) * samples; ++y)
                    for (std::uint32_t x = s * samples; x < (s + 1) * samples; ++x) {
                        const auto sample = std::size_t(y) * side + x;
                        if (!hit_[sample])
                            continue;
                        ++covered;
                        auto group = std::find_if(groups.begin(), groups.end(),
                                                  [&](const Group &g) { return g.triangle == hit_[sample] - 1; });
                        if (group == groups.end()) {
                            group = groups.insert(groups.end(), Group{});
                            group->triangle = hit_[sample] - 1;
                        }
                        ++group->count;
                        group->w1 += weights_[sample][0];
                        group->w2 += weights_[sample][1];
                    }
                if (!covered)
                    continue;
                Surface sum;
                for (const auto &group : groups) {
                    const auto n = float(group.count);
                    const auto value = shade(triangles_[group.triangle], group.w1 / n, group.w2 / n,
                                             footprint_[group.triangle], axes, front_[group.triangle] != 0);
                    sum.color = sum.color + value.color * n;
                    sum.normal = sum.normal + value.normal * n;
                    sum.emission = sum.emission + value.emission * n;
                    sum.height += value.height * n;
                    sum.occlusion += value.occlusion * n;
                    sum.roughness += value.roughness * n;
                    sum.metallic += value.metallic * n;
                }
                const auto share = 1 / float(covered);
                const auto texel = ((std::size_t{j} * size + t) * atlas + std::size_t{i} * size + s) * 4;
                const auto color = sum.color * share, emission = sum.emission * (share / emission_scale_);
                const auto normal = normalized(sum.normal);
                images.color[texel] = encode_srgb(color.x);
                images.color[texel + 1] = encode_srgb(color.y);
                images.color[texel + 2] = encode_srgb(color.z);
                images.color[texel + 3] = to_byte(float(covered) / float(samples * samples));
                images.normal_depth[texel] = to_byte(normal.x * .5F + .5F);
                images.normal_depth[texel + 1] = to_byte(normal.y * .5F + .5F);
                images.normal_depth[texel + 2] = to_byte(normal.z * .5F + .5F);
                images.normal_depth[texel + 3] = to_byte(sum.height * share / radius_ * .5F + .5F);
                images.surface[texel] = to_byte(sum.occlusion * share);
                images.surface[texel + 1] = to_byte(sum.roughness * share);
                images.surface[texel + 2] = to_byte(sum.metallic * share);
                if (emits_) {
                    images.emissive[texel] = encode_srgb(emission.x);
                    images.emissive[texel + 1] = encode_srgb(emission.y);
                    images.emissive[texel + 2] = encode_srgb(emission.z);
                }
                covered_[std::size_t(t) * size + s] = 1;
            }
        dilate(i, j, images);
    }
    // Gives each uncovered texel of frame (i, j) the values of the nearest covered texel, by steps between neighbors,
    // keeping its coverage 0.
    void dilate(std::uint32_t i, std::uint32_t j, Images &images) {
        const auto size = options_.frame_size, atlas = options_.frames * size;
        std::deque<std::uint32_t> queue;
        for (std::uint32_t texel = 0; texel < size * size; ++texel)
            if (covered_[texel])
                queue.push_back(texel);
        const auto offset = [&](std::uint32_t texel) {
            return ((std::size_t{j} * size + texel / size) * atlas + std::size_t{i} * size + texel % size) * 4;
        };
        while (!queue.empty()) {
            const auto texel = queue.front();
            queue.pop_front();
            const auto s = texel % size, t = texel / size;
            const std::array<std::pair<long, long>, 4> steps{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
            for (const auto &[dx, dy] : steps) {
                const long x = long(s) + dx, y = long(t) + dy;
                if (x < 0 || y < 0 || x >= long(size) || y >= long(size))
                    continue;
                const auto next = std::uint32_t(y) * size + std::uint32_t(x);
                if (covered_[next])
                    continue;
                covered_[next] = 1;
                const auto from = offset(texel), to = offset(next);
                for (std::size_t c = 0; c < 3; ++c) {
                    images.color[to + c] = images.color[from + c];
                    images.surface[to + c] = images.surface[from + c];
                    if (emits_)
                        images.emissive[to + c] = images.emissive[from + c];
                }
                for (std::size_t c = 0; c < 4; ++c)
                    images.normal_depth[to + c] = images.normal_depth[from + c];
                queue.push_back(next);
            }
        }
    }

    ImpostorOptions options_;
    std::vector<BakeMaterial> materials_;
    std::map<std::pair<int, float>, Sampled> samplers_;
    std::vector<BakeVertex> vertices_;
    std::vector<BakeTriangle> triangles_;
    Vec3 center_;
    float radius_{}, emission_scale_ = 1;
    bool emits_{};
    // Per frame: each vertex's position across the frame in samples and its height toward the viewpoint, the
    // triangles nearest first, and for each sample the height, triangle (plus 1, 0 for none) and barycentric weights of
    // the nearest surface; each triangle's facing and texture footprint; and which texels are covered.
    std::vector<Projected> projected_;
    std::vector<std::uint32_t> order_;
    std::vector<float> nearest_;
    std::vector<float> depth_;
    std::vector<std::uint32_t> hit_;
    std::vector<std::array<float, 2>> weights_;
    std::vector<std::uint8_t> front_;
    std::vector<float> footprint_;
    std::vector<std::uint8_t> covered_;
};
} // namespace

ImpostorAtlas bake_impostor(const Mesh &mesh, const ImpostorOptions &options) {
    require(known(options.layout), "Unknown impostor layout");
    require(options.frames >= minimum_frames && options.frames <= maximum_frames && options.frame_size >= 8 &&
                options.frame_size <= 1024 && options.frames * options.frame_size <= 8192 && options.samples >= 1 &&
                options.samples <= 8,
            options_message);
    return Baker(mesh, options).bake();
}

std::shared_ptr<const Mesh> Mesh::compile_impostor(const ImpostorAtlas &atlas, TexelRetention texel_retention) {
    const auto &frames = atlas.frames;
    require(known(frames.layout), "Unknown impostor layout");
    require(frames.count >= minimum_frames && frames.count <= maximum_frames && finite(frames.center) &&
                std::isfinite(frames.radius) && frames.radius > 0,
            frames_message);
    const auto side = atlas.color.image ? atlas.color.image->width : 0;
    const auto square = [&](const Texture &texture, TextureEncoding encoding) {
        return texture.image && texture.image->width == side && texture.image->height == side &&
               texture.encoding == encoding;
    };
    require(side && side % frames.count == 0 && square(atlas.color, TextureEncoding::srgb) &&
                square(atlas.normal_depth, TextureEncoding::linear) && square(atlas.surface, TextureEncoding::linear) &&
                (!atlas.emissive || square(*atlas.emissive, TextureEncoding::srgb)),
            images_message);
    require(std::isfinite(atlas.emission_scale) && atlas.emission_scale >= 1,
            "Impostor emission scale must be finite and at least 1");
    Asset asset;
    asset.nodes.resize(1);
    asset.nodes[0].name = "Impostor";
    // The shaders read each frame up to half a texel of the level they sample from its edges, so every map filters
    // linearly within and between levels and clamps at the atlas's edges, whatever its sampler says.
    const auto clamped = [](Texture texture) {
        texture.sampler = {Filter::linear, Filter::linear, Filter::linear, Wrap::clamp, Wrap::clamp, true};
        return texture;
    };
    asset.textures = {clamped(atlas.color), clamped(atlas.normal_depth), clamped(atlas.surface)};
    Material material;
    material.name = "Impostor";
    material.texture = 0;
    material.normal_texture = 1;
    material.metallic_roughness_texture = material.occlusion_texture = 2;
    material.metallic = material.roughness = 1;
    material.alpha_mode = AlphaMode::mask;
    material.alpha_cutoff = .5F;
    if (atlas.emissive) {
        asset.textures.push_back(clamped(*atlas.emissive));
        material.emissive_texture = 3;
        material.emissive = {atlas.emission_scale, atlas.emission_scale, atlas.emission_scale};
    }
    asset.materials.push_back(material);
    // The quad's corners, by texture coordinate, at opposite corners of the cube around the sphere, so that its bounds
    // are that cube. The renderer places each corner facing the eye from its texture coordinate alone.
    const auto &c = frames.center;
    const auto r = frames.radius;
    std::array<SourceVertex, 4> corners{};
    corners[0].position = c + Vec3{-r, -r, -r};
    corners[1].position = c + Vec3{r, -r, r};
    corners[2].position = c + Vec3{r, r, r};
    corners[3].position = c + Vec3{-r, r, -r};
    corners[0].uv = {0, 0};
    corners[1].uv = {1, 0};
    corners[2].uv = {1, 1};
    corners[3].uv = {0, 1};
    for (auto &corner : corners)
        corner.normal = {0, 0, 1};
    SourcePrimitive quad;
    quad.material = 0;
    quad.mesh_name = "Impostor";
    for (const auto k : {0, 1, 2, 0, 2, 3})
        quad.vertices.push_back(corners[std::size_t(k)]);
    asset.primitives.push_back(std::move(quad));
    auto result = std::make_shared<Mesh>(*compile(asset, {.texel_retention = texel_retention}));
    result->impostor_ = frames;
    return result;
}
} // namespace anima
