#pragma once
// Height fog against its documented contract (anima::HeightFog): unlit quads at known heights and distances, whose
// expected colors integrate the documented density numerically along each pixel's ray rather than repeating the
// shaders' closed form; sunlight scattered forward and backward through the Henyey-Greenstein phase function; a blended
// quad composited over a fogged one as each is fogged at its own distance; uniform fog, with and without sunlight,
// whose height leaves the same frame; the sky fogged at HeightFog::sky_distance, so that a quad there meets it without
// a seam that the unfogged sky shows, and the atmosphere's sky dimmed there by the fog; the background of an
// environment without an atmosphere, exposed and tone mapped as set, and fogged at the sky distance as the sky is;
// animaFogged(), which fogs a custom material as the standard material is fogged; and the cap on the fog's light,
// which a fog color above the largest half float meets on every path without sunlight.
#include "blending.hpp"
#include "custom_materials.hpp"
#include <algorithm>
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
    return anima::perspective(std::numbers::pi_v<float> / 4, aspect, .1F, 1000) *
           anima::look_at(eye, eye + anima::Vec3{0, 0, -1});
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
        depth += double(e.fog.density) * std::exp(-double(e.fog.falloff) * (height - double(e.fog.height)));
    }
    return std::exp(-depth * std::hypot(path[0], path[1], path[2]) / steps);
}
// The largest half float, at which the fog's light is capped in each channel.
constexpr double maximum_half_float = 65504;
// @p color at @p at, as the fog's documented transmittance and light, capped at the largest half float, show it from
// the eye, times the exposure.
inline Color fogged(const anima::Environment &e, const Point &at, const Color &color) {
    const Point path{at[0] - eye.x, at[1] - eye.y, at[2] - eye.z};
    const auto sun = anima::normalized(e.sun.direction);
    const auto cosine = (path[0] * sun.x + path[1] * sun.y + path[2] * sun.z) / std::hypot(path[0], path[1], path[2]);
    const double g = e.fog.sun_anisotropy;
    const auto phase = (1 - g * g) / (4 * std::numbers::pi * std::pow(1 + g * g - 2 * g * cosine, 1.5));
    const auto sunlight = anima::atmosphere_sunlight(e);
    const std::array scattering{double(e.fog.sun_scattering.x) * sunlight.x,
                                double(e.fog.sun_scattering.y) * sunlight.y,
                                double(e.fog.sun_scattering.z) * sunlight.z};
    const Color fog{e.fog.color.x, e.fog.color.y, e.fog.color.z};
    const auto kept = transmittance(e, at);
    Color result{};
    for (std::size_t c = 0; c < result.size(); ++c) {
        const auto light = std::min(fog[c] + scattering[c] * phase, maximum_half_float);
        result[c] = double(e.exposure) * (kept * color[c] + (1 - kept) * light);
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
        (void)scene->create({}, blending_test::facing(blending_test::opaque(plate.color), plate.center, .4F, .4F));
    return scene;
}
// Fog that thickens toward the ground below a base height above it.
inline anima::Environment height_fog() {
    anima::Environment environment;
    environment.fog.color = {.3F, .35F, .42F};
    environment.fog.density = .06F;
    environment.fog.height = 1.5F;
    environment.fog.falloff = .35F;
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
    (void)scene->create({},
                        blending_test::facing(blending_test::blended(glaze, glaze_alpha), glaze_center(), .08F, .08F));
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
    environment.fog.sun_scattering = {.3F, .25F, .2F};
    environment.fog.sun_anisotropy = .7F;
    harness.render("sunlit", {scene}, view_projection, environment);
    expect_all("sunlit", environment, "a quad in height fog lit by the sun");
    environment.fog.sun_anisotropy = -.5F;
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
    environment.fog.falloff = 0;
    harness.render("uniform", {scene}, view_projection, environment);
    expect_plates(harness, "uniform", view_projection, environment, "A quad in uniform fog");
    environment.sun = {{.2F, .3F, -1}, {1.2F, 1.1F, 1}};
    environment.fog.sun_scattering = {.3F, .25F, .2F};
    harness.render("uniform-sunlit", {scene}, view_projection, environment);
    expect_plates(harness, "uniform-sunlit", view_projection, environment, "A quad in uniform fog lit by the sun");
    environment.fog.height = 7.5F;
    harness.render("uniform-sunlit-moved", {scene}, view_projection, environment);
    harness.images.require(gpu_check::same(harness.images["uniform-sunlit"], harness.images["uniform-sunlit-moved"]),
                           "Moving the height of uniform fog lit by the sun changed the frame",
                           {"uniform-sunlit", "uniform-sunlit-moved"});
    harness.images.discard({"uniform", "uniform-sunlit", "uniform-sunlit-moved"});
}

// The linear value that display level @p level encodes; the inverse of blending_test::encoded().
inline double decoded(double level) {
    const auto value = level / 255;
    return value <= .04045 ? value / 12.92 : std::pow((value + .055) / 1.055, 2.4);
}
// A black sky, from an atmosphere that neither scatters nor absorbs over a black ground, fogged at its sky distance,
// meets a black quad there; unfogged, it shows a seam. A sky that scatters keeps its own color dimmed by the fog's
// transmittance there: its fogged pixel is its unfogged pixel fogged as a quad at that distance would be.
inline void check_sky(blending_test::Harness &harness) {
    const auto view_projection = view(harness.aspect());
    constexpr float distance = 250;
    anima::Environment environment;
    auto &atmosphere = environment.atmosphere;
    atmosphere.enabled = true;
    atmosphere.rayleigh_scattering = atmosphere.mie_scattering = atmosphere.mie_absorption =
        atmosphere.ozone_absorption = atmosphere.ground_albedo = {0, 0, 0};
    // Behind the eye, so that no sun disc shows.
    environment.sun = {{0, .4F, 1}, {1, 1, 1}};
    environment.fog.color = {.6F, .6F, .6F};
    environment.fog.density = .004F;
    environment.fog.falloff = .02F;
    environment.fog.sky_distance = distance;
    auto scene = std::make_shared<anima::Scene>();
    (void)scene->create(
        {}, blending_test::facing(blending_test::opaque(blending_test::black), {0, eye.y, -distance}, 6, 6));
    const anima::Vec3 on_quad{4, eye.y, -distance}, on_sky{8, eye.y, -distance};
    harness.render("horizon", {scene}, view_projection, environment);
    const auto &image = harness.images["horizon"];
    const auto inverse = anima::inverse(view_projection);
    const auto beyond = ray(inverse, blending_test::pixel_of(view_projection, on_sky, image), image);
    const Point sky_point{eye.x + beyond[0] * distance, eye.y + beyond[1] * distance, eye.z + beyond[2] * distance};
    harness.expect("horizon", view_projection, on_quad,
                   fogged(environment, seen(view_projection, image, on_quad, -distance), blending_test::black),
                   "A black quad at the sky's fog distance");
    harness.expect("horizon", view_projection, on_sky, fogged(environment, sky_point, blending_test::black),
                   "The fogged black sky beside it");
    const auto quad_pixel = blending_test::pixel_of(view_projection, on_quad, image);
    const auto sky_pixel = blending_test::pixel_of(view_projection, on_sky, image);
    const auto quad_color = gpu_check::pixel(image, quad_pixel[0], quad_pixel[1]);
    const auto sky_color = gpu_check::pixel(image, sky_pixel[0], sky_pixel[1]);
    for (std::size_t c = 0; c < quad_color.size(); ++c)
        harness.images.require(std::abs(quad_color[c] - sky_color[c]) <= 1,
                               "A quad at the sky's fog distance differs from the sky beside it by " +
                                   gpu_check::text(quad_color) + " against " + gpu_check::text(sky_color),
                               {"horizon"});
    // The control: the unfogged sky stays black, which the fogged quad does not match.
    environment.fog.sky_distance = 0;
    harness.render("horizon-unfogged", {scene}, view_projection, environment);
    harness.expect("horizon-unfogged", view_projection, on_sky, blending_test::black, "The unfogged black sky");
    const auto &unfogged = harness.images["horizon-unfogged"];
    const auto seam = gpu_check::pixel(unfogged, sky_pixel[0], sky_pixel[1])[0] -
                      gpu_check::pixel(unfogged, quad_pixel[0], quad_pixel[1])[0];
    harness.images.require(std::abs(seam) > 20, "Without sky fog the quad still matched the sky, so the check is blind",
                           {"horizon-unfogged"});
    // The default atmosphere's sky, lit brightly, under an orange fog far from its pale horizon, so that the share of
    // its own color the fog keeps shows plainly.
    environment.atmosphere = {};
    environment.atmosphere.enabled = true;
    environment.sun.irradiance = {6, 6, 6};
    environment.fog.color = {.5F, .2F, .05F};
    harness.render("sky-unfogged", {}, view_projection, environment);
    const auto shown = gpu_check::pixel(harness.images["sky-unfogged"], sky_pixel[0], sky_pixel[1]);
    const Color sky{decoded(shown[0]), decoded(shown[1]), decoded(shown[2])};
    harness.images.require(sky[2] - environment.fog.color.z > .2,
                           "The default atmosphere's sky is too near the fog's color to show its fog: " +
                               gpu_check::text(shown),
                           {"sky-unfogged"});
    environment.fog.sky_distance = distance;
    harness.render("sky-fogged", {}, view_projection, environment);
    harness.expect("sky-fogged", view_projection, on_sky, fogged(environment, sky_point, sky),
                   "The default atmosphere's sky fogged at its sky distance");
    harness.images.discard({"horizon", "horizon-unfogged", "sky-unfogged", "sky-fogged"});
}

// With the atmosphere disabled, the background shows its color times the exposure wherever no mesh draws, and Reinhard
// tone mapping compresses it as it does the scene. Fog leaves it as set until the fog reaches the sky, at its sky
// distance, where the background fades as a quad of its color there does, so the two meet without a seam; an
// orthographic view, which is not fogged, still shows it as set.
inline void check_background(blending_test::Harness &harness) {
    const auto view_projection = view(harness.aspect());
    constexpr float distance = 250;
    anima::Environment environment;
    environment.background = {.4F, .1F, .02F};
    environment.exposure = 1.25F;
    const Color background{environment.background.x, environment.background.y, environment.background.z};
    // The environment's background as the display shows it, exposed and, with Reinhard tone mapping, compressed.
    const auto exposed = [&] {
        const auto &set = environment.background;
        const Color color{set.x, set.y, set.z};
        Color result{};
        for (std::size_t c = 0; c < result.size(); ++c) {
            const auto value = color[c] * environment.exposure;
            result[c] = environment.tone_mapping == anima::ToneMapping::reinhard ? value / (1 + value) : value;
        }
        return result;
    };
    auto scene = std::make_shared<anima::Scene>();
    (void)scene->create({}, blending_test::facing(blending_test::opaque(background), {0, eye.y, -distance}, 6, 6));
    const anima::Vec3 on_quad{4, eye.y, -distance}, on_background{8, eye.y, -distance};
    harness.render("background", {scene}, view_projection, environment);
    harness.expect("background", view_projection, on_background, exposed(), "The background without an atmosphere");
    // A background bright enough that tone mapping changes it plainly.
    environment.background = {4, 1, .2F};
    environment.tone_mapping = anima::ToneMapping::reinhard;
    harness.render("background-tone-mapped", {scene}, view_projection, environment);
    harness.expect("background-tone-mapped", view_projection, on_background, exposed(),
                   "The background through Reinhard tone mapping");
    environment.background = {.4F, .1F, .02F};
    environment.tone_mapping = anima::ToneMapping::none;
    // A fog far from the background's color in red, so that fogging it shows plainly.
    environment.fog.color = {.1F, .6F, .6F};
    environment.fog.density = .004F;
    environment.fog.falloff = .02F;
    harness.render("background-unfogged", {scene}, view_projection, environment);
    harness.expect("background-unfogged", view_projection, on_background, exposed(),
                   "The background in fog without a sky distance");
    const auto &unfogged = harness.images["background-unfogged"];
    const auto quad_pixel = blending_test::pixel_of(view_projection, on_quad, unfogged);
    const auto background_pixel = blending_test::pixel_of(view_projection, on_background, unfogged);
    // The control: the fogged quad differs from the unfogged background, which the check would otherwise not see.
    const auto seam = gpu_check::pixel(unfogged, background_pixel[0], background_pixel[1])[0] -
                      gpu_check::pixel(unfogged, quad_pixel[0], quad_pixel[1])[0];
    harness.images.require(std::abs(seam) > 20,
                           "Without a sky distance the fogged quad still matched the background, so the check is blind",
                           {"background-unfogged"});
    environment.fog.sky_distance = distance;
    harness.render("background-fogged", {scene}, view_projection, environment);
    const auto &image = harness.images["background-fogged"];
    const auto inverse = anima::inverse(view_projection);
    const auto beyond = ray(inverse, background_pixel, image);
    const Point background_point{eye.x + beyond[0] * distance, eye.y + beyond[1] * distance,
                                 eye.z + beyond[2] * distance};
    harness.expect("background-fogged", view_projection, on_quad,
                   fogged(environment, seen(view_projection, image, on_quad, -distance), background),
                   "A quad of the background's color at the sky's fog distance");
    harness.expect("background-fogged", view_projection, on_background,
                   fogged(environment, background_point, background), "The fogged background beside it");
    const auto quad_color = gpu_check::pixel(image, quad_pixel[0], quad_pixel[1]);
    const auto background_color = gpu_check::pixel(image, background_pixel[0], background_pixel[1]);
    for (std::size_t c = 0; c < quad_color.size(); ++c)
        harness.images.require(std::abs(quad_color[c] - background_color[c]) <= 1,
                               "A quad at the sky's fog distance differs from the background beside it by " +
                                   gpu_check::text(quad_color) + " against " + gpu_check::text(background_color),
                               {"background-fogged"});
    // Orthographic views are not fogged, so the background keeps its color in the same fog.
    const auto orthographic =
        anima::orthographic(harness.aspect(), 20, .1F, 1000) * anima::look_at(eye, eye + anima::Vec3{0, 0, -1});
    harness.render("background-orthographic", {scene}, orthographic, environment);
    harness.expect("background-orthographic", orthographic, on_background, exposed(),
                   "The background of an orthographic view in fog");
    harness.images.discard({"background", "background-tone-mapped", "background-unfogged", "background-fogged",
                            "background-orthographic"});
}

// The color that fogged_probe() writes animaFogged() of.
constexpr Color probe_color{.6, .4, .2};
// A custom material whose fragment shader, custom_probe.frag, writes animaFogged() of probe_color.
inline std::shared_ptr<const anima::CustomMaterial> fogged_probe() {
    anima::CustomMaterialDefinition definition;
    definition.name = "fogged probe";
    definition.vertex_shader = custom_material_test::words(custom_material_test::surface_vertex);
    definition.fragment_shader = custom_material_test::words(custom_material_test::probe_fragment);
    definition.parameters = custom_material_test::parameters({{0, 0, 0, 0}});
    constexpr std::uint32_t probe = 14; // The probe that custom_probe.frag fogs probe_color in.
    std::memcpy(definition.parameters.data(), &probe, sizeof(probe));
    return std::make_shared<const anima::CustomMaterial>(std::move(definition));
}

// A custom quad that writes animaFogged() of a color beside a standard unlit quad of that color, mirrored across the
// view's axis below a sun in its vertical plane, so both lie as far from the eye, as high and at the same angle to
// the sun: both show the documented fog.
inline void check_custom(blending_test::Harness &harness) {
    const auto view_projection = view(harness.aspect());
    auto environment = height_fog();
    environment.sun = {{0, .3F, -1}, {1.2F, 1.1F, 1}};
    environment.fog.sun_scattering = {.3F, .25F, .2F};
    const anima::Vec3 standard{-1.2F, .8F, -8}, custom{1.2F, .8F, -8};
    auto scene = std::make_shared<anima::Scene>();
    (void)scene->create({}, blending_test::facing(blending_test::opaque(probe_color), standard, .4F, .4F));
    (void)custom_material_test::add(*scene, custom_material_test::surface(custom, .4F, .4F), fogged_probe());
    harness.render("custom", {scene}, view_projection, environment);
    const auto &image = harness.images["custom"];
    for (const auto &[center, what] :
         {std::pair{standard, "A standard unlit quad"}, std::pair{custom, "A custom quad through animaFogged()"}})
        harness.expect("custom", view_projection, center,
                       fogged(environment, seen(view_projection, image, center, center.z), probe_color),
                       std::string(what) + " in height fog lit by the sun");
    harness.images.discard({"custom"});
}

// Fog without sunlight whose color lies far above the largest half float in red and green and below it in blue draws
// the frame that its color capped at that float in each channel draws: in uniform fog, through the pipelines compiled
// without the height fog's code, and in height fog, on the plates, a custom quad through animaFogged() and a black sky
// fogged at its sky distance. An exposure of 1 / 65504 shows the cap as display white, so each pixel shows the
// documented fog short of white, where the uncapped color would show white.
inline void check_capped_light(blending_test::Harness &harness) {
    const auto view_projection = view(harness.aspect());
    auto scene = plate_scene();
    const anima::Vec3 custom{1.2F, .8F, -8};
    (void)custom_material_test::add(*scene, custom_material_test::surface(custom, .4F, .4F), fogged_probe());
    auto environment = height_fog();
    const auto height_falloff = environment.fog.falloff;
    // A black sky, from an atmosphere that neither scatters nor absorbs over a black ground, under a sun behind the
    // eye, so that no sun disc shows, fogged as a surface beyond the plates would be.
    auto &atmosphere = environment.atmosphere;
    atmosphere.enabled = true;
    atmosphere.rayleigh_scattering = atmosphere.mie_scattering = atmosphere.mie_absorption =
        atmosphere.ozone_absorption = atmosphere.ground_albedo = {0, 0, 0};
    environment.sun = {{0, .4F, 1}, {1, 1, 1}};
    constexpr float sky_distance = 25; // Beyond the farthest plate, 20 m ahead.
    environment.fog.sky_distance = sky_distance;
    environment.exposure = float(1 / maximum_half_float);
    constexpr anima::Vec3 beyond{1e6F, 1e6F, 3e4F};
    constexpr auto cap = float(maximum_half_float);
    const anima::Vec3 capped{std::min(beyond.x, cap), std::min(beyond.y, cap), std::min(beyond.z, cap)};
    // A point far ahead, on the horizon right of every quad.
    const anima::Vec3 on_sky{8, eye.y, -250};
    for (const auto &[falloff, kind] : {std::pair{0.F, "uniform"}, std::pair{height_falloff, "height"}}) {
        const std::string what = std::string(kind) + " fog", name = std::string("capped-") + kind,
                          at_cap = name + "-at-cap";
        environment.fog.falloff = falloff;
        environment.fog.color = beyond;
        harness.render(name, {scene}, view_projection, environment);
        const auto &image = harness.images[name];
        const auto expect = [&](anima::Vec3 world, const Point &at, const Color &color, const std::string &which) {
            require((1 - transmittance(environment, at)) * beyond.x * environment.exposure > 1,
                    which + " in " + what + " is fogged too little for the uncapped fog's light to show white");
            harness.expect(name, view_projection, world, fogged(environment, at, color),
                           which + " in " + what + " of a color above the largest half float");
        };
        for (const auto &plate : plates())
            expect(plate.center, seen(view_projection, image, plate.center, plate.center.z), plate.color, "A quad");
        expect(custom, seen(view_projection, image, custom, custom.z), probe_color,
               "A custom quad through animaFogged()");
        const auto beside =
            ray(anima::inverse(view_projection), blending_test::pixel_of(view_projection, on_sky, image), image);
        expect(on_sky,
               {eye.x + beside[0] * sky_distance, eye.y + beside[1] * sky_distance, eye.z + beside[2] * sky_distance},
               blending_test::black, "The black sky");
        environment.fog.color = capped;
        harness.render(at_cap, {scene}, view_projection, environment);
        const auto mismatch =
            "A fog color above the largest half float drew another frame in " + what + " than the color capped at it";
        harness.images.require_same(name, at_cap, mismatch);
        harness.images.discard({name, at_cap});
    }
}

inline int run(int argc, char **argv) {
    require(argc == 3, "Usage: consumer --fog OUTPUT");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    blending_test::Harness harness(argv[2]);
    check_height_fog(harness);
    check_uniform(harness);
    check_sky(harness);
    check_background(harness);
    check_custom(harness);
    check_capped_light(harness);
    harness.finish();
    std::cout << "PASS fog: height fog and sunlight scattered forward and backward match the documented density "
                 "integrated along each ray, for opaque and blended quads and through animaFogged(), uniform fog lit "
                 "by the sun ignores its height, the sky and the background fogged at the sky distance meet a quad "
                 "there without the seam that they show unfogged, the background shows its color exposed and tone "
                 "mapped, and a fog color above the largest half float lights the fog as that color capped at it "
                 "does\n";
    return 0;
}
} // namespace fog_test
