// Impostors: what bake_impostor() writes into each frame, checked against ImpostorFrames' layout from the mesh's own
// geometry, and the Mesh that Mesh::compile_impostor() makes of an atlas. GPU checks cover how the renderer draws it.
#include <algorithm>
#include <anima/impostor.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

using namespace anima;

namespace {
// A unit sphere of @p rings by @p segments quads around the origin, with smooth outward normals.
SourcePrimitive sphere(int rings, int segments) {
    SourcePrimitive primitive;
    primitive.material = 0;
    const auto corner = [&](int ring, int segment) {
        const float theta = std::numbers::pi_v<float> * float(ring) / float(rings),
                    phi = 2 * std::numbers::pi_v<float> * float(segment % segments) / float(segments);
        SourceVertex vertex;
        vertex.position = ring == 0 ? Vec3{0, 1, 0}
                          : ring == rings
                              ? Vec3{0, -1, 0}
                              : Vec3{std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
        vertex.normal = vertex.position;
        vertex.uv = {float(segment) / float(segments), float(ring) / float(rings)};
        return vertex;
    };
    for (int ring = 0; ring < rings; ++ring)
        for (int segment = 0; segment < segments; ++segment) {
            const auto a = corner(ring, segment), b = corner(ring + 1, segment), c = corner(ring + 1, segment + 1),
                       d = corner(ring, segment + 1);
            for (const auto &v : {a, c, b, a, d, c})
                primitive.vertices.push_back(v);
        }
    return primitive;
}
Material colored(Vec3 factor) {
    Material material;
    material.factor = factor;
    material.roughness = .6F;
    material.metallic = .2F;
    return material;
}
Asset sphere_asset() {
    Asset asset;
    asset.nodes.resize(1);
    asset.materials.push_back(colored({1, .5F, .25F}));
    asset.primitives.push_back(sphere(32, 64));
    return asset;
}
// A square from (-1, -1) to (1, 1) in the XY plane facing +Z, its quarters colored red (-X, -Y), green (+X, -Y), blue
// (-X, +Y) and white (+X, +Y), its texture coordinates (x, y) mapped from [-1, 1] to [0, 1].
Asset quarters_asset(bool double_sided) {
    Asset asset;
    asset.nodes.resize(1);
    Material material;
    material.double_sided = double_sided;
    asset.materials.push_back(material);
    SourcePrimitive primitive;
    primitive.material = 0;
    for (const auto &[x0, y0, color] : {std::tuple{-1.F, -1.F, Vec3{1, 0, 0}}, std::tuple{0.F, -1.F, Vec3{0, 1, 0}},
                                        std::tuple{-1.F, 0.F, Vec3{0, 0, 1}}, std::tuple{0.F, 0.F, Vec3{1, 1, 1}}}) {
        const auto at = [&](float x, float y) {
            SourceVertex vertex;
            vertex.position = {x, y, 0};
            vertex.normal = {0, 0, 1};
            vertex.color = color;
            vertex.uv = {(x + 1) / 2, (y + 1) / 2};
            return vertex;
        };
        const auto a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0 + 1, y0 + 1), d = at(x0, y0 + 1);
        for (const auto &v : {a, b, c, a, c, d})
            primitive.vertices.push_back(v);
    }
    asset.primitives.push_back(std::move(primitive));
    return asset;
}
// The direction and axes of frame (i, j), computed from ImpostorFrames' description.
struct Frame {
    Vec3 direction, right, up;
};
Frame documented_frame(ImpostorLayout layout, std::uint32_t count, std::uint32_t i, std::uint32_t j) {
    const double u = 2.0 * i / (count - 1) - 1, v = 2.0 * j / (count - 1) - 1;
    double x = layout == ImpostorLayout::hemisphere ? (u + v) / 2 : u,
           z = layout == ImpostorLayout::hemisphere ? (u - v) / 2 : v;
    const double y = 1 - std::abs(x) - std::abs(z);
    if (y < 0 && layout == ImpostorLayout::sphere) {
        const double folded_x = (1 - std::abs(z)) * (x < 0 ? -1 : 1);
        z = (1 - std::abs(x)) * (z < 0 ? -1 : 1);
        x = folded_x;
    }
    const auto d = normalized({float(x), float(std::max(y, layout == ImpostorLayout::hemisphere ? 0.0 : y)), float(z)});
    const auto right = d.x == 0 && d.z == 0 ? Vec3{1, 0, 0} : normalized({d.z, 0, -d.x});
    return {d, right, cross(d, right)};
}
// Texel (s, t) of frame (i, j) of @p texture, as RGBA bytes.
std::array<int, 4> texel(const ImpostorAtlas &atlas, const Texture &texture, std::uint32_t i, std::uint32_t j,
                         std::uint32_t s, std::uint32_t t) {
    const auto size = atlas.color.image->width / atlas.frames.frames_per_side;
    const auto &image = *texture.image;
    const auto *p = &image.rgba[(std::size_t(j * size + t) * image.width + i * size + s) * 4];
    return {p[0], p[1], p[2], p[3]};
}
ImpostorOptions options(std::uint32_t frames_per_side, std::uint32_t frame_size, std::uint32_t samples,
                        ImpostorLayout layout = ImpostorLayout::hemisphere) {
    ImpostorOptions value;
    value.layout = layout;
    value.frames_per_side = frames_per_side;
    value.frame_size = frame_size;
    value.samples = samples;
    return value;
}
int srgb_byte(float linear) {
    return int(
        std::lround(255 * (linear <= .0031308F ? linear * 12.92F : 1.055F * std::pow(linear, 1 / 2.4F) - .055F)));
}
} // namespace

TEST_CASE("Baking rejects options out of range and meshes it cannot bake") {
    const auto mesh = Mesh::compile(sphere_asset());
    const char *options_message =
        "Impostor options must give 2 to 32 frames of 8 to 1024 texels, at most 8192 in all, and 1 to 8 samples";
    for (const auto &invalid : {options(1, 8, 1), options(33, 8, 1, ImpostorLayout::sphere), options(4, 7, 1),
                                options(4, 1025, 1), options(32, 512, 1), options(4, 8, 0), options(4, 8, 9)})
        CHECK_THROWS_WITH_AS((void)bake_impostor(*mesh, invalid), options_message, std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)bake_impostor(*mesh, options(4, 8, 1, static_cast<ImpostorLayout>(7))),
                         "Unknown impostor layout", std::invalid_argument);

    auto skinned = sphere_asset();
    skinned.nodes.resize(2);
    skinned.skins.push_back({{1}, {identity()}});
    skinned.primitives[0].skin = 0;
    for (auto &vertex : skinned.primitives[0].vertices)
        vertex.weights = {1, 0, 0, 0};
    CHECK_THROWS_WITH_AS((void)bake_impostor(*Mesh::compile(skinned)), "Impostors bake only rigid meshes",
                         std::invalid_argument);
    for (const bool unlit : {false, true}) {
        auto asset = sphere_asset();
        asset.materials[0].alpha_mode = unlit ? AlphaMode::opaque : AlphaMode::blend;
        asset.materials[0].unlit = unlit;
        CHECK_THROWS_WITH_AS((void)bake_impostor(*Mesh::compile(asset)),
                             "Impostors bake only lit opaque and masked materials", std::invalid_argument);
    }
    Asset empty;
    empty.nodes.resize(1);
    CHECK_THROWS_WITH_AS((void)bake_impostor(*Mesh::compile(empty)), "An impostor requires a mesh with triangles",
                         std::invalid_argument);
}

TEST_CASE("Each frame views the mesh from its documented direction") {
    // A smooth unit sphere looks the same from everywhere, so every frame must show the disc that it projects to, with
    // the heights and normals of the sphere's front from that frame's direction.
    const auto mesh = Mesh::compile(sphere_asset());
    for (const auto layout : {ImpostorLayout::hemisphere, ImpostorLayout::sphere}) {
        CAPTURE(int(layout));
        const auto atlas = bake_impostor(*mesh, options(4, 32, 4, layout));
        CHECK(atlas.frames.layout == layout);
        CHECK(atlas.frames.frames_per_side == 4);
        CHECK(length(atlas.frames.center) < 1e-6F);
        CHECK(atlas.frames.radius == doctest::Approx(1).epsilon(1e-6));
        CHECK(atlas.color.image->width == 128);
        CHECK(atlas.color.encoding == TextureEncoding::srgb);
        CHECK(atlas.normal_depth.encoding == TextureEncoding::linear);
        CHECK(atlas.surface.encoding == TextureEncoding::linear);
        CHECK_FALSE(atlas.emissive);
        CHECK(atlas.emission_scale == 1);
        std::size_t inside = 0, outside = 0, wrong = 0;
        for (std::uint32_t j = 0; j < 4; ++j)
            for (std::uint32_t i = 0; i < 4; ++i) {
                const auto frame = documented_frame(layout, 4, i, j);
                for (std::uint32_t t = 0; t < 32; ++t)
                    for (std::uint32_t s = 0; s < 32; ++s) {
                        const float a = (float(s) + .5F) / 16 - 1, b = (float(t) + .5F) / 16 - 1;
                        const float rho = std::sqrt(a * a + b * b);
                        const auto color = texel(atlas, atlas.color, i, j, s, t);
                        if (rho > 1.05F) {
                            ++outside;
                            wrong += color[3] != 0;
                            continue;
                        }
                        if (rho > .9F)
                            continue;
                        ++inside;
                        const float height = std::sqrt(1 - rho * rho);
                        const auto expected = frame.right * a + frame.up * b + frame.direction * height;
                        const auto normal = texel(atlas, atlas.normal_depth, i, j, s, t);
                        const auto surface = texel(atlas, atlas.surface, i, j, s, t);
                        const auto decoded = [](int byte) { return float(byte) / 255 * 2 - 1; };
                        wrong += color[3] != 255 || std::abs(color[0] - srgb_byte(1)) > 1 ||
                                 std::abs(color[1] - srgb_byte(.5F)) > 1 || std::abs(color[2] - srgb_byte(.25F)) > 1 ||
                                 std::abs(decoded(normal[3]) - height) > .02F ||
                                 std::abs(decoded(normal[0]) - expected.x) > .03F ||
                                 std::abs(decoded(normal[1]) - expected.y) > .03F ||
                                 std::abs(decoded(normal[2]) - expected.z) > .03F || surface[0] != 255 ||
                                 surface[1] != 153 || surface[2] != 51 || surface[3] != 255;
                    }
            }
        CHECK(inside > 16 * 500);
        CHECK(outside > 16 * 100);
        CHECK(wrong == 0);
    }
}

TEST_CASE("Frames lay out their texture axes and faces as documented") {
    // From +Z, a hemisphere's frame (4, 0) of 5, the square shows its quarters with +X to the right of the frame and +Y
    // up it, texture row 0 at the bottom. A single-sided square seen from behind, from -Z in frame (0, 4), shows
    // nothing; a double-sided one shows its back, mirrored, with normals toward the viewpoint.
    for (const bool double_sided : {false, true}) {
        CAPTURE(double_sided);
        const auto atlas = bake_impostor(*Mesh::compile(quarters_asset(double_sided)), options(5, 16, 4));
        // The square's corners lie sqrt(2) from its center, so it spans texels 3 to 12 of each frame's 16.
        CHECK(atlas.frames.radius == doctest::Approx(std::sqrt(2.F)));
        const auto quarter = [&](std::uint32_t i, std::uint32_t j, std::uint32_t s, std::uint32_t t) {
            const auto c = texel(atlas, atlas.color, i, j, s, t);
            return std::array{c[0], c[1], c[2], c[3]};
        };
        CHECK(quarter(4, 0, 5, 5) == std::array{255, 0, 0, 255});
        CHECK(quarter(4, 0, 10, 5) == std::array{0, 255, 0, 255});
        CHECK(quarter(4, 0, 5, 10) == std::array{0, 0, 255, 255});
        CHECK(quarter(4, 0, 10, 10) == std::array{255, 255, 255, 255});
        CHECK(texel(atlas, atlas.normal_depth, 4, 0, 8, 8)[2] == 255);
        // Outside the square, each texel takes the color of the nearest covered one, uncovered.
        CHECK(quarter(4, 0, 10, 14) == std::array{255, 255, 255, 0});
        CHECK(quarter(4, 0, 1, 1) == std::array{255, 0, 0, 0});
        if (!double_sided) {
            for (std::uint32_t t = 0; t < 16; ++t)
                for (std::uint32_t s = 0; s < 16; ++s) {
                    CHECK(texel(atlas, atlas.color, 0, 4, s, t) == std::array{0, 0, 0, 0});
                    CHECK(texel(atlas, atlas.normal_depth, 0, 4, s, t) == std::array{0, 0, 0, 0});
                }
            continue;
        }
        // From -Z, right is -X, so the frame's right half shows the square's left half.
        CHECK(quarter(0, 4, 5, 5) == std::array{0, 255, 0, 255});
        CHECK(quarter(0, 4, 10, 10) == std::array{0, 0, 255, 255});
        CHECK(texel(atlas, atlas.normal_depth, 0, 4, 8, 8)[2] == 0);
    }
    // Edge-on, from directly above, the square covers nothing.
    const auto above = bake_impostor(*Mesh::compile(quarters_asset(true)), options(5, 16, 4));
    for (std::uint32_t t = 0; t < 16; ++t)
        for (std::uint32_t s = 0; s < 16; ++s)
            CHECK(texel(above, above.color, 2, 2, s, t)[3] == 0);
}

TEST_CASE("Masked materials cover only where their alpha reaches the cutoff") {
    auto asset = quarters_asset(true);
    auto image = std::make_shared<Image>();
    image->width = 2;
    image->height = 1;
    image->rgba = {255, 255, 255, 255, 255, 255, 255, 0};
    Sampler sampler;
    sampler.mag = sampler.min = Filter::nearest;
    sampler.mipmapped = false;
    asset.textures.push_back({image, sampler, TextureEncoding::srgb});
    asset.materials[0].texture = 0;
    asset.materials[0].alpha_mode = AlphaMode::mask;
    const auto atlas = bake_impostor(*Mesh::compile(asset), options(5, 16, 4));
    // The texture's opaque texel covers the square's -X half.
    for (std::uint32_t t = 4; t < 12; ++t) {
        CHECK(texel(atlas, atlas.color, 4, 0, 5, t)[3] == 255);
        CHECK(texel(atlas, atlas.color, 4, 0, 10, t)[3] == 0);
    }
}

TEST_CASE("Normal maps bend the baked normals in either tangent frame") {
    // A normal map that tilts every normal fully toward its tangent, +X along the texture's u, from authored tangents
    // and from the triangles' texture coordinates.
    for (const bool authored : {true, false}) {
        CAPTURE(authored);
        auto asset = quarters_asset(true);
        auto image = std::make_shared<Image>();
        image->width = image->height = 1;
        image->rgba = {255, 128, 128, 255};
        Sampler sampler;
        sampler.mipmapped = false;
        asset.textures.push_back({image, sampler, TextureEncoding::linear});
        asset.materials[0].normal_texture = 0;
        if (authored)
            for (auto &vertex : asset.primitives[0].vertices)
                vertex.tangent = {1, 0, 0, 1};
        const auto atlas = bake_impostor(*Mesh::compile(asset), options(5, 16, 4));
        const auto normal = texel(atlas, atlas.normal_depth, 4, 0, 8, 8);
        CHECK(normal[0] == 255);
        CHECK(std::abs(normal[1] - 128) <= 1);
        CHECK(std::abs(normal[2] - 128) <= 1);
    }
}

TEST_CASE("Emission bakes divided by its scale") {
    auto asset = quarters_asset(true);
    asset.materials[0].emissive = {2, 1, 0};
    const auto atlas = bake_impostor(*Mesh::compile(asset), options(5, 16, 4));
    REQUIRE(atlas.emissive);
    CHECK(atlas.emissive->encoding == TextureEncoding::srgb);
    CHECK(atlas.emission_scale == 2);
    const auto emitted = texel(atlas, *atlas.emissive, 4, 0, 8, 8);
    CHECK(emitted == std::array{255, srgb_byte(.5F), 0, 255});
}

TEST_CASE("An impostor mesh draws one quad around its source's center") {
    auto asset = sphere_asset();
    // Placed off the origin and scaled, so that the frames' center and radius come from the rest pose.
    asset.nodes[0].rest.translation = {3, 4, 5};
    asset.nodes[0].rest.scale = {2, 2, 2};
    const auto source = Mesh::compile(asset);
    CHECK(source->impostor() == nullptr);
    const auto atlas = bake_impostor(*source, options(4, 16, 2, ImpostorLayout::sphere));
    CHECK(atlas.frames.center.x == doctest::Approx(3));
    CHECK(atlas.frames.center.y == doctest::Approx(4));
    CHECK(atlas.frames.center.z == doctest::Approx(5));
    CHECK(atlas.frames.radius == doctest::Approx(2).epsilon(1e-5));
    for (const auto retention : {TexelRetention::keep, TexelRetention::until_upload}) {
        const auto mesh = Mesh::compile_impostor(atlas, retention);
        REQUIRE(mesh->impostor());
        CHECK(mesh->impostor()->layout == ImpostorLayout::sphere);
        CHECK(mesh->impostor()->frames_per_side == 4);
        CHECK(mesh->impostor()->radius == atlas.frames.radius);
        CHECK(mesh->texel_retention() == retention);
        REQUIRE(mesh->primitives().size() == 1);
        CHECK(mesh->primitives()[0].index_count == 6);
        CHECK(mesh->primitives()[0].levels.empty());
        const auto &bounds = mesh->rest_bounds();
        const auto &expected = source->rest_bounds();
        for (const auto axis : {0, 1, 2}) {
            const auto center = [&](const RenderBounds &b) {
                return (std::array{b.minimum.x, b.minimum.y, b.minimum.z}[axis] +
                        std::array{b.maximum.x, b.maximum.y, b.maximum.z}[axis]) /
                       2;
            };
            CHECK(center(bounds) == doctest::Approx(center(expected)));
            CHECK(std::array{bounds.maximum.x, bounds.maximum.y, bounds.maximum.z}[axis] -
                      std::array{bounds.minimum.x, bounds.minimum.y, bounds.minimum.z}[axis] ==
                  doctest::Approx(2 * atlas.frames.radius));
        }
        const auto &material = mesh->description()->materials.at(0);
        CHECK(material.alpha_mode == AlphaMode::mask);
        CHECK(material.alpha_cutoff == .5F);
        CHECK(material.double_sided);
        CHECK(material.texture == 0);
        CHECK(material.normal_texture == 1);
        CHECK(material.metallic_roughness_texture == 2);
        CHECK(material.occlusion_texture == 2);
        CHECK(material.emissive_texture == -1);
        CHECK(material.metallic == 1);
        CHECK(material.roughness == 1);
    }
    auto emissive = atlas;
    emissive.emissive = atlas.color;
    emissive.emission_scale = 3;
    const auto glowing = Mesh::compile_impostor(emissive);
    const auto &material = glowing->description()->materials.at(0);
    CHECK(material.emissive_texture == 3);
    CHECK(material.emissive.x == 3);
}

TEST_CASE("Mips of an impostor's color keep the color spread into its uncovered texels") {
    // Every texel of the sphere's frames holds its base color, those it covers and those that dilation fills around
    // them. Mips built as the renderer builds a masked base color's keep that color at every level whose texels lie
    // within one frame, transparent texels included, so filtering never blends black into a silhouette.
    const auto atlas = bake_impostor(*Mesh::compile(sphere_asset()), options(4, 16, 2));
    const std::array expected{255, srgb_byte(.5F), srgb_byte(.25F)};
    const auto mips = texture_mips(atlas.color, {.alpha_coverage_cutoff = .5F});
    for (std::size_t level = 0; level <= 4; ++level) {
        CAPTURE(level);
        const auto &mip = mips.at(level);
        std::size_t uncovered = 0, off_color = 0;
        for (std::size_t i = 0; i < mip.rgba.size(); i += 4) {
            uncovered += mip.rgba[i + 3] == 0;
            for (std::size_t c = 0; c < 3; ++c)
                off_color += std::abs(int(mip.rgba[i + c]) - expected[c]) > 1;
        }
        CHECK(off_color == 0);
        if (level == 1)
            CHECK(uncovered > 0); // Whole blocks of uncovered texels, which zero-weighted averaging used to blacken.
    }
}

TEST_CASE("An impostor's maps sample linearly and clamped, whatever their samplers say") {
    auto atlas = bake_impostor(*Mesh::compile(sphere_asset()), options(4, 16, 1));
    atlas.emissive = atlas.color;
    for (auto *texture : {&atlas.color, &atlas.normal_depth, &atlas.surface, &*atlas.emissive})
        texture->sampler = {Filter::nearest, Filter::nearest, Filter::nearest, Wrap::repeat, Wrap::mirror, false};
    const auto mesh = Mesh::compile_impostor(atlas);
    const auto &textures = mesh->description()->textures;
    REQUIRE(textures.size() == 4);
    for (const auto &texture : textures) {
        CHECK(texture.sampler.mag == Filter::linear);
        CHECK(texture.sampler.min == Filter::linear);
        CHECK(texture.sampler.mip == Filter::linear);
        CHECK(texture.sampler.u == Wrap::clamp);
        CHECK(texture.sampler.v == Wrap::clamp);
        CHECK(texture.sampler.mipmapped);
    }
}

TEST_CASE("Compiling an impostor rejects frames and images it cannot draw") {
    const auto atlas = bake_impostor(*Mesh::compile(sphere_asset()), options(4, 16, 1));
    const char *frames_message =
        "Impostor frames must number from 2 to 32 per side, with a finite center and a positive finite radius";
    const char *images_message = "Impostor images must be equal squares divisible into the frames, color and emission "
                                 "sRGB, normals and surface linear";
    const auto rejects = [](const ImpostorAtlas &value, const char *message) {
        CHECK_THROWS_WITH_AS((void)Mesh::compile_impostor(value), message, std::invalid_argument);
    };
    for (const auto count : {1U, 33U, 3U}) {
        auto value = atlas;
        value.frames.frames_per_side = count;
        // 64 texels do not divide into 3 frames.
        rejects(value, count == 3 ? images_message : frames_message);
    }
    for (const float radius : {0.F, -1.F, std::numeric_limits<float>::infinity(), std::nanf("")}) {
        auto value = atlas;
        value.frames.radius = radius;
        rejects(value, frames_message);
    }
    auto value = atlas;
    value.frames.center.y = std::nanf("");
    rejects(value, frames_message);
    value = atlas;
    value.frames.layout = static_cast<ImpostorLayout>(7);
    rejects(value, "Unknown impostor layout");
    const auto resized = [&](std::uint32_t width, std::uint32_t height) {
        auto image = std::make_shared<Image>();
        image->width = width;
        image->height = height;
        image->rgba.assign(std::size_t(width) * height * 4, 0);
        return std::shared_ptr<const Image>(image);
    };
    for (const auto &[width, height] : {std::pair{64U, 32U}, std::pair{32U, 32U}, std::pair{0U, 0U}}) {
        value = atlas;
        value.surface.image = resized(width, height);
        rejects(value, images_message);
    }
    value = atlas;
    value.color.image = nullptr;
    rejects(value, images_message);
    value = atlas;
    value.color.encoding = TextureEncoding::linear;
    rejects(value, images_message);
    value = atlas;
    value.normal_depth.encoding = TextureEncoding::srgb;
    rejects(value, images_message);
    value = atlas;
    value.emissive = atlas.surface;
    rejects(value, images_message);
    for (const float scale : {.5F, std::numeric_limits<float>::infinity()}) {
        value = atlas;
        value.emission_scale = scale;
        rejects(value, "Impostor emission scale must be finite and at least 1");
    }
    CHECK_THROWS_WITH_AS((void)Mesh::compile_impostor(atlas, static_cast<TexelRetention>(7)), "Unknown texel retention",
                         std::invalid_argument);
}
