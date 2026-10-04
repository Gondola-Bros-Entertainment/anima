#pragma once
#include "gpu_checks.hpp"
#include "reference.hpp"
#include "rejection.hpp"
#include "resources.hpp"
#include <anima/lighting.hpp>
#include <anima/scene_set.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
namespace environment_test {
inline std::shared_ptr<const anima::Asset> fixture(bool sloped = false) {
    auto asset = std::make_shared<anima::Asset>();
    asset->nodes.resize(1);
    anima::Material ground;
    ground.name = "ground";
    ground.factor = {.5F, .5F, .5F};
    asset->materials.push_back(ground);
    anima::Material canopy;
    canopy.name = "masked canopy";
    canopy.factor = {.4F, .1F, .5F};
    canopy.texture = 0;
    canopy.alpha_mode = anima::AlphaMode::mask;
    asset->materials.push_back(canopy);
    anima::Texture alpha{std::make_shared<anima::Image>(anima::Image{
                             2, 2, {255, 255, 255, 255, 255, 255, 255, 0, 255, 255, 255, 0, 255, 255, 255, 255}}),
                         {}};
    alpha.sampler.mag = alpha.sampler.min = anima::Filter::nearest;
    alpha.sampler.mipmapped = false;
    asset->textures.push_back(alpha);
    for (int layer = 0; layer < (sloped ? 1 : 2); ++layer) {
        const float extent = layer == 0 ? 8.F : 1.5F, y = layer == 0 ? 0.F : 3.F;
        anima::SourcePrimitive p;
        p.material = layer;
        for (const auto uv : {std::array<float, 2>{0, 0}, {0, 1}, {1, 1}, {0, 0}, {1, 1}, {1, 0}}) {
            anima::SourceVertex v;
            v.position = {(uv[0] * 2 - 1) * extent, y, (uv[1] * 2 - 1) * extent};
            // Deliberately retain a different shading normal. Shadow receiver
            // depth must follow actual geometry, not a smoothed/normal-mapped normal.
            if (sloped)
                v.position.y = .35F * v.position.x;
            v.normal = {0, 1, 0};
            v.uv = uv;
            v.tangent = {1, 0, 0, -1};
            p.vertices.push_back(v);
        }
        asset->primitives.push_back(std::move(p));
    }
    return asset;
}
inline std::shared_ptr<const anima::Asset> curved_fixture() {
    auto asset = std::make_shared<anima::Asset>();
    asset->nodes.resize(1);
    anima::Material material;
    material.name = "smooth coarse cylinder";
    material.factor = {.5F, .5F, .5F};
    material.roughness = 1;
    asset->materials.push_back(material);
    anima::SourcePrimitive primitive;
    primitive.material = 0;
    constexpr int sides = 12;
    const auto vertex = [](int side, float y) {
        const float angle = side * (2 * std::numbers::pi_v<float> / sides);
        anima::SourceVertex v;
        v.normal = {std::sin(angle), 0, std::cos(angle)};
        v.position = {.3F * v.normal.x, y, .3F * v.normal.z};
        v.tangent = {std::cos(angle), 0, -std::sin(angle), 1};
        return v;
    };
    for (int side = 0; side < sides; ++side) {
        const auto a = vertex(side, -.4F), b = vertex(side + 1, -.4F);
        const auto c = vertex(side + 1, .4F), d = vertex(side, .4F);
        for (const auto &v : {a, b, c, a, c, d})
            primitive.vertices.push_back(v);
        for (const float y : {-.4F, .4F}) {
            anima::SourceVertex center;
            center.position = {0, y, 0};
            center.normal = {0, y > 0 ? 1.F : -1.F, 0};
            auto first = vertex(y > 0 ? side : side + 1, y);
            auto second = vertex(y > 0 ? side + 1 : side, y);
            first.normal = second.normal = center.normal;
            for (const auto &v : {center, first, second})
                primitive.vertices.push_back(v);
        }
    }
    asset->primitives.push_back(std::move(primitive));
    return asset;
}
constexpr std::size_t rgb_channels = 3;
// Mean of each channel over the pixels within @p radius of the pixel that contains @p at.
inline std::array<double, rgb_channels> window_mean(const gpu_check::Image &image, std::array<float, 2> at,
                                                    int radius) {
    const auto column = static_cast<long>(std::floor(at[0])), row = static_cast<long>(std::floor(at[1]));
    resource_test::require(column - radius >= 0 && row - radius >= 0 && column + radius < long(image.width) &&
                               row + radius < long(image.height),
                           "Sample window leaves the capture");
    std::array<double, rgb_channels> sum{};
    for (auto y = row - radius; y <= row + radius; ++y)
        for (auto x = column - radius; x <= column + radius; ++x)
            for (std::size_t c = 0; c < rgb_channels; ++c)
                sum[c] += image.rgb[(std::size_t(y) * image.width + std::size_t(x)) * rgb_channels + c];
    const auto count = double((2 * radius + 1) * (2 * radius + 1));
    for (auto &value : sum)
        value /= count;
    return sum;
}
// Pixel coordinates of @p world under @p view_projection in a @p width by @p height image.
inline std::array<float, 2> project(const anima::Mat4 &view_projection, anima::Vec3 world, std::uint32_t width,
                                    std::uint32_t height) {
    const auto clip = [&](unsigned row) {
        return view_projection[row] * world.x + view_projection[4 + row] * world.y +
               view_projection[8 + row] * world.z + view_projection[12 + row];
    };
    const auto w = clip(3);
    return {(clip(0) / w + 1) * .5F * float(width), (clip(1) / w + 1) * .5F * float(height)};
}
// Appends a square face centered on @p center that spans @p u and @p v either way, wound counterclockwise about
// its normal, cross(u, v).
inline void add_face(anima::SourcePrimitive &primitive, anima::Vec3 center, anima::Vec3 u, anima::Vec3 v) {
    const std::array corners{center - u - v, center + u - v, center + u + v, center - u + v};
    for (const auto corner : {0U, 1U, 2U, 0U, 2U, 3U}) {
        anima::SourceVertex vertex;
        vertex.position = corners[corner];
        vertex.normal = anima::normalized(anima::cross(u, v));
        primitive.vertices.push_back(vertex);
    }
}
// A matte box centered on its origin with half extents @p half, or with @p ground only its face toward +Y.
inline std::shared_ptr<const anima::Asset> box_fixture(anima::Vec3 half, bool ground = false) {
    auto asset = std::make_shared<anima::Asset>();
    asset->nodes.resize(1);
    anima::Material matte;
    matte.name = ground ? "ground" : "matte box";
    matte.factor = {.8F, .8F, .8F};
    asset->materials.push_back(matte);
    anima::SourcePrimitive primitive;
    primitive.material = 0;
    const anima::Vec3 x{half.x, 0, 0}, y{0, half.y, 0}, z{0, 0, half.z};
    if (ground)
        add_face(primitive, {}, z, x);
    else {
        add_face(primitive, x, y, z);
        add_face(primitive, x * -1.F, z, y);
        add_face(primitive, y, z, x);
        add_face(primitive, y * -1.F, x, z);
        add_face(primitive, z, x, y);
        add_face(primitive, z * -1.F, y, x);
    }
    asset->primitives.push_back(std::move(primitive));
    return asset;
}
// A mesh and its (-1, 1, 1) mirror image under the same light shade as mirror images, casting and receiving
// shadows alike. The sun, the camera and the shadow cascade that the view centers lie in the plane x = 0 that relates
// them, and each cube stands on its own ground tile, the mirrored cube's tile being the other tile mirrored.
template <class Capture>
void check_mirrored_shading(anima::VulkanRenderer &renderer, Capture &&capture, const gpu_check::Captures &images) {
    constexpr float half = .5F, offset = 1.5F, turn = .5235988F; // 30 degrees about +Y.
    constexpr anima::Vec3 tile_half{offset, 0, 3};               // Covers a cube and its shadow.
    constexpr float tile_z = -1;
    constexpr anima::Quat unrotated{0, 0, 0, 1};
    constexpr float matrix_tolerance = 1e-6F;
    constexpr int window_radius = 3;             // Pixels around each sample, which sits well inside its face.
    constexpr double face_tolerance = 3;         // Channel levels between the means of corresponding samples.
    constexpr double lit_margin = 30;            // Channel levels by which a sunlit face outshines its shadow.
    constexpr int symmetry_tolerance = 16;       // Channel levels between a pixel and its reflection.
    constexpr double mismatch_limit = .002;      // Fraction of reflected pixel pairs allowed beyond the tolerance.
    constexpr float shadow_z = -2.2F;            // Ground depth inside each cube's shadow and in view past it.
    constexpr anima::Vec3 sunlit{2.2F, 0, 1.2F}; // Ground in front of the right cube, outside every shadow.
    auto scene = std::make_shared<anima::Scene>();
    const auto tile = anima::Mesh::compile(*box_fixture(tile_half, true));
    auto ground = scene->create("ground", tile);
    auto mirrored_ground = scene->create("mirrored ground", tile);
    ground.set_world_transform({{-offset, 0, tile_z}, unrotated, {1, 1, 1}});
    mirrored_ground.set_world_transform({{offset, 0, tile_z}, unrotated, {-1, 1, 1}});
    const auto cube = anima::Mesh::compile(*box_fixture({half, half, half}));
    auto original = scene->create("original", cube);
    auto mirrored = scene->create("mirrored", cube);
    original.set_world_transform({{-offset, half, 0}, {0, std::sin(turn / 2), 0, std::cos(turn / 2)}, {1, 1, 1}});
    mirrored.set_world_transform({{offset, half, 0}, {0, -std::sin(turn / 2), 0, std::cos(turn / 2)}, {-1, 1, 1}});
    const anima::Mat4 reflection{-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (const auto &[left, right] : {std::pair{ground, mirrored_ground}, std::pair{original, mirrored}}) {
        const auto expected = reflection * left.world_matrix(), actual = right.world_matrix();
        for (std::size_t i = 0; i < actual.size(); ++i)
            resource_test::require(std::abs(actual[i] - expected[i]) < matrix_tolerance,
                                   "A mirrored object is not its counterpart's reflection");
    }
    anima::Environment lighting;
    lighting.sun.direction = {0, .342F, .94F}; // 20 degrees above the horizon, behind the camera.
    lighting.sun.radiance = {2.5F, 2.5F, 2.5F};
    lighting.fill.radiance = {};
    lighting.ambient_sky = {.25F, .3F, .4F};
    lighting.ambient_ground = {.04F, .03F, .02F};
    // One cascade over the 10 m in view, with texels of about 7 mm.
    lighting.shadow_cascades.enabled = true;
    lighting.shadow_cascades.count = 1;
    lighting.shadow_cascades.distance = 10;
    const auto view =
        anima::perspective(std::numbers::pi_v<float> / 4, 4.F / 3, .05F, 50) * anima::look_at({0, 5, 3}, {0, 0, -1});
    renderer.set_view(view);
    renderer.set_environment(lighting);
    renderer.set_scenes({scene});
    capture("mirrored-shading");
    const auto &image = images["mirrored-shading"];
    struct Sample {
        const char *name;
        anima::Vec3 original, mirrored;
    };
    const auto face = [&](const char *name, anima::Vec3 local) {
        return Sample{name, anima::point(original.world_matrix(), local), anima::point(mirrored.world_matrix(), local)};
    };
    const std::array samples{face("front face", {0, 0, half}), face("top face", {0, half, 0}),
                             Sample{"ground shadow", {-offset, 0, shadow_z}, {offset, 0, shadow_z}},
                             Sample{"sunlit ground", {-sunlit.x, sunlit.y, sunlit.z}, sunlit}};
    std::array<std::array<double, rgb_channels>, std::tuple_size_v<decltype(samples)>> originals{};
    std::ostringstream report;
    bool matched = true;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        originals[i] = window_mean(image, project(view, samples[i].original, image.width, image.height), window_radius);
        const auto reflected =
            window_mean(image, project(view, samples[i].mirrored, image.width, image.height), window_radius);
        report << samples[i].name << ": original";
        for (const auto value : originals[i])
            report << ' ' << std::lround(value);
        report << ", mirrored";
        for (const auto value : reflected)
            report << ' ' << std::lround(value);
        report << "; ";
        for (std::size_t c = 0; c < rgb_channels; ++c)
            matched = matched && std::abs(originals[i][c] - reflected[c]) <= face_tolerance;
    }
    std::size_t mismatched = 0;
    for (std::uint32_t y = 0; y < image.height; ++y)
        for (std::uint32_t x = 0; x < image.width / 2; ++x) {
            const auto left = (std::size_t(y) * image.width + x) * rgb_channels;
            const auto right = (std::size_t(y) * image.width + (image.width - 1 - x)) * rgb_channels;
            for (std::size_t c = 0; c < rgb_channels; ++c)
                if (std::abs(int(image.rgb[left + c]) - int(image.rgb[right + c])) > symmetry_tolerance) {
                    ++mismatched;
                    break;
                }
        }
    const auto mismatch = double(mismatched) / (double(image.width / 2) * image.height);
    report << "reflected pixels beyond " << symmetry_tolerance << " levels: " << mismatch * 100 << "%";
    std::cout << "MIRRORED SHADING " << report.str() << '\n';
    // Without light and shadow to compare, matching samples would prove nothing.
    images.require(originals[0][0] > originals[2][0] + lit_margin && originals[3][0] > originals[2][0] + lit_margin,
                   "The original cube's front face or its ground is not sunlit, or its shadow is missing",
                   {"mirrored-shading"});
    images.require(matched && mismatch <= mismatch_limit, "Mirrored draws shade differently: " + report.str(),
                   {"mirrored-shading"});

    // The CPU reference draws the scene's snapshot through identity matrices, so the mirrored cube and tile shade
    // as they do directly only when the snapshot keeps each triangle's winding against its normals.
    renderer.set_scenes({reference_test::scene(scene->snapshot())});
    capture("mirrored-reference");
    const auto &reference = images["mirrored-reference"];
    images.require(gpu_check::same_size(reference, image), "The reference capture's size differs from the direct one's",
                   {"mirrored-shading", "mirrored-reference"});
    std::ostringstream reference_report;
    bool reference_matched = true;
    for (const auto &sample : samples)
        for (const bool mirrored_side : {false, true}) {
            const auto pixel =
                project(view, mirrored_side ? sample.mirrored : sample.original, image.width, image.height);
            const auto direct = window_mean(image, pixel, window_radius);
            const auto through_reference = window_mean(reference, pixel, window_radius);
            reference_report << sample.name << (mirrored_side ? " mirrored:" : " original:");
            for (std::size_t c = 0; c < rgb_channels; ++c) {
                reference_report << ' ' << std::lround(direct[c]) << '/' << std::lround(through_reference[c]);
                reference_matched = reference_matched && std::abs(direct[c] - through_reference[c]) <= face_tolerance;
            }
            reference_report << "; ";
        }
    const auto agreement = gpu_check::parity(reference, image);
    reference_report << "mean difference " << agreement.mean << ", pixels beyond 16 levels: " << agreement.large * 100
                     << "%";
    std::cout << "MIRRORED REFERENCE direct/reference " << reference_report.str() << '\n';
    images.require(reference_matched, "The CPU reference shades mirrored draws differently: " + reference_report.str(),
                   {"mirrored-shading", "mirrored-reference"});
    images.require_parity("mirrored-reference", "mirrored-shading");
}
// A box scaled to zero along Z shades as the square it collapses to, like an ordinary square facing +Z at the
// mirror position. The sun and the camera lie in the plane x = 0 between them and nothing casts shadows, so only
// their normals can differ. An upward square in view shows that a +Y normal would shade them differently.
template <class Capture>
void check_collapsed_shading(anima::VulkanRenderer &renderer, Capture &&capture, const gpu_check::Captures &images) {
    constexpr float half = .5F, offset = 1.2F;
    constexpr float quarter_turn = 1.5707964F; // Turns +Y to +Z about +X.
    constexpr anima::Quat unrotated{0, 0, 0, 1};
    constexpr int window_radius = 3;     // Pixels around each sample, which sits well inside its square.
    constexpr double face_tolerance = 3; // Channel levels between the collapsed and the ordinary square.
    constexpr double normal_margin = 20; // Channel levels by which the upward square differs from the ordinary one.
    auto scene = std::make_shared<anima::Scene>();
    auto collapsed = scene->create("collapsed", anima::Mesh::compile(*box_fixture({half, half, half})));
    collapsed.set_world_transform({{-offset, half, 0}, unrotated, {1, 1, 0}});
    const auto square = anima::Mesh::compile(*box_fixture({half, 0, half}, true)); // Faces +Y.
    auto facing = scene->create("facing", square);
    facing.set_world_transform(
        {{offset, half, 0}, {std::sin(quarter_turn / 2), 0, 0, std::cos(quarter_turn / 2)}, {1, 1, 1}});
    auto upward = scene->create("upward", square);
    upward.set_world_transform({{0, 0, 1}, unrotated, {1, 1, 1}});
    anima::Environment lighting;
    lighting.sun.direction = {0, .342F, .94F}; // 20 degrees above the horizon, behind the camera.
    lighting.sun.radiance = {2.5F, 2.5F, 2.5F};
    lighting.fill.radiance = {};
    lighting.ambient_sky = {.25F, .3F, .4F};
    lighting.ambient_ground = {.04F, .03F, .02F};
    lighting.shadow_cascades.enabled = false;
    const auto view =
        anima::perspective(std::numbers::pi_v<float> / 4, 4.F / 3, .05F, 50) * anima::look_at({0, 2, 5}, {0, half, 0});
    renderer.set_view(view);
    renderer.set_environment(lighting);
    renderer.set_scenes({scene});
    capture("collapsed-shading");
    const auto &image = images["collapsed-shading"];
    const auto sample = [&](anima::Vec3 world) {
        return window_mean(image, project(view, world, image.width, image.height), window_radius);
    };
    const auto flattened = sample({-offset, half, 0}), ordinary = sample({offset, half, 0}), up = sample({0, 0, 1});
    const std::array means{std::pair{"collapsed", flattened}, std::pair{"ordinary", ordinary}, std::pair{"upward", up}};
    std::ostringstream report;
    bool matched = true, distinct = false;
    for (const auto &[name, value] : means) {
        report << name;
        for (const auto channel : value)
            report << ' ' << std::lround(channel);
        report << "; ";
    }
    for (std::size_t c = 0; c < rgb_channels; ++c) {
        matched = matched && std::abs(flattened[c] - ordinary[c]) <= face_tolerance;
        distinct = distinct || std::abs(up[c] - ordinary[c]) > normal_margin;
    }
    std::cout << "COLLAPSED SHADING " << report.str() << '\n';
    // If +Y and +Z normals shaded alike here, a match would prove nothing.
    images.require(distinct, "The upward and the ordinary square shade alike: " + report.str(), {"collapsed-shading"});
    images.require(matched, "A collapsed draw shades unlike its flattened surface: " + report.str(),
                   {"collapsed-shading"});
}
// The pixel of ground point (x, y, z) seen from @p eye looking at the origin, through a 45-degree vertical field of
// view and a 4:3 aspect, the environment views' projection. Rounds half to even, as the removed Python check did.
inline gpu_check::Rgb ground_pixel(const gpu_check::Image &image, double x, double z,
                                   std::array<double, 3> eye = {0, 6, 10}, double y = 0) {
    const double cotangent = 1 + std::numbers::sqrt2; // 1 / tan(22.5 degrees)
    constexpr double aspect = 4. / 3;
    const auto length = std::sqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]);
    const std::array backward{eye[0] / length, eye[1] / length, eye[2] / length};
    const auto horizontal = std::hypot(eye[0], eye[2]);
    const std::array right{eye[2] / horizontal, 0., -eye[0] / horizontal};
    const std::array up{backward[1] * right[2], backward[2] * right[0] - backward[0] * right[2],
                        -backward[1] * right[0]};
    const auto x_view = right[0] * x + right[2] * z;
    const auto y_view = up[0] * x + up[1] * y + up[2] * z;
    const auto depth = length - backward[0] * x - backward[1] * y - backward[2] * z;
    const auto px = std::nearbyint((1 + cotangent * x_view / (depth * aspect)) * image.width / 2);
    const auto py = std::nearbyint((1 - cotangent * y_view / depth) * image.height / 2);
    return gpu_check::pixel(image, std::size_t(px), std::size_t(py));
}
inline int sum(const gpu_check::Rgb &c) { return c[0] + c[1] + c[2]; }
// The orthographic view of the curved fixture: 1.2 m wide and 0.9 m high, with reversed depth from 1 at the eye to 0
// at 20 m.
inline anima::Mat4 curved_view() {
    return anima::orthographic(1.2F / .9F, .9F, 0, 20) * anima::look_at({0, 0, 3}, {0, 0, 0});
}
// Light grazing the curved fixture from its side, without ambient light, and, when @p shadowed, one cascade of the
// default biases over the 20 m in view, whose texels of about 2 cm are as coarse as a game's far shadows.
inline anima::Environment curved_environment(bool shadowed) {
    anima::Environment lighting;
    lighting.sun.direction = {-4, 0, 1};
    lighting.sun.radiance = {3, 3, 3};
    lighting.fill.radiance = {};
    lighting.ambient_sky = lighting.ambient_ground = lighting.ambient_specular = {};
    lighting.shadow_cascades.enabled = shadowed;
    lighting.shadow_cascades.count = 1;
    lighting.shadow_cascades.distance = 20;
    lighting.shadow_cascades.resolution = 1024;
    return lighting;
}
// The coarse 12-sided cylinder's smooth normals face the light at these probes while the polygon beside them faces
// away. Ray casting each shadow texel's light ray through the polygon proves that ordinary self-occlusion there
// stays under the configured residual bias, so extending that nearly edge-on plane must not invent a shadow: the
// shadowed image may differ from the unshadowed one by 2 levels, rendering and quantization error.
inline void check_curved_receiver(const gpu_check::Captures &images) {
    constexpr int least_direct_light = 16, largest_error = 2;
    constexpr double cylinder_radius = .3, view_width = 1.2, view_height = .9, side_width = .15;
    const auto lighting = curved_environment(true);
    const auto cascades = anima::fit_shadow_cascades(lighting, curved_view());
    images.require(cascades.size() == 1, "The curved view fits no shadow cascade", {});
    // The cascade's texels, which the renderer lays out as fit_shadow_cascades() states, and its biases in them.
    const auto &cascade = cascades.front();
    const double texel = cascade.texel, shadow_width = 2. * cascade.radius, shadow_texels = shadow_width / texel;
    const double constant_bias = lighting.shadow_cascades.constant_bias,
                 slope_bias = lighting.shadow_cascades.slope_bias;
    images.require_same("curved-shadowed", "curved-reference-shadowed", "The curved receiver's backends differ");
    const auto &unshadowed = images["curved-unshadowed"];
    const auto &shadowed = images["curved-shadowed"];
    images.require(gpu_check::same_size(unshadowed, shadowed), "The curved captures differ in size",
                   {"curved-unshadowed", "curved-shadowed"});
    using Point = std::array<double, 2>; // X and Z.
    const auto dot = [](const Point &a, const Point &b) { return a[0] * b[0] + a[1] * b[1]; };
    std::array<Point, 12> vertices{};
    for (std::size_t i = 0; i < vertices.size(); ++i)
        vertices[i] = {cylinder_radius * std::sin(double(i) * std::numbers::pi / 6),
                       cylinder_radius * std::cos(double(i) * std::numbers::pi / 6)};
    const Point light{-4 / std::sqrt(17.), 1 / std::sqrt(17.)};
    // The cascade's axis across the sun in this plane, and where along it the square is centered.
    const Point right{light[1], -light[0]};
    const auto middle = dot({cascade.center.x, cascade.center.z}, right);
    const auto geometric_light = dot({std::sin(std::numbers::pi / 12), std::cos(std::numbers::pi / 12)}, light);
    images.require(geometric_light < 0, "The curved fixture no longer reaches the geometric light terminator", {});
    for (const double fraction : {.04, .06, .08}) {
        // Probe actual pixel centers, not the ideal projected coordinates.
        const auto px = std::nearbyint((side_width * fraction / view_width + .5) * unshadowed.width - .5);
        const auto x = ((px + .5) / unshadowed.width - .5) * view_width;
        const auto t = x / side_width;
        const Point radial{x, vertices[0][1] * (1 - t) + vertices[1][1] * t};
        const auto length = std::hypot(radial[0], radial[1]);
        const auto shaded_light = dot({radial[0] / length, radial[1] / length}, light);
        images.require(shaded_light > 0, "A curved probe is not directly lit", {});
        const auto bias = texel * (constant_bias + slope_bias * (1 - shaded_light));
        const auto first_texel = std::floor(((dot(radial, right) - middle) / shadow_width + .5) * shadow_texels - .5);
        double advance = 0;
        for (const double tap : {-1., 0., 1., 2.}) {
            const auto projected = ((first_texel + tap + .5) / shadow_texels - .5) * shadow_width + middle;
            for (std::size_t i = 0; i < vertices.size(); ++i) {
                const auto &a = vertices[i], &b = vertices[(i + 1) % vertices.size()];
                const auto ra = dot(a, right), rb = dot(b, right);
                if (std::min(ra, rb) <= projected && projected <= std::max(ra, rb)) {
                    const auto u = (projected - ra) / (rb - ra);
                    const Point hit{a[0] + (b[0] - a[0]) * u, a[1] + (b[1] - a[1]) * u};
                    advance = std::max(advance, dot(hit, light) - dot(radial, light));
                }
            }
        }
        images.require(advance < bias,
                       "A curved probe is occluded by " + std::to_string(advance) + " m, beyond the bias " +
                           std::to_string(bias),
                       {});
        for (const double y : {-.2, 0., .2}) {
            const auto py = std::nearbyint((.5 - y / view_height) * unshadowed.height - .5);
            const auto a = gpu_check::pixel(unshadowed, std::size_t(px), std::size_t(py));
            const auto b = gpu_check::pixel(shadowed, std::size_t(px), std::size_t(py));
            images.require(std::min({a[0], a[1], a[2]}) > least_direct_light,
                           "A curved probe lacks measurable direct light: " + gpu_check::text(a),
                           {"curved-unshadowed"});
            images.require(gpu_check::difference(a, b) <= largest_error,
                           "The curved receiver invented a self-shadow at " + std::to_string(px) + ", " +
                               std::to_string(py) + ": " + gpu_check::text(a) + " unshadowed, " + gpu_check::text(b) +
                               " shadowed",
                           {"curved-unshadowed", "curved-shadowed"});
        }
    }
}
// The removed Python check's comparisons of the captures that run() takes, with its thresholds.
inline void check_images(const gpu_check::Captures &images) {
    for (const auto *name : {"baseline", "restored", "rejected", "inactive"})
        images.require_same("shadowed", std::string("scene-light-") + name,
                            "Scene light selection changed accepted pixels");
    constexpr double least_change = .005;
    for (const auto &[first, second] :
         {std::pair{"baseline", "rotated"}, std::pair{"rotated", "tinted"}, std::pair{"tinted", "settings"}})
        images.require_changed(std::string("scene-light-") + first, std::string("scene-light-") + second, least_change,
                               "A scene light update had no visible effect");
    for (const auto *other :
         {"shadowed-unculled", "invalid-preserved", "reference-shadowed", "restored-shadow", "camera-restored"})
        images.require_same("shadowed", other, "The environment image changed");
    for (const auto *other : {"detail-reference-shadowed", "detail-invalid-preserved", "detail-snapped"})
        images.require_same("detail-shadowed", other, "The detail shadow image changed");
    for (const auto *other : {"detail-away", "detail-disabled"})
        images.require_same("shadowed", other, "The detail region damaged world coverage");
    images.require_same("sky", "sky-shadows-disabled", "An empty shadow pass changed the sky");
    check_curved_receiver(images);
    // An unobstructed plane facing the light must not shadow itself, even at a light cosine of about 0.044.
    constexpr int largest_slope_error = 2;
    for (const auto *fixture_name : {"slope", "steep-slope"}) {
        const std::string prefix = fixture_name;
        images.require_same(prefix + "-shadowed", prefix + "-reference-shadowed", "The receiver's backends differ");
        for (const double x : {-3., -1.5, 0., 1.5, 3.})
            for (const double z : {-3., -1.5, 0., 1.5, 3.}) {
                constexpr double slope = .35;
                const auto a = ground_pixel(images[prefix + "-unshadowed"], x, z, {0, 6, 10}, slope * x);
                const auto b = ground_pixel(images[prefix + "-shadowed"], x, z, {0, 6, 10}, slope * x);
                images.require(gpu_check::difference(a, b) <= largest_slope_error,
                               prefix + " shadows itself at " + std::to_string(x) + ", " + std::to_string(z) + ": " +
                                   gpu_check::text(a) + " unshadowed, " + gpu_check::text(b) + " shadowed",
                               {prefix + "-unshadowed", prefix + "-shadowed"});
            }
    }
    for (const auto *name : {"multi-scene-shadowed", "multi-scene-unculled", "multi-scene-rejected"})
        images.require_same(name, "shadowed", "Multi-scene shadows or selection changed the image");
    images.require_same("multi-scene-unloaded", "caster-hidden", "An unloaded caster kept rendering");
    // The 2x2 alpha mask has opaque diagonal quadrants and clear off-diagonal ones: the opaque ones darken the
    // ground below them by more than 90 levels summed over the channels, and the clear ones by at most 6, or 12
    // with the camera moved. The translated detail fixture keeps the same geometry relative to its camera, and
    // detail-boundary also samples the 0.16 m blend strip at x = -0.7.
    constexpr int least_shadow = 90, largest_clear_change = 6, largest_moved_clear_change = 12;
    const auto shadow = [&](const std::string &name, int loss, bool opaque, int clear_limit, double x, double z) {
        images.require(opaque ? loss > least_shadow : std::abs(loss) <= clear_limit,
                       name + " darkens the ground at " + std::to_string(x) + ", " + std::to_string(z) + " by " +
                           std::to_string(loss) + (opaque ? " under an opaque texel" : " under a clear texel"),
                       {"unshadowed", name});
    };
    for (const auto &[x, z, opaque] : {std::tuple{-.7, -.7, true}, std::tuple{.7, -.7, false},
                                       std::tuple{-.7, .7, false}, std::tuple{.7, .7, true}}) {
        const auto a = ground_pixel(images["unshadowed"], x, z);
        shadow("shadowed", sum(a) - sum(ground_pixel(images["shadowed"], x, z)), opaque, largest_clear_change, x, z);
        images.require(ground_pixel(images["caster-hidden"], x, z) == a, "An invisible caster kept its shadow",
                       {"unshadowed", "caster-hidden"});
        images.require(ground_pixel(images["non-casting"], x, z) == a, "A caster that casts no shadows kept its shadow",
                       {"unshadowed", "non-casting"});
        for (const auto *name :
             {"detail-shadowed", "detail-only", "detail-outside-world", "detail-boundary", "non-casting-receiver"})
            shadow(name, sum(a) - sum(ground_pixel(images[name], x, z)), opaque, largest_clear_change, x, z);
        shadow("camera-left", sum(a) - sum(ground_pixel(images["camera-left"], x, z, {-4, 6, 10})), opaque,
               largest_moved_clear_change, x, z);
    }
}
inline int run(int argc, char **argv) {
    using resource_test::require;
    require(argc == 3, "Usage: consumer --environment OUTPUT");
    const std::filesystem::path output = argv[2];
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    gpu_check::Video video;
    const auto window = gpu_check::window("Anima environment verification", 800, 600, SDL_WINDOW_RESIZABLE);
    anima::RendererOptions options;
    options.validation = true;
    options.profile = true;
    anima::VulkanRenderer renderer(window.get(), options);
    const auto asset = fixture();
    const auto compiled = anima::Mesh::compile(*asset);
    auto scene = std::make_shared<anima::Scene>();
    auto drawn = scene->create({}, compiled).renderer();
    const auto view =
        anima::perspective(std::numbers::pi_v<float> / 4, 4.F / 3, .05F, 100) * anima::look_at({0, 6, 10}, {0, 0, 0});
    renderer.set_view(view);
    renderer.set_scenes({scene});
    anima::Environment environment;
    environment.sun.direction = {0, 1, 0};
    environment.sun.radiance = {3, 2.7F, 2.3F};
    environment.fill.radiance = {0, 0, 0};
    environment.ambient_sky = {.16F, .18F, .3F};
    environment.ambient_ground = {.08F, .06F, .09F};
    environment.atmosphere.enabled = true;
    // One cascade over the 25 m around the scene, which every view below keeps in it.
    environment.shadow_cascades.count = 1;
    environment.shadow_cascades.distance = 25;
    const auto start = std::chrono::steady_clock::now();
    gpu_check::Captures images(output);
    // Draws a frame, and reads it back as @p name unless the name is empty.
    auto capture = [&](const std::string &name) {
        if (!name.empty())
            renderer.request_capture();
        for (;;) {
            require(std::chrono::steady_clock::now() - start < gpu_check::watchdog, "Environment watchdog");
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                        "Environment test interrupted");
                if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                    renderer.request_resize();
            }
            if (renderer.draw())
                break;
            SDL_Delay(5);
        }
        if (!name.empty())
            images.add(name, gpu_check::take(renderer));
    };
    renderer.set_environment(environment);
    capture("unshadowed");
    environment.shadow_cascades.enabled = true;
    renderer.set_environment(environment);
    capture("shadowed");
    require(renderer.resource_stats().shadow_draw_calls == 2, "Missing shadow casters");
    require(renderer.resource_stats().shadow_bytes > 1024, "Shadow allocation missing");
    const auto main_draws = renderer.resource_stats().draw_calls;
    // The caster stays visible but draws nothing into the shadow maps; the ground checks below compare it.
    drawn.set_casts_shadows(false);
    capture("non-casting");
    require(renderer.resource_stats().shadow_draw_calls == 0, "A renderer that casts no shadows drew into them");
    require(renderer.resource_stats().draw_calls == main_draws, "A renderer that casts no shadows stopped drawing");
    drawn.set_casts_shadows(true);
    // Resolve authored lighting through the production component graph. Geometry
    // remains independently selected above; these scenes contain only light data.
    anima::SceneSet lighting;
    auto authored = lighting.create("lighting");
    auto sun = authored->create("sun");
    sun.set_world_matrix({1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1});
    sun.add_component<anima::DirectionalLightComponent>(environment.sun.radiance);
    auto fill = authored->create("fill");
    fill.add_component<anima::DirectionalLightComponent>(environment.fill.radiance);
    auto settings_object = authored->create("environment");
    auto settings = settings_object.add_component<anima::SceneEnvironment>(
        static_cast<const anima::EnvironmentSettings &>(environment));
    settings->sun = sun;
    settings->fill = fill;
    renderer.set_environment(anima::lighting_environment(lighting));
    capture("scene-light-baseline");
    anima::ComponentCodecs light_codecs;
    anima::add_lighting_component_codecs(light_codecs);
    const auto light_document = anima::serialize_scene(authored.get(), {}, light_codecs);
    const auto sun_key = sun.key(), settings_key = settings_object.key();
    sun.set_world_matrix(anima::inverse(anima::look_at({4, 6, 1}, {0, 0, 0})));
    renderer.set_environment(anima::lighting_environment(lighting));
    capture("scene-light-rotated");
    sun.get_component<anima::DirectionalLightComponent>()->set_radiance({.2F, 1, 3});
    renderer.set_environment(anima::lighting_environment(lighting));
    capture("scene-light-tinted");
    auto changed_settings = settings->settings();
    changed_settings.exposure = .4F;
    changed_settings.fog_density = .08F;
    settings->configure(changed_settings);
    renderer.set_environment(anima::lighting_environment(lighting));
    capture("scene-light-settings");
    authored = lighting.replace(authored, light_document, {}, light_codecs);
    require(!sun.valid() && !settings.valid(), "Lighting replacement retained old component handles");
    renderer.set_environment(anima::lighting_environment(lighting));
    capture("scene-light-restored");
    settings = authored->find(settings_key).get_component<anima::SceneEnvironment>();
    settings->sun = sun;
    rejection::rejects<std::invalid_argument>([&] { renderer.set_environment(anima::lighting_environment(lighting)); },
                                              "Selected light must be a live object in the scene selection");
    capture("scene-light-rejected");
    settings->sun = authored->find(sun_key);
    settings->sun.set_active(false);
    rejection::rejects<std::invalid_argument>([&] { renderer.set_environment(anima::lighting_environment(lighting)); },
                                              "Selected light must have an active DirectionalLightComponent");
    capture("scene-light-inactive");
    // Subsequent checks use the same accepted immediate environment as before.
    renderer.set_environment(environment);
    // Put the receiver and caster into different scenes, sharing the same mesh.
    const auto uploads = renderer.resource_stats().mesh_uploads;
    anima::SceneSet layers;
    auto caster = layers.create("caster");
    caster->create({}, compiled).renderer().set_primitive_visible(0, false);
    drawn.set_primitive_visible(1, false);
    renderer.set_scenes({scene, caster.render_scene()});
    capture("multi-scene-shadowed");
    require(renderer.resource_stats().mesh_uploads == uploads && renderer.resource_stats().shadow_draw_calls == 2,
            "Cross-scene shadows lost a caster or duplicated resource uploads");
    // The ground stops casting and still receives the other scene's caster; the ground checks compare it.
    drawn.set_casts_shadows(false);
    capture("non-casting-receiver");
    require(renderer.resource_stats().shadow_draw_calls == 1, "A non-casting receiver still drew into the shadows");
    drawn.set_casts_shadows(true);
    renderer.set_frustum_culling(false);
    capture("multi-scene-unculled");
    renderer.set_frustum_culling(true);
    rejection::rejects<std::invalid_argument>([&] { renderer.set_scenes({scene, scene}); },
                                              "Renderer selection contains a duplicate scene");
    rejection::rejects<std::invalid_argument>([&] { renderer.set_scenes({scene, nullptr}); },
                                              "Renderer selection contains a null scene");
    rejection::rejects<std::runtime_error>(
        [&] { renderer.set_scenes({caster.render_scene()}, {anima::RendererFailureStage::ready}); },
        "Injected resource preparation failure after ready");
    capture("multi-scene-rejected");
    layers.unload(caster);
    capture("multi-scene-unloaded");
    require(renderer.resource_stats().shadow_draw_calls == 1, "Unloaded scene retained shadow draws");
    drawn.set_primitive_visible(1, true);
    renderer.set_scenes({scene});
    renderer.set_frustum_culling(false);
    capture("shadowed-unculled");
    renderer.set_frustum_culling(true);
    auto bad = environment;
    bad.exposure = std::numeric_limits<float>::quiet_NaN();
    rejection::rejects<std::invalid_argument>([&] { renderer.set_environment(bad); },
                                              "Invalid environment exposure or fog density");
    capture("invalid-preserved");
    renderer.set_scenes({reference_test::scene(anima::make_mesh_snapshot(*asset, anima::sample_pose(*asset)))});
    capture("reference-shadowed");
    renderer.set_scenes({scene});
    drawn.set_primitive_visible(1, false);
    capture("caster-hidden");
    drawn.set_primitive_visible(1, true);
    environment.shadow_cascades.resolution = 1024;
    renderer.set_environment(environment);
    capture("");
    environment.shadow_cascades.resolution = 2048;
    renderer.set_environment(environment);
    capture("restored-shadow");
    renderer.set_view(anima::perspective(std::numbers::pi_v<float> / 4, 4.F / 3, .05F, 2) *
                      anima::look_at({0, 1, 1}, {0, 0, 0}));
    capture("");
    require(renderer.resource_stats().culled_draws >= 1 && renderer.resource_stats().shadow_draw_calls == 2,
            "Main camera incorrectly removed an offscreen shadow caster");
    renderer.set_view(view);
    // Move the main camera continuously, refitting the cascade to each view on the world's texel grid. Returning to
    // the original view must reproduce the original shadowed image.
    for (int step = 0; step <= 16; ++step) {
        const float x = step <= 8 ? -.5F * step : -4.F + .5F * (step - 8);
        renderer.set_view(anima::perspective(std::numbers::pi_v<float> / 4, 4.F / 3, .05F, 100) *
                          anima::look_at({x, 6, 10}, {0, 0, 0}));
        capture(step == 8 ? "camera-left" : step == 16 ? "camera-restored" : "");
    }
    // Optional detail region: keep the cascades' coverage while refining a small subject, including independent
    // casters outside both the main camera and every cascade.
    environment.detail_shadow.enabled = true;
    environment.detail_shadow.extent = 2;
    environment.detail_shadow.depth = 30;
    environment.detail_shadow.resolution = 1024;
    renderer.set_environment(environment);
    capture("detail-shadowed");
    require(renderer.resource_stats().shadow_draw_calls == 4, "Detail pass lost a caster");
    renderer.set_scenes({reference_test::scene(anima::make_mesh_snapshot(*asset, anima::sample_pose(*asset)))});
    capture("detail-reference-shadowed");
    renderer.set_scenes({scene});
    bad = environment;
    bad.detail_shadow.resolution = std::numeric_limits<std::uint32_t>::max();
    rejection::rejects<std::invalid_argument>([&] { renderer.set_environment(bad); },
                                              "Shadow resolution exceeds device capabilities");
    capture("detail-invalid-preserved");
    environment.detail_shadow.center.x = .0001F;
    renderer.set_environment(environment);
    capture("detail-snapped");
    environment.detail_shadow.center.x = 1.25F;
    renderer.set_environment(environment);
    capture("detail-boundary");
    environment.detail_shadow.center.x = 5;
    renderer.set_environment(environment);
    capture("detail-away");
    environment.detail_shadow.center.x = 0;
    environment.shadow_cascades.enabled = false;
    renderer.set_environment(environment);
    capture("detail-only");
    renderer.set_view(anima::perspective(std::numbers::pi_v<float> / 4, 4.F / 3, .05F, 2) *
                      anima::look_at({0, 1, 1}, {0, 0, 0}));
    capture("");
    require(renderer.resource_stats().culled_draws >= 1 && renderer.resource_stats().shadow_draw_calls == 2,
            "Main camera removed a detail-region caster");
    auto translated = anima::identity();
    translated[12] = 40;
    drawn.set_pose(anima::sample_pose(*asset), translated);
    // Cascades that end a metre from the eye hold neither the translated caster nor its ground.
    environment.shadow_cascades.enabled = true;
    environment.shadow_cascades.distance = 1;
    environment.detail_shadow.center.x = 40;
    renderer.set_environment(environment);
    renderer.set_view(anima::perspective(std::numbers::pi_v<float> / 4, 4.F / 3, .05F, 100) *
                      anima::look_at({40, 6, 10}, {40, 0, 0}));
    capture("detail-outside-world");
    require(renderer.resource_stats().shadow_draw_calls == 2, "Detail casters depended on cascade coverage");
    environment.shadow_cascades.distance = 25;
    drawn.set_pose(anima::sample_pose(*asset), anima::identity());
    renderer.set_view(view);
    environment.detail_shadow.enabled = false;
    renderer.set_environment(environment);
    capture("detail-disabled");
    require(renderer.resource_stats().shadow_draw_calls == 2, "Disabled detail pass still submitted geometry");
    // A disabled region keeps its 1x1 map, so only enabling it holds its resolution to the device's limits.
    constexpr auto beyond_any_device = std::numeric_limits<std::uint32_t>::max(); // A 2D image edge no GPU offers.
    constexpr std::string_view device_limit = "Shadow resolution exceeds device capabilities";
    const auto placeholder_bytes = renderer.resource_stats().shadow_bytes;
    auto oversized = environment;
    oversized.detail_shadow.resolution = beyond_any_device;
    renderer.set_environment(oversized);
    capture("");
    require(renderer.resource_stats().shadow_bytes == placeholder_bytes, "A disabled region outgrew its 1x1 map");
    oversized.detail_shadow.enabled = true;
    bool limited = false;
    try {
        renderer.set_environment(oversized);
    } catch (const std::invalid_argument &error) {
        limited = error.what() == device_limit;
    }
    require(limited, "Enabling a region beyond the device's limits was not rejected");
    capture("");
    require(renderer.resource_stats().shadow_bytes == placeholder_bytes, "A rejected environment replaced a map");
    renderer.set_environment(environment);
    const auto slope = fixture(true);
    auto slope_scene = std::make_shared<anima::Scene>();
    (void)slope_scene->create({}, anima::Mesh::compile(*slope));
    renderer.set_scenes({slope_scene});
    environment.sun.direction = {1, .6F, .1F};
    environment.shadow_cascades.enabled = false;
    renderer.set_environment(environment);
    capture("slope-unshadowed");
    environment.shadow_cascades.enabled = true;
    renderer.set_environment(environment);
    capture("slope-shadowed");
    renderer.set_scenes({reference_test::scene(anima::make_mesh_snapshot(*slope, anima::sample_pose(*slope)))});
    capture("slope-reference-shadowed");
    // A geometric light cosine of about .044 still needs exact receiver-plane
    // correction. Disabling that correction near grazing must not turn an
    // unobstructed, light-facing plane into an acne pattern.
    const auto slope_sun = environment.sun.direction;
    environment.sun.direction = {1, .4F, .1F};
    environment.shadow_cascades.enabled = false;
    renderer.set_scenes({slope_scene});
    renderer.set_environment(environment);
    capture("steep-slope-unshadowed");
    environment.shadow_cascades.enabled = true;
    renderer.set_environment(environment);
    capture("steep-slope-shadowed");
    renderer.set_scenes({reference_test::scene(anima::make_mesh_snapshot(*slope, anima::sample_pose(*slope)))});
    capture("steep-slope-reference-shadowed");
    environment.sun.direction = slope_sun;
    // The cylinder's 0..30-degree side is nearly tangent to this light, while
    // its interpolated radial normals still face it. No external caster exists.
    // This exposes receiver-plane extrapolation across curved geometry that a
    // single sloped plane cannot exercise.
    const auto curved = curved_fixture();
    auto curved_scene = std::make_shared<anima::Scene>();
    (void)curved_scene->create({}, anima::Mesh::compile(*curved));
    renderer.set_scenes({curved_scene});
    renderer.set_view(curved_view());
    renderer.set_environment(curved_environment(false));
    capture("curved-unshadowed");
    renderer.set_environment(curved_environment(true));
    capture("curved-shadowed");
    renderer.set_scenes({reference_test::scene(anima::make_mesh_snapshot(*curved, anima::sample_pose(*curved)))});
    capture("curved-reference-shadowed");
    renderer.set_environment(environment);
    renderer.set_view(view);
    renderer.set_scenes({std::make_shared<anima::Scene>()});
    capture("sky");
    environment.shadow_cascades.enabled = false;
    renderer.set_environment(environment);
    capture("sky-shadows-disabled");
    check_mirrored_shading(renderer, capture, images);
    check_collapsed_shading(renderer, capture, images);
    const auto stats = renderer.shutdown();
    require(!stats.validation_errors && !stats.validation_warnings, "Environment GPU validation failed");
    check_images(images);
    std::cout << "PASS environment: scene lights, masked shadows, planar and curved receivers, detail regions, "
                 "reference parity, sky, mirrored and collapsed shading and rollback, with clean validation\n";
    return 0;
}
} // namespace environment_test
