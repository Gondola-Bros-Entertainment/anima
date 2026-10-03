#include "near.hpp"
#include <anima/assets/scene_validation.hpp>
#include <anima/environment.hpp>
#include <anima/scene.hpp>
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace anima;
namespace {
constexpr float tolerance = 1e-4F; // Matrix products, shadow depths, tangents and vertex alpha.
constexpr auto texel_range = "Shadow texel size exceeds finite range";
// A sun straight overhead and an enabled detail region of extent 1.5 and depth 20 centered at (1, 2, 3).
Environment overhead_sun() {
    Environment env;
    env.sun.direction = {0, 1, 0};
    env.detail_shadow.enabled = true;
    env.detail_shadow.extent = 1.5F;
    env.detail_shadow.depth = 20;
    env.detail_shadow.center = {1, 2, 3};
    return env;
}
// Four enabled cascades of 2048 texels to 100 m, from a sun in the sky's south-west quarter.
Environment cascaded() {
    Environment env;
    env.sun.direction = {-.5F, .72F, .38F};
    env.shadow_cascades.enabled = true;
    return env;
}
// A 16:9 perspective view from 3 cm to 480 m, looking down and across from above the origin.
Mat4 wide_view(Vec3 eye = {4, 3, 9}, Vec3 target = {0, 1, 0}, float far_plane = 480) {
    return perspective(16.F / 9, .03F, far_plane) * look_at(eye, target);
}
// The axes along which the sun's shadow projections lie, as directional shadows document them: right and up across
// the sun, and forward away from it.
std::array<Vec3, 3> sun_axes(Vec3 sun) {
    const auto forward = normalized(sun) * -1.F;
    const auto right = normalized(cross(forward, std::abs(forward.y) > .99F ? Vec3{1, 0, 0} : Vec3{0, 1, 0}));
    return {right, cross(right, forward), forward};
}
// Split @p k of @p count over view depths from @p near_depth to @p far_depth, as ShadowCascades states it.
double split(double near_depth, double far_depth, double share, int k, int count) {
    const double fraction = double(k) / count;
    return share * near_depth * std::pow(far_depth / near_depth, fraction) +
           (1 - share) * (near_depth + (far_depth - near_depth) * fraction);
}
// The corners of perspective view @p view's frustum at view depths @p from and @p to, scaled from those of its far
// plane, whose depth is @p far_depth: unprojecting the far plane, rather than the near one, keeps rounding small.
std::array<Vec3, 8> slice_corners(const Mat4 &view, float far_depth, double from, double to) {
    const auto inverted = inverse(view);
    const auto origin = view_origin(view);
    const Vec3 eye{origin[0], origin[1], origin[2]};
    std::array<Vec3, 8> corners{};
    for (int c = 0; c < 4; ++c) {
        const std::array<float, 4> clip{c & 1 ? 1.F : -1.F, c & 2 ? 1.F : -1.F, 0, 1};
        std::array<float, 4> world{};
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k)
                world[r] += inverted[k * 4 + r] * clip[k];
        const Vec3 on_far{world[0] / world[3], world[1] / world[3], world[2] / world[3]};
        corners[c] = eye + (on_far - eye) * float(from / far_depth);
        corners[c + 4] = eye + (on_far - eye) * float(to / far_depth);
    }
    return corners;
}
// A 2x1 image from transparent black to opaque white.
Texture gradient(TextureEncoding encoding) {
    return {std::make_shared<Image>(Image{2, 1, {0, 0, 0, 0, 255, 255, 255, 255}}), {}, encoding};
}
// Data maps on texture 0, which must be linear, and color maps on texture 1, which must be sRGB.
Material textured() {
    Material material;
    material.normal_texture = 0;
    material.metallic_roughness_texture = 0;
    material.occlusion_texture = 0;
    material.emissive_texture = 1;
    material.texture = 1;
    material.emissive = {.2F, .3F, .4F};
    material.alpha_mode = AlphaMode::mask;
    return material;
}
// Two triangles with textured() that differ only in tangent handedness and alpha.
std::shared_ptr<Asset> seams() {
    auto source = std::make_shared<Asset>();
    source->nodes.resize(1);
    source->textures = {gradient(TextureEncoding::linear), gradient(TextureEncoding::srgb)};
    source->materials.push_back(textured());
    SourcePrimitive primitive;
    primitive.material = 0;
    for (int i = 0; i < 6; ++i) {
        SourceVertex v;
        v.position = {float(i % 3), float(i % 3 == 1), 0};
        v.normal = {0, 0, 1};
        v.tangent = {1, 0, 0, i < 3 ? 1.F : -1.F};
        v.alpha = i < 3 ? 1.F : .25F;
        primitive.vertices.push_back(v);
    }
    source->primitives.push_back(primitive);
    return source;
}
// Negates X and scales Y, so it flips tangent handedness.
Mat4 mirror() {
    auto result = identity();
    result[0] = -2;
    result[5] = 3;
    return result;
}
} // namespace

TEST_CASE("A view projection times its inverse is the identity, and a singular matrix is rejected") {
    const auto vp = perspective(1.3F, .03F, 80) * look_at({4, 5, 7}, {1, 0, 2});
    const auto product = vp * inverse(vp);
    for (std::size_t i = 0; i < product.size(); ++i) {
        CAPTURE(i);
        CHECK(product[i] == Near{identity()[i], tolerance});
    }
    CHECK_THROWS_WITH_AS(inverse({}), math_error_message(MathErrorCode::singular_matrix), MathError);
}

TEST_CASE("The detail region maps its depth to [0, 1] and snaps its origin to texels") {
    auto env = overhead_sun();
    const auto detail = detail_shadow_matrix(env);
    CHECK(point(detail, {1, 2, 3}).z == Near{.5F, tolerance});
    CHECK(point(detail, {1, 12, 3}).z == Near{0, tolerance});
    CHECK(point(detail, {1, -8, 3}).z == Near{1, tolerance});
    env.detail_shadow.center.x += .0001F;
    CHECK(detail == detail_shadow_matrix(env));
    env.detail_shadow.center.x += .2F;
    CHECK(detail != detail_shadow_matrix(env));
}

TEST_CASE("Invalid lights, exposure, fog and shadow settings are rejected") {
    const auto env = overhead_sun();
    auto bad = env;
    bad.sun.direction = {};
    CHECK_THROWS_WITH_AS(validate_environment(bad), "Invalid directional light", std::invalid_argument);
    bad = env;
    bad.exposure = 0;
    CHECK_THROWS_WITH_AS(validate_environment(bad), "Invalid environment exposure or fog density",
                         std::invalid_argument);
    bad = env;
    bad.fog_density = -1;
    CHECK_THROWS_WITH_AS(validate_environment(bad), "Invalid environment exposure or fog density",
                         std::invalid_argument);
    bad = env;
    bad.fog_falloff = -1;
    CHECK_THROWS_WITH_AS(validate_environment(bad), "Invalid environment height fog", std::invalid_argument);
    // The phase function's asymmetry lies strictly between -1 and 1.
    bad = env;
    bad.fog_sun_anisotropy = -.99F;
    CHECK_NOTHROW(validate_environment(bad));
    bad.fog_sun_anisotropy = 1;
    CHECK_THROWS_WITH_AS(validate_environment(bad), "Invalid environment height fog", std::invalid_argument);
    bad = env;
    bad.detail_shadow.extent = std::numeric_limits<float>::max();
    CHECK_THROWS_WITH_AS(detail_shadow_matrix(bad), texel_range, std::invalid_argument);
    bad = env;
    bad.detail_shadow.extent = std::numeric_limits<float>::denorm_min();
    CHECK_THROWS_WITH_AS(detail_shadow_matrix(bad), texel_range, std::invalid_argument);
    bad = env;
    bad.detail_shadow.center.y = std::numeric_limits<float>::quiet_NaN();
    CHECK_THROWS_WITH_AS(validate_environment(bad), "Invalid directional shadow region", std::invalid_argument);
    bad = env;
    bad.shadow_cascades.count = 0;
    CHECK_THROWS_WITH_AS(validate_environment(bad), "Invalid shadow cascades", std::invalid_argument);
    // Disabled cascades are validated as enabled ones are.
    CHECK_THROWS_WITH_AS(fit_shadow_cascades(bad, wide_view()), "Invalid shadow cascades", std::invalid_argument);
}

TEST_CASE("Shadow cascades split a perspective view's depth as ShadowCascades states") {
    auto env = cascaded();
    const auto cascades = fit_shadow_cascades(env, wide_view());
    REQUIRE(cascades.size() == 4);
    for (int i = 0; i < 4; ++i) {
        CAPTURE(i);
        const auto begin = split(.03, 100, .75, i, 4), end = split(.03, 100, .75, i + 1, 4);
        CHECK(cascades[std::size_t(i)].begin == Near{begin, 1e-5 * end});
        CHECK(cascades[std::size_t(i)].end == Near{end, 1e-5 * end});
    }
    // A far plane nearer than the distance ends the shadows; even splits ignore the logarithm.
    env.shadow_cascades.logarithmic_split = 0;
    env.shadow_cascades.count = 3;
    const auto clipped = fit_shadow_cascades(env, wide_view({4, 3, 9}, {0, 1, 0}, 60));
    REQUIRE(clipped.size() == 3);
    for (int i = 0; i < 3; ++i) {
        CAPTURE(i);
        CHECK(clipped[std::size_t(i)].end == Near{.03 + (60 - .03) * (i + 1) / 3, 1e-3});
    }
    // An orthographic view splits evenly from its near plane, to its far plane or the distance.
    env.shadow_cascades.logarithmic_split = .75F;
    env.shadow_cascades.count = 4;
    const auto ortho = orthographic(4.F / 3, 2, -5, 25) * look_at({0, 0, 3}, {0, 0, 0});
    for (const auto &[distance, last] : {std::pair{20.F, 20.F}, std::pair{100.F, 30.F}}) {
        CAPTURE(distance);
        env.shadow_cascades.distance = distance;
        const auto even = fit_shadow_cascades(env, ortho);
        REQUIRE(even.size() == 4);
        for (std::size_t i = 0; i < even.size(); ++i) {
            CAPTURE(i);
            CHECK(even[i].begin == Near{last * float(i) / 4, 1e-4});
            CHECK(even[i].end == Near{last * float(i + 1) / 4, 1e-4});
        }
    }
}

TEST_CASE("Each cascade's square holds its slice of the view and the filter, on whole texels") {
    const auto env = cascaded();
    const auto view = wide_view();
    const auto cascades = fit_shadow_cascades(env, view);
    REQUIRE(cascades.size() == 4);
    const auto [right, up, forward] = sun_axes(env.sun.direction);
    for (std::size_t i = 0; i < cascades.size(); ++i) {
        CAPTURE(i);
        const auto &cascade = cascades[i];
        CHECK(cascade.texel == Near{2 * cascade.radius / 2048, 1e-9});
        // The slice reaches back to where the previous cascade starts blending into this one.
        const double from = i ? cascade.begin - .1 * (cascade.begin - cascades[i - 1].begin) : cascade.begin;
        for (const auto corner : slice_corners(view, 480, from, cascade.end)) {
            // The filter reads 2.5 texels around a sample, which must stay inside the square.
            const auto room = cascade.radius - 2.5F * cascade.texel + 1e-4F;
            CHECK(std::abs(dot(right, corner - cascade.center)) <= room);
            CHECK(std::abs(dot(up, corner - cascade.center)) <= room);
        }
        for (const auto axis : {right, up}) {
            const auto texels = double(dot(axis, cascade.center)) / cascade.texel;
            CHECK(texels == Near{std::round(texels), .01});
        }
        // The square's center maps to its middle, and the sphere's depth to [0, 1].
        const auto middle = point(cascade.view_projection, cascade.center);
        CHECK(middle.x == Near{0, 1e-4});
        CHECK(middle.y == Near{0, 1e-4});
        CHECK(point(cascade.view_projection, cascade.center - forward * cascade.radius).z >= -1e-5F);
        CHECK(point(cascade.view_projection, cascade.center + forward * cascade.radius).z <= 1 + 1e-5F);
    }
    // Farther cascades are coarser.
    for (std::size_t i = 1; i < cascades.size(); ++i)
        CHECK(cascades[i].texel > cascades[i - 1].texel);
}

TEST_CASE("Turning and moving the view keeps each cascade's size and moves it by whole texels") {
    const auto env = cascaded();
    const auto first = fit_shadow_cascades(env, wide_view());
    REQUIRE(first.size() == 4);
    const auto [right, up, forward] = sun_axes(env.sun.direction);
    (void)forward;
    for (int step = 1; step <= 40; ++step) {
        CAPTURE(step);
        const float turn = .37F * float(step), rise = .2F * std::sin(.5F * float(step));
        const Vec3 eye{4 + .731F * float(step), 3 + .1F * float(step % 7), 9 - .417F * float(step)};
        const auto moved = fit_shadow_cascades(env, wide_view(eye, eye + Vec3{std::cos(turn), rise, std::sin(turn)}));
        REQUIRE(moved.size() == first.size());
        for (std::size_t i = 0; i < moved.size(); ++i) {
            CAPTURE(i);
            CHECK(moved[i].radius == first[i].radius); // Exactly: the size does not depend on the camera's pose.
            for (const auto axis : {right, up}) {
                const auto shift = double(dot(axis, moved[i].center - first[i].center)) / first[i].texel;
                CHECK(shift == Near{std::round(shift), .02});
            }
        }
    }
    // A move of a fraction of the finest texel either keeps a cascade's square or shifts it by one whole texel, so
    // a point keeps its place within its texel, and the shadow edges that texels make stay where they are.
    const Vec3 point_in_view{1, .5F, 2};
    for (const auto fraction : {.1F, .3F, .45F}) {
        CAPTURE(fraction);
        const auto offset = (right + up * .7F) * (fraction * first[0].texel);
        const auto moved = fit_shadow_cascades(env, wide_view(Vec3{4, 3, 9} + offset, Vec3{0, 1, 0} + offset));
        for (std::size_t i = 0; i < moved.size(); ++i) {
            CAPTURE(i);
            const auto before = point(first[i].view_projection, point_in_view),
                       after = point(moved[i].view_projection, point_in_view);
            const auto u = double(after.x - before.x) * 1024, v = double(after.y - before.y) * 1024;
            CHECK(u == Near{std::round(u), .02});
            CHECK(v == Near{std::round(v), .02});
        }
    }
}

TEST_CASE("A view with no depth to shade gets no cascades, and an invalid view is rejected") {
    auto env = cascaded();
    env.shadow_cascades.distance = 2;
    CHECK(fit_shadow_cascades(env, perspective(1, 3, 50) * look_at({0, 0, 5}, {})).empty());
    CHECK_FALSE(fit_shadow_cascades(env, perspective(1, 1, 50) * look_at({0, 0, 5}, {})).empty());
    CHECK_THROWS_WITH_AS(fit_shadow_cascades(env, Mat4{}), math_error_message(MathErrorCode::singular_matrix),
                         MathError);
    auto nonfinite = wide_view();
    nonfinite[5] = std::numeric_limits<float>::infinity();
    CHECK_THROWS_WITH_AS(fit_shadow_cascades(env, nonfinite), math_error_message(MathErrorCode::nonfinite_matrix),
                         MathError);
}

TEST_CASE("Color mips average in sRGB, and data mips and alpha average linearly") {
    const auto color = texture_mips(gradient(TextureEncoding::srgb));
    CHECK(color.back().rgba[0] == 188);
    CHECK(color.back().rgba[3] == 128);
    const auto data = gradient(TextureEncoding::linear);
    CHECK(texture_mips(data).back().rgba[0] == 128);
    CHECK_THROWS_WITH_AS(base_color_mips(data), "Base-color mipmaps require sRGB encoding", std::invalid_argument);
}

TEST_CASE("A material texture must have the encoding its use requires") {
    const auto source = seams();
    auto material = textured();
    CHECK_NOTHROW(validate_material(material, source->textures));
    material.normal_texture = 1;
    CHECK_THROWS_WITH_AS(validate_material(material, source->textures),
                         "Material texture encoding does not match its usage", std::invalid_argument);
}

TEST_CASE("Compiling keeps tangent and alpha seams, and mirroring flips tangent handedness") {
    const auto source = seams();
    const auto compiled = Mesh::compile(*source);
    CHECK(compiled->vertices().size() == 6);
    const auto t = tangent(mirror(), {1, 0, 0, 1});
    CHECK(t[0] == Near{-2, tolerance});
    CHECK(t[3] == Near{-1, tolerance});
    CHECK(tangent(mirror(), {}) == std::array<float, 4>{}); // A missing tangent gains no frame.
    Scene resources;
    const auto id = resources.add(compiled);
    resources.set_pose(id, sample_pose(*source), mirror());
    const auto snapshot = resources.snapshot();
    CHECK(snapshot.vertices[3].alpha == Near{.25F, tolerance});
    CHECK(snapshot.vertices[3].tangent[3] == Near{1, tolerance});
}

TEST_CASE("Snapshots offset each instance's texture references, and compiled materials keep their own copy") {
    const auto source = seams();
    const auto compiled = Mesh::compile(*source);
    Scene resources;
    const auto id = resources.add(compiled);
    resources.set_pose(id, sample_pose(*source), mirror());
    (void)resources.add(compiled);
    const auto composed = resources.snapshot();
    CHECK(composed.material_data[1].normal_texture == 2);
    CHECK(composed.material_data[1].emissive_texture == 3);
    CHECK_NOTHROW(validate_scene(composed));
    source->materials[0].normal_scale = .5F;
    CHECK(compiled->materials()->material_data[0].normal_scale == 1);
}

TEST_CASE("A tangent handedness other than 0, 1 or -1 is rejected") {
    auto source = seams();
    source->primitives[0].vertices[0].tangent[3] = .5F;
    CHECK_THROWS_WITH_AS(Mesh::compile(*source), "Invalid render vertex", std::invalid_argument);
}

namespace {
// The transmittance of @p a from @p altitude along the direction at @p elevation degrees above the horizon, by
// uniform midpoint steps along a point that marches from the ground's center outward, independently of
// atmosphere_transmittance()'s substitution; zero where the path meets the ground.
std::array<double, 3> brute_transmittance(const Atmosphere &a, double altitude, double elevation) {
    const double angle = elevation * std::numbers::pi / 180, ground = a.planet_radius, top = ground + a.thickness;
    const double x0 = 0, y0 = ground + altitude, dx = std::cos(angle), dy = std::sin(angle);
    // Where the path leaves the top: |p0 + t d| = top.
    const double b = x0 * dx + y0 * dy, c = x0 * x0 + y0 * y0 - top * top;
    const double end = -b + std::sqrt(b * b - c);
    constexpr int steps = 100'000;
    std::array<double, 3> depth{};
    for (int i = 0; i < steps; ++i) {
        const double t = end * (i + .5) / steps, x = x0 + t * dx, y = y0 + t * dy;
        const double height = std::hypot(x, y) - ground;
        if (height < 0)
            return {0, 0, 0};
        const double rayleigh = std::exp(-height / a.rayleigh_scale_height),
                     mie = std::exp(-height / a.mie_scale_height),
                     ozone = std::max(0.0, 1 - std::abs(height - a.ozone_altitude) / (a.ozone_width / 2.0));
        const std::array<std::array<float, 3>, 4> c3{
            {{a.rayleigh_scattering.x, a.rayleigh_scattering.y, a.rayleigh_scattering.z},
             {a.mie_scattering.x, a.mie_scattering.y, a.mie_scattering.z},
             {a.mie_absorption.x, a.mie_absorption.y, a.mie_absorption.z},
             {a.ozone_absorption.x, a.ozone_absorption.y, a.ozone_absorption.z}}};
        for (std::size_t k = 0; k < 3; ++k)
            depth[k] += (c3[0][k] * rayleigh + (double(c3[1][k]) + c3[2][k]) * mie + c3[3][k] * ozone) * end / steps;
    }
    return {std::exp(-depth[0]), std::exp(-depth[1]), std::exp(-depth[2])};
}
} // namespace

TEST_CASE("An atmosphere's transmittance matches an independent integral of its media") {
    const Atmosphere earth;
    // Straight up from the ground the optical depth has a closed form: each exponential medium's coefficient times
    // its scale height times 1 - exp(-thickness / height), and the ozone tent's coefficient times half its width.
    const auto zenith = atmosphere_transmittance(earth, 0, 1);
    const auto closed = [&](float rayleigh, float mie, float ozone) {
        return std::exp(
            -(rayleigh * earth.rayleigh_scale_height *
                  (1 - std::exp(-double(earth.thickness) / earth.rayleigh_scale_height)) +
              mie * earth.mie_scale_height * (1 - std::exp(-double(earth.thickness) / earth.mie_scale_height)) +
              ozone * earth.ozone_width / 2.0));
    };
    CHECK(zenith.x == Near{closed(earth.rayleigh_scattering.x, earth.mie_scattering.x + earth.mie_absorption.x,
                                  earth.ozone_absorption.x),
                           1e-5});
    CHECK(zenith.y == Near{closed(earth.rayleigh_scattering.y, earth.mie_scattering.y + earth.mie_absorption.y,
                                  earth.ozone_absorption.y),
                           1e-5});
    CHECK(zenith.z == Near{closed(earth.rayleigh_scattering.z, earth.mie_scattering.z + earth.mie_absorption.z,
                                  earth.ozone_absorption.z),
                           1e-5});
    // Along low and grazing paths from the ground and above it, against uniform steps along a marching point.
    for (const double altitude : {0.0, 2'000.0, 20'000.0})
        for (const double elevation : {10.0, 2.0, 0.0, -.5}) {
            CAPTURE(altitude);
            CAPTURE(elevation);
            const auto expected = brute_transmittance(earth, altitude, elevation);
            const auto actual = atmosphere_transmittance(earth, altitude, std::sin(elevation * std::numbers::pi / 180));
            CHECK(actual.x == Near{expected[0], 1e-4});
            CHECK(actual.y == Near{expected[1], 1e-4});
            CHECK(actual.z == Near{expected[2], 1e-4});
        }
    // Earth's values at the zenith, 10 and 2 degrees, which two independent integrations gave to three places.
    const std::array<std::pair<double, std::array<double, 3>>, 3> rows{
        {{90, {.940, .868, .762}}, {10, {.713, .459, .222}}, {2, {.331, .085, .006}}}};
    for (const auto &[elevation, values] : rows) {
        CAPTURE(elevation);
        const auto actual = atmosphere_transmittance(earth, 0, std::sin(elevation * std::numbers::pi / 180));
        CHECK(actual.x == Near{values[0], 1e-3});
        CHECK(actual.y == Near{values[1], 1e-3});
        CHECK(actual.z == Near{values[2], 1e-3});
    }
}

TEST_CASE("An atmosphere reddens and dims the light as the path lowers, and the ground blocks it") {
    const Atmosphere earth;
    // From the ground any path below the horizon meets it; from above, a path just below the horizon passes.
    for (const double cosine : {-1e-6, -.01, -1.0})
        CHECK(atmosphere_transmittance(earth, 0, cosine).x == 0);
    CHECK(atmosphere_transmittance(earth, 2'000, std::sin(-.5 * std::numbers::pi / 180)).x > 0);
    CHECK(atmosphere_transmittance(earth, 0, 0).x > 0);
    Vec3 previous{0, 0, 0};
    double previous_ratio = std::numeric_limits<double>::infinity();
    for (int degrees = 0; degrees <= 90; degrees += 5) {
        CAPTURE(degrees);
        const auto t = atmosphere_transmittance(earth, 0, std::sin(degrees * std::numbers::pi / 180));
        CHECK(t.x >= previous.x);
        CHECK(t.y >= previous.y);
        CHECK(t.z >= previous.z);
        const double ratio = double(t.x) / t.z;
        CHECK(ratio < previous_ratio);
        previous = t;
        previous_ratio = ratio;
    }
}

TEST_CASE("Invalid atmospheres and samples are rejected") {
    constexpr auto invalid = "Invalid atmosphere", sample = "Invalid atmosphere sample";
    constexpr auto nan = std::numeric_limits<float>::quiet_NaN(), infinite = std::numeric_limits<float>::infinity();
    const std::vector<std::function<void(Atmosphere &)>> breaks{[](Atmosphere &a) { a.planet_radius = 0; },
                                                                [](Atmosphere &a) { a.planet_radius = infinite; },
                                                                [](Atmosphere &a) { a.thickness = -1; },
                                                                [](Atmosphere &a) { a.thickness = nan; },
                                                                [](Atmosphere &a) { a.planet_radius = 9.9999e8F; },
                                                                [](Atmosphere &a) { a.rayleigh_scattering.y = -1e-6F; },
                                                                [](Atmosphere &a) { a.rayleigh_scale_height = 0; },
                                                                [](Atmosphere &a) { a.mie_scattering.z = nan; },
                                                                [](Atmosphere &a) { a.mie_absorption.x = infinite; },
                                                                [](Atmosphere &a) { a.mie_scale_height = -1; },
                                                                [](Atmosphere &a) { a.ozone_absorption.x = -1e-9F; },
                                                                [](Atmosphere &a) { a.ozone_altitude = nan; },
                                                                [](Atmosphere &a) { a.ozone_width = 0; }};
    for (std::size_t index = 0; index < breaks.size(); ++index) {
        CAPTURE(index);
        Atmosphere bad;
        breaks[index](bad);
        CHECK_THROWS_WITH_AS(validate_atmosphere(bad), invalid, std::invalid_argument);
        CHECK_THROWS_WITH_AS((void)atmosphere_transmittance(bad, 0, 1), invalid, std::invalid_argument);
    }
    const Atmosphere earth;
    CHECK_NOTHROW(validate_atmosphere(earth));
    for (const auto &[altitude, cosine] : {std::pair{-1.0, 1.0}, std::pair{100'001.0, 1.0}, std::pair{double(nan), 1.0},
                                           std::pair{0.0, 1.5}, std::pair{0.0, -1.01}, std::pair{0.0, double(nan)}}) {
        CAPTURE(altitude);
        CAPTURE(cosine);
        CHECK_THROWS_WITH_AS((void)atmosphere_transmittance(earth, altitude, cosine), sample, std::invalid_argument);
    }
}
