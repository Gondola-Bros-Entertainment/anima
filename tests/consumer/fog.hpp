#pragma once
// Height fog against its documented contract (EnvironmentSettings::fog_density): unlit quads at known heights and
// distances, whose expected colors integrate the documented density numerically along each pixel's ray rather than
// repeating the shaders' closed form; sunlight scattered forward and backward through the Henyey-Greenstein phase
// function; a blended quad composited over a fogged one as each is fogged at its own distance; uniform fog, with and
// without sunlight, whose height leaves the same frame; the sky fogged at fog_sky_distance, so that a quad there meets
// it without a seam that the unfogged sky shows; and animaFogged(), which fogs a custom material as the standard
// material is fogged.
#include "blending.hpp"
#include "custom_materials.hpp"
#include <anima/environment.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

namespace fog_test {
using blending_test::Color;
using blending_test::require;
using Point = std::array<double, 3>;

constexpr anima::Vec3 eye{0, 2, 0};
inline anima::Mat4 view(float aspect) {
    using anima::operator*;
    return anima::perspective(aspect, .1F, 1000) * anima::look_at(eye, eye + anima::Vec3{0, 0, -1});
}
// The unit direction from the eye through the center of @p pixel of @p image, drawn through the view-projection whose
// inverse is @p inverse.
inline Point ray(const anima::Mat4 &inverse, blending_test::Pixel pixel, const gpu_check::Image &image) {
    const auto ndc_x = (double(pixel[0]) + .5) / image.width * 2 - 1,
               ndc_y = (double(pixel[1]) + .5) / image.height * 2 - 1;
    std::array<double, 4> far{};
    for (unsigned row = 0; row < 4; ++row)
        far[row] = double(inverse[row]) * ndc_x + double(inverse[4 + row]) * ndc_y + double(inverse[12 + row]);
    Point direction{far[0] / far[3] - eye.x, far[1] / far[3] - eye.y, far[2] / far[3] - eye.z};
    const auto length = std::hypot(direction[0], direction[1], direction[2]);
    for (auto &axis : direction)
        axis /= length;
    return direction;
}
// The share of light that crosses the fog from @p to to the eye, integrating the documented density numerically.
inline double transmittance(const anima::EnvironmentSettings &e, const Point &to) {
    constexpr int steps = 4096;
    const Point path{to[0] - eye.x, to[1] - eye.y, to[2] - eye.z};
    double depth = 0;
    for (int i = 0; i < steps; ++i) {
        const auto height = eye.y + path[1] * (i + .5) / steps;
        depth += double(e.fog_density) * std::exp(-double(e.fog_falloff) * (height - double(e.fog_height)));
    }
    return std::exp(-depth * std::hypot(path[0], path[1], path[2]) / steps);
}
// @p color at @p at, as the fog's documented transmittance and light show it from the eye.
inline Color fogged(const anima::Environment &e, const Point &at, const Color &color) {
    const Point path{at[0] - eye.x, at[1] - eye.y, at[2] - eye.z};
    const auto sun = anima::normalized(e.sun.direction);
    const auto cosine = (path[0] * sun.x + path[1] * sun.y + path[2] * sun.z) / std::hypot(path[0], path[1], path[2]);
    const double g = e.fog_sun_anisotropy;
    const auto phase = (1 - g * g) / (4 * std::numbers::pi * std::pow(1 + g * g - 2 * g * cosine, 1.5));
    const std::array scattering{double(e.fog_sun_scattering.x) * e.sun.radiance.x,
                                double(e.fog_sun_scattering.y) * e.sun.radiance.y,
                                double(e.fog_sun_scattering.z) * e.sun.radiance.z};
    const Color fog{e.fog_color.x, e.fog_color.y, e.fog_color.z};
    const auto kept = transmittance(e, at);
    Color result{};
    for (std::size_t c = 0; c < result.size(); ++c) {
        result[c] = kept * color[c] + (1 - kept) * (fog[c] + scattering[c] * phase);
        require(result[c] < 1, "The fog check's expected color reaches display white, which would hide errors");
    }
    return result;
}
// The point on the plane z = @p plane that the pixel showing @p world sees.
inline Point seen(const anima::Mat4 &view_projection, const gpu_check::Image &image, anima::Vec3 world, double plane) {
    const auto direction =
        ray(anima::inverse(view_projection), blending_test::pixel_of(view_projection, world, image), image);
    const auto t = (plane - eye.z) / direction[2];
    return {eye.x + direction[0] * t, eye.y + direction[1] * t, eye.z + direction[2] * t};
}

// Unlit quads facing the eye at heights below, at and above the fog's base, from 5 m to 20 m away.
struct Plate {
    anima::Vec3 center;
    Color color;
};
inline const std::vector<Plate> &plates() {
    static const std::vector<Plate> value{{{-1.5F, .2F, -5}, {.6, .1, .1}}, {{0, 3.5F, -9}, {.1, .6, .2}},
                                          {{1.5F, -.5F, -7}, {.2, .3, .7}}, {{-2.5F, 1, -14}, {.7, .6, .1}},
                                          {{2.5F, 6, -20}, {.1, .1, .1}},   {{0, -2, -12}, {.5, .5, .5}}};
    return value;
}
inline std::shared_ptr<anima::Scene> plate_scene() {
    auto scene = std::make_shared<anima::Scene>();
    for (const auto &plate : plates())
        (void)scene->add(blending_test::facing(blending_test::opaque(plate.color), plate.center, .4F, .4F));
    return scene;
}
// Fog that thickens toward the ground below a base height above it.
inline anima::Environment height_fog() {
    anima::Environment environment;
    environment.fog_color = {.3F, .35F, .42F};
    environment.fog_density = .06F;
    environment.fog_height = 1.5F;
    environment.fog_falloff = .35F;
    return environment;
}
// Requires each plate's center pixel in capture @p name to show its color fogged by @p environment.
inline void expect_plates(blending_test::Harness &harness, const std::string &name, const anima::Mat4 &view_projection,
                          const anima::Environment &environment, const std::string &what) {
    for (const auto &plate : plates()) {
        const auto at = seen(view_projection, harness.images[name], plate.center, plate.center.z);
        harness.expect(name, view_projection, plate.center, fogged(environment, at, plate.color),
                       what + " at height " + std::to_string(plate.center.y) + ", " + std::to_string(-plate.center.z) +
                           " m ahead");
    }
}

// A blended quad 10 m ahead over a corner of the plate 12 m ahead at height -2, away from that plate's center, where
// the fog is three times as dense as at its base height, so a pipeline that left out the height fog would miss it.
constexpr Color glaze{.8, .5, .2};
constexpr float glaze_alpha = .5F;
inline anima::Vec3 glaze_target() { return plates()[5].center + anima::Vec3{.2F, .2F, 0}; }
inline anima::Vec3 glaze_center() { return eye + (glaze_target() - eye) * (10.F / 12); }

// Density that falls with height, with sunlight scattered toward the eye forward and backward, and without; the
// blended quad shows the "over" of its color and the plate's, each fogged at its own distance and height.
inline void check_height_fog(blending_test::Harness &harness) {
    const auto view_projection = view(harness.aspect());
    auto scene = plate_scene();
    (void)scene->add(blending_test::facing(blending_test::blended(glaze, glaze_alpha), glaze_center(), .08F, .08F));
    const auto expect_all = [&](const std::string &name, const anima::Environment &environment,
                                const std::string &what) {
        expect_plates(harness, name, view_projection, environment, what);
        const auto &image = harness.images[name];
        const auto front = glaze_center();
        harness.expect(name, view_projection, front,
                       blending_test::over(glaze_alpha,
                                           fogged(environment, seen(view_projection, image, front, front.z), glaze),
                                           fogged(environment, seen(view_projection, image, front, glaze_target().z),
                                                  plates()[5].color)),
                       "A blended quad over " + what);
    };
    auto environment = height_fog();
    harness.render("height", {scene}, view_projection, environment);
    expect_all("height", environment, "a quad in height fog");
    // A sun ahead of the eye: forward scattering brightens the fog most along the rays nearest it, and backward
    // scattering least.
    environment.sun = {{.2F, .3F, -1}, {1.2F, 1.1F, 1}};
    environment.fog_sun_scattering = {.3F, .25F, .2F};
    environment.fog_sun_anisotropy = .7F;
    harness.render("sunlit", {scene}, view_projection, environment);
    expect_all("sunlit", environment, "a quad in height fog lit by the sun");
    environment.fog_sun_anisotropy = -.5F;
    harness.render("sunlit-backward", {scene}, view_projection, environment);
    expect_all("sunlit-backward", environment, "a quad in height fog that scatters sunlight backward");
    harness.images.discard({"height", "sunlit", "sunlit-backward"});
}

// Uniform fog keeps exp(-density * distance), as before height fog: without sunlight through pipelines compiled
// without the height fog's code, and with sunlight through those compiled with it, where moving its height changes
// nothing.
inline void check_uniform(blending_test::Harness &harness) {
    const auto view_projection = view(harness.aspect());
    const auto scene = plate_scene();
    auto environment = height_fog();
    environment.fog_falloff = 0;
    harness.render("uniform", {scene}, view_projection, environment);
    expect_plates(harness, "uniform", view_projection, environment, "A quad in uniform fog");
    environment.sun = {{.2F, .3F, -1}, {1.2F, 1.1F, 1}};
    environment.fog_sun_scattering = {.3F, .25F, .2F};
    harness.render("uniform-sunlit", {scene}, view_projection, environment);
    expect_plates(harness, "uniform-sunlit", view_projection, environment, "A quad in uniform fog lit by the sun");
    environment.fog_height = 7.5F;
    harness.render("uniform-sunlit-moved", {scene}, view_projection, environment);
    harness.images.require(gpu_check::same(harness.images["uniform-sunlit"], harness.images["uniform-sunlit-moved"]),
                           "Moving the height of uniform fog lit by the sun changed the frame",
                           {"uniform-sunlit", "uniform-sunlit-moved"});
    harness.images.discard({"uniform", "uniform-sunlit", "uniform-sunlit-moved"});
}

// A sky of one color, fogged at fog_sky_distance, meets a quad of the same color there; unfogged, it shows a seam.
inline void check_sky(blending_test::Harness &harness) {
    const auto view_projection = view(harness.aspect());
    constexpr float distance = 250;
    const Color sky{.25, .4, .6};
    anima::Environment environment;
    environment.sky = true;
    environment.sky_zenith = environment.sky_horizon = environment.sky_ground = {.25F, .4F, .6F};
    // Behind the eye, so that no sun disc or glow shows.
    environment.sun = {{0, .4F, 1}, {1, 1, 1}};
    environment.fog_color = {.6F, .6F, .6F};
    environment.fog_density = .004F;
    environment.fog_falloff = .02F;
    environment.fog_sky_distance = distance;
    auto scene = std::make_shared<anima::Scene>();
    (void)scene->add(blending_test::facing(blending_test::opaque(sky), {0, eye.y, -distance}, 6, 6));
    const anima::Vec3 on_quad{4, eye.y, -distance}, on_sky{8, eye.y, -distance};
    harness.render("horizon", {scene}, view_projection, environment);
    const auto &image = harness.images["horizon"];
    const auto inverse = anima::inverse(view_projection);
    const auto beyond = ray(inverse, blending_test::pixel_of(view_projection, on_sky, image), image);
    const Point sky_point{eye.x + beyond[0] * distance, eye.y + beyond[1] * distance, eye.z + beyond[2] * distance};
    harness.expect("horizon", view_projection, on_quad,
                   fogged(environment, seen(view_projection, image, on_quad, -distance), sky),
                   "A quad at the sky's fog distance");
    harness.expect("horizon", view_projection, on_sky, fogged(environment, sky_point, sky), "The fogged sky beside it");
    const auto quad_pixel = blending_test::pixel_of(view_projection, on_quad, image);
    const auto sky_pixel = blending_test::pixel_of(view_projection, on_sky, image);
    const auto quad_color = gpu_check::pixel(image, quad_pixel[0], quad_pixel[1]);
    const auto sky_color = gpu_check::pixel(image, sky_pixel[0], sky_pixel[1]);
    for (std::size_t c = 0; c < quad_color.size(); ++c)
        harness.images.require(std::abs(quad_color[c] - sky_color[c]) <= 1,
                               "A quad at the sky's fog distance differs from the sky beside it by " +
                                   gpu_check::text(quad_color) + " against " + gpu_check::text(sky_color),
                               {"horizon"});
    // The control: the unfogged sky keeps its own color, which the fogged quad does not match.
    environment.fog_sky_distance = 0;
    harness.render("horizon-unfogged", {scene}, view_projection, environment);
    harness.expect("horizon-unfogged", view_projection, on_sky, sky, "The unfogged sky");
    const auto &unfogged = harness.images["horizon-unfogged"];
    const auto seam = gpu_check::pixel(unfogged, sky_pixel[0], sky_pixel[1])[0] -
                      gpu_check::pixel(unfogged, quad_pixel[0], quad_pixel[1])[0];
    harness.images.require(std::abs(seam) > 20, "Without sky fog the quad still matched the sky, so the check is blind",
                           {"horizon-unfogged"});
    harness.images.discard({"horizon", "horizon-unfogged"});
}

// A custom quad that writes animaFogged() of a color beside a standard unlit quad of that color, mirrored across the
// view's axis below a sun in its vertical plane, so both lie as far from the eye, as high and at the same angle to
// the sun: both show the documented fog.
inline void check_custom(blending_test::Harness &harness) {
    const auto view_projection = view(harness.aspect());
    auto environment = height_fog();
    environment.sun = {{0, .3F, -1}, {1.2F, 1.1F, 1}};
    environment.fog_sun_scattering = {.3F, .25F, .2F};
    const Color color{.6, .4, .2};
    const anima::Vec3 standard{-1.2F, .8F, -8}, custom{1.2F, .8F, -8};
    auto scene = std::make_shared<anima::Scene>();
    (void)scene->add(blending_test::facing(blending_test::opaque(color), standard, .4F, .4F));
    anima::CustomMaterialDefinition definition;
    definition.name = "fogged probe";
    definition.vertex_shader = custom_material_test::words(custom_material_test::surface_vertex);
    definition.fragment_shader = custom_material_test::words(custom_material_test::probe_fragment);
    definition.parameters = custom_material_test::parameters({{0, 0, 0, 0}});
    constexpr std::uint32_t fogged_probe = 14; // custom_probe.frag writes animaFogged() of this color.
    std::memcpy(definition.parameters.data(), &fogged_probe, sizeof(fogged_probe));
    (void)custom_material_test::add(*scene, custom_material_test::surface(custom, .4F, .4F),
                                    std::make_shared<const anima::CustomMaterial>(std::move(definition)));
    harness.render("custom", {scene}, view_projection, environment);
    const auto &image = harness.images["custom"];
    for (const auto &[center, what] :
         {std::pair{standard, "A standard unlit quad"}, std::pair{custom, "A custom quad through animaFogged()"}})
        harness.expect("custom", view_projection, center,
                       fogged(environment, seen(view_projection, image, center, center.z), color),
                       std::string(what) + " in height fog lit by the sun");
    harness.images.discard({"custom"});
}

inline int run(int argc, char **argv) {
    require(argc == 3, "Usage: consumer --fog OUTPUT");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    blending_test::Harness harness(argv[2]);
    check_height_fog(harness);
    check_uniform(harness);
    check_sky(harness);
    check_custom(harness);
    harness.finish();
    std::cout << "PASS fog: height fog and sunlight scattered forward and backward match the documented density "
                 "integrated along each ray, for opaque and blended quads and through animaFogged(), uniform fog lit "
                 "by the sun ignores its height, and the sky fogged at its distance meets a quad there without the "
                 "seam that the unfogged sky shows\n";
    return 0;
}
} // namespace fog_test
