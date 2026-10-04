#pragma once
// Tone mapping against tone_mapping_reference.hpp's independent evaluation of each anima::ToneMapping curve. A custom
// material, custom_ramp.frag, fills a grid of patches with chosen linear colors: ramps from 2^-12 to 2^8 in half stops
// in gray, in the Rec.709 primaries and secondaries, in an orange, a sky blue and a grass green, and in two reds whose
// hue after AgX's inset lies just above 0 and just below 1, where it wraps; each curve's breakpoints after exposure, a
// sixteenth below, at and a sixteenth above; and a control, neutral 2 after exposure, which the four curves show at
// levels further apart than the tolerance, so a curve that the renderer ignored or confused with another fails there.
// Each color is rounded to the scene target's format before it is written, so the target holds it exactly whatever
// rounding the device applies, and the center pixel of each patch must show the display encoding of the reference's
// result within 2 levels: at exposures 1 and 1.7, at render scale 1, whose display conversion filters the target
// bilinearly before the curve, and at 2, where each window pixel averages the curve's results for the scene pixels
// under it, and in the packed SceneColorFormat::b10g11r11 target. check_reference() first holds the reference to
// properties its sources state.
#include "blending.hpp"
#include "custom_materials.hpp"
#include "tone_mapping_reference.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <anima/custom_material.hpp>
#include <anima/environment.hpp>
#include <anima/scene.hpp>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace tone_mapping_test {
constexpr std::uint32_t ramp_fragment[] =
#include "custom_ramp.frag.inc"
    ;
using blending_test::require;
using tone_mapping_reference::Rgb;

// The power of 2 that custom_ramp.frag multiplies each object's factor by; the factor is at most 1, so colors reach
// 2^8, and dividing by it keeps every rounded color exact in a float factor.
constexpr double ramp_scale = 256;
// The control's color after exposure, which the curves show apart.
constexpr double control = 2;

// Holds tone_mapping_reference.hpp to properties that its sources state. PBR Neutral shows each channel of a color
// whose channels all lie from 0.08 to 0.8 0.04 lower, never shows a channel above 1, and keeps every color in the plane
// through it and white. AgX keeps neutral 0.18 at 0.18, shows a neutral ramp that never falls, and shows neutral colors
// below 0.18 * 2^-10 as 0 and from 16.3 as 1. The control's four levels differ by more than blending_test::tolerance.
inline void check_reference() {
    const tone_mapping_reference::Curves curves;
    const auto agx = [&](double v) { return curves.agx({v, v, v}); };
    constexpr double exact = 1e-12;
    // Channels from 0.08 to 0.8 in steps of 0.04.
    const auto channel = [](int step) { return .08 + .04 * step; };
    for (int red = 0; red <= 18; ++red)
        for (int green = 0; green <= 18; ++green)
            for (int blue = 0; blue <= 18; ++blue) {
                const double r = channel(red), g = channel(green), b = channel(blue);
                const auto shown = curves.pbr_neutral({r, g, b});
                require(std::abs(shown[0] - (r - .04)) < exact && std::abs(shown[1] - (g - .04)) < exact &&
                            std::abs(shown[2] - (b - .04)) < exact,
                        "The PBR Neutral reference does not show a color from 0.08 to 0.8 0.04 lower");
            }
    for (int exponent = -12; exponent <= 15; ++exponent)
        for (const Rgb &hue : {Rgb{1, .35, .08}, Rgb{.25, .45, 1}, Rgb{1, 0, 0}, Rgb{.12, .25, .04}, Rgb{1, 1, 1}}) {
            const Rgb c{std::ldexp(hue[0], exponent), std::ldexp(hue[1], exponent), std::ldexp(hue[2], exponent)};
            const auto shown = curves.pbr_neutral(c);
            require(std::max({shown[0], shown[1], shown[2]}) <= 1, "The PBR Neutral reference shows a channel above 1");
            // The determinant of the input, the output and white is 0 when the three lie in one plane through 0.
            const double plane =
                             c[0] * (shown[1] - shown[2]) - c[1] * (shown[0] - shown[2]) + c[2] * (shown[0] - shown[1]),
                         size = std::max({c[0], c[1], c[2]}) * std::max({shown[0], shown[1], shown[2]});
            require(std::abs(plane) <= 1e-12 * size,
                    "The PBR Neutral reference moved a color out of the plane through it and white");
        }
    require(std::abs(agx(.18)[0] - .18) < 1e-9, "The AgX reference does not keep neutral 0.18");
    double previous = 0;
    for (int step = -14 * 64; step <= 10 * 64; ++step) {
        const auto shown = agx(std::exp2(step / 64.0));
        require(shown[0] >= previous && std::abs(shown[0] - shown[1]) < exact && std::abs(shown[1] - shown[2]) < exact,
                "The AgX reference falls or leaves neutral along a neutral ramp");
        previous = shown[0];
    }
    require(agx(.18 * std::exp2(-10) * .999)[0] == 0 && agx(1e-6)[0] == 0,
            "The AgX reference shows a neutral below 10 stops under 0.18 above 0");
    for (const double v : {16.3, 20., 1000., 65504.})
        require(agx(v)[0] == 1, "The AgX reference shows a neutral from 16.3 below 1");
    std::vector<double> levels;
    for (const auto mapping : tone_mapping_reference::mappings)
        levels.push_back(blending_test::encoded(curves.displayed({control, control, control}, 1, mapping)[0]));
    for (std::size_t a = 0; a < levels.size(); ++a)
        for (std::size_t b = a + 1; b < levels.size(); ++b)
            require(std::abs(levels[a] - levels[b]) > blending_test::tolerance,
                    "Two curves show the control within the tolerance of each other, so it could not tell them apart");
}

// A patch: its linear color as the scene target holds it, and what it shows.
struct Patch {
    Rgb color;
    std::string what;
};
// The mantissa bits that @p format keeps in each channel.
inline std::array<int, 3> mantissa_bits(anima::SceneColorFormat format) {
    if (format == anima::SceneColorFormat::b10g11r11)
        return {6, 6, 5};
    return {10, 10, 10};
}
// The colors of the ramps, each at 1 in its largest channel.
inline const std::vector<std::pair<Rgb, std::string>> &ramp_colors() {
    static const std::vector<std::pair<Rgb, std::string>> value{
        {{1, 1, 1}, "gray"},         {{1, 0, 0}, "red"},         {{0, 1, 0}, "green"},   {{0, 0, 1}, "blue"},
        {{1, 1, 0}, "yellow"},       {{0, 1, 1}, "cyan"},        {{1, 0, 1}, "magenta"}, {{1, .35, .08}, "orange"},
        {{.25, .45, 1}, "sky blue"}, {{.12, .25, .04}, "grass"}, {{1, .05, .1}, "rose"}, {{1, 0, .2}, "crimson"}};
    return value;
}
// Colors at the curves' breakpoints after exposure. PBR Neutral's offset changes form where the smallest channel
// reaches 0.08, and its compression starts where the largest channel, less the offset of 0.04, passes 0.76. AgX's
// logarithm meets 0, the sigmoid's pivot and 1 at neutral 0.18 * 2^-10, 0.18 and 0.18 * 2^6.5, as the inset keeps
// neutral colors neutral.
inline const std::vector<std::pair<Rgb, std::string>> &breakpoints() {
    static const double floor = .18 * std::exp2(-10), white = .18 * std::exp2(6.5);
    static const std::vector<std::pair<Rgb, std::string>> value{{{.08, .08, .08}, "PBR Neutral's offset in gray"},
                                                                {{.5, .3, .08}, "PBR Neutral's offset in orange"},
                                                                {{.8, .8, .8}, "PBR Neutral's compression in gray"},
                                                                {{.8, .4, .2}, "PBR Neutral's compression in orange"},
                                                                {{floor, floor, floor}, "AgX's black"},
                                                                {{.18, .18, .18}, "AgX's pivot"},
                                                                {{white, white, white}, "AgX's white"}};
    return value;
}
// Every patch for @p exposure, rounded to the nearest values of @p format: the ramps, the breakpoints after exposure
// and the control.
inline std::vector<Patch> patches(double exposure, anima::SceneColorFormat format) {
    const auto bits = mantissa_bits(format);
    std::vector<Patch> result;
    const auto add = [&](const Rgb &color, const std::string &what) {
        result.push_back(
            {{tone_mapping_reference::rounded(color[0], bits[0]), tone_mapping_reference::rounded(color[1], bits[1]),
              tone_mapping_reference::rounded(color[2], bits[2])},
             what});
    };
    for (const auto &[color, name] : ramp_colors())
        for (int half_stops = -24; half_stops <= 16; ++half_stops) {
            const double scale = std::exp2(half_stops / 2.0);
            std::ostringstream stops;
            stops << half_stops / 2.0;
            add({color[0] * scale, color[1] * scale, color[2] * scale}, name + " at 2^" + stops.str());
        }
    for (const auto &[color, name] : breakpoints())
        for (const auto &[side, where] :
             {std::pair{15 / 16., "below "}, std::pair{1., "at "}, std::pair{17 / 16., "above "}})
            add({color[0] * side / exposure, color[1] * side / exposure, color[2] * side / exposure}, where + name);
    add({control / exposure, control / exposure, control / exposure}, "the control, neutral 2 after exposure");
    return result;
}

// The patches' places: rows of `columns` cells across an orthographic view 2 units high, each patch covering 80% of
// its cell, so that the center of each patch lies many pixels from its edges.
class Grid {
  public:
    static constexpr std::size_t columns = 30;
    Grid(float aspect, std::size_t count)
        : aspect_(aspect), rows_(float((count + columns - 1) / columns)),
          view_(anima::orthographic(aspect, 2, .1F, 10) * anima::look_at({0, 0, 0}, {0, 0, -1})) {}
    [[nodiscard]] const anima::Mat4 &view() const noexcept { return view_; }
    [[nodiscard]] anima::Vec3 center(std::size_t index) const {
        const auto column = float(index % columns), row = float(index / columns);
        return {aspect_ * (2 * (column + .5F) / columns - 1), 1 - 2 * (row + .5F) / rows_, -1};
    }
    [[nodiscard]] float half_width() const { return .8F * aspect_ / columns; }
    [[nodiscard]] float half_height() const { return .8F / rows_; }

  private:
    float aspect_, rows_;
    anima::Mat4 view_;
};

inline std::shared_ptr<const anima::CustomMaterial> ramp_material() {
    anima::CustomMaterialDefinition definition;
    definition.name = "tone mapping ramp";
    definition.vertex_shader = custom_material_test::words(custom_material_test::surface_vertex);
    definition.fragment_shader = custom_material_test::words(ramp_fragment);
    definition.parameters = custom_material_test::parameters({{float(ramp_scale), 0, 0, 0}});
    return std::make_shared<const anima::CustomMaterial>(std::move(definition));
}
// A quad for each of @p set at its place in @p grid, drawn with @p material and its color over ramp_scale as its
// factor.
inline std::shared_ptr<anima::Scene> patch_scene(const Grid &grid, const std::vector<Patch> &set,
                                                 const std::shared_ptr<const anima::CustomMaterial> &material) {
    auto scene = std::make_shared<anima::Scene>();
    const auto mesh = custom_material_test::surface({0, 0, 0}, grid.half_width(), grid.half_height());
    for (std::size_t i = 0; i < set.size(); ++i) {
        auto object = custom_material_test::add(*scene, mesh, material);
        object.set_local_position(grid.center(i));
        const auto &c = set[i].color;
        object.renderer().set_material_factor(
            0, {float(c[0] / ramp_scale), float(c[1] / ramp_scale), float(c[2] / ramp_scale)});
    }
    return scene;
}

// Requires the center pixel of each of @p set in capture @p name to show the display level of its color through
// @p mapping at @p exposure, as @p curves evaluates it, within blending_test::tolerance levels in every channel.
inline void expect(const blending_test::Harness &harness, const std::string &name, const Grid &grid,
                   const std::vector<Patch> &set, double exposure, anima::ToneMapping mapping,
                   const tone_mapping_reference::Curves &curves) {
    const auto &image = harness.images[name];
    double largest = 0;
    std::size_t worst = 0, failed = 0;
    std::string failures;
    for (std::size_t i = 0; i < set.size(); ++i) {
        const auto expected = curves.displayed(set[i].color, exposure, mapping);
        const auto at = blending_test::pixel_of(grid.view(), grid.center(i), image);
        const auto actual = gpu_check::pixel(image, at[0], at[1]);
        double difference = 0;
        std::string wanted;
        for (std::size_t c = 0; c < expected.size(); ++c) {
            difference = std::max(difference, std::abs(actual[c] - blending_test::encoded(expected[c])));
            wanted += (c ? " " : "") + std::to_string(blending_test::encoded(expected[c]));
        }
        if (difference > largest) {
            largest = difference;
            worst = i;
        }
        // The first few failures, which name the patches.
        constexpr std::size_t reported = 8;
        if (difference > blending_test::tolerance && failed++ < reported)
            failures += "; " + set[i].what + " (linear " + blending_test::text(set[i].color) + ") shows " +
                        gpu_check::text(actual) + ", expected " + wanted;
    }
    std::cout << "TONE MAPPING " << name << ": " << set.size() << " patches, largest difference " << largest
              << " levels, at " << set[worst].what << '\n';
    harness.images.require(failed == 0,
                           name + ": " + std::to_string(failed) + " of " + std::to_string(set.size()) +
                               " patches differ from the reference by more than 2 levels" + failures,
                           {name});
}

// Draws the patches through every curve at exposures 1 and 1.7 into @p harness's scene target, names each capture with
// @p suffix, and compares it with the reference.
inline void check_frames(blending_test::Harness &harness, const std::string &suffix) {
    const auto format = harness.scene_color_format();
    const auto material = ramp_material();
    const tone_mapping_reference::Curves curves;
    for (const float exposure : {1.F, 1.7F}) {
        const auto set = patches(exposure, format);
        const Grid grid(harness.aspect(), set.size());
        const auto scene = patch_scene(grid, set, material);
        anima::Environment environment;
        environment.background = {0, 0, 0};
        environment.exposure = exposure;
        for (const auto mapping : tone_mapping_reference::mappings) {
            environment.tone_mapping = mapping;
            const auto name = std::string(tone_mapping_reference::name(mapping)) + "-exposure-" +
                              (exposure == 1 ? "1" : "1.7") + "-" + suffix;
            harness.render(name, {scene}, grid.view(), environment);
            expect(harness, name, grid, set, exposure, mapping, curves);
            harness.images.discard({name});
        }
    }
}

inline int run(int argc, char **argv) {
    require(argc == 3, "Usage: consumer --tone-mapping OUTPUT");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    {
        blending_test::Harness harness(argv[2]);
        require(harness.scene_color_format() == anima::SceneColorFormat::rgba16f,
                "The renderer did not default to the RGBA16F scene target");
        check_frames(harness, "rgba16f");
        harness.set_render_scale(2);
        check_frames(harness, "rgba16f-scale-2");
        harness.finish();
    }
    {
        // One window at a time: the first renderer and its window close before this one opens.
        blending_test::Harness harness(argv[2], anima::SceneColorFormat::b10g11r11);
        const bool packed = harness.scene_color_format() == anima::SceneColorFormat::b10g11r11;
        std::cout << "TONE MAPPING scene color format for a b10g11r11 request: "
                  << (packed ? "B10G11R11_UFLOAT_PACK32" : "R16G16B16A16_SFLOAT, as the device lacks the packed format")
                  << '\n';
        check_frames(harness, packed ? "b10g11r11" : "b10g11r11-fallback");
        harness.finish();
    }
    std::cout << "PASS tone mapping: ramps from 2^-12 to 2^8 in gray and in saturated colors, each curve's breakpoints "
                 "and a control show each anima::ToneMapping curve within 2 levels of an independent double-precision "
                 "evaluation, at exposures 1 and 1.7, at render scales 1 and 2, and in the packed scene target\n";
    return 0;
}
} // namespace tone_mapping_test
