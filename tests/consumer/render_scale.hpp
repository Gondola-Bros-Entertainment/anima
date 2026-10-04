#pragma once
// The render scale through the public API. The view renders into scene targets of the window's pixel size times
// VulkanRenderer::render_scale(), which display conversion filters to the window. At 0.5, 1.5 and 2 every capture keeps
// the window's size and shows the same scene, with its edges softened or smoothed; the scene targets' bytes follow the
// square of the scale; water that reads opaque depth and color through the frame block's viewport shows the backdrops
// behind it as at 1; and the LOD threshold counts the scene targets' pixels, so that at 0.5 a threshold chooses the
// levels that twice that threshold chooses at 1. Above 1 each window pixel averages the target pixels under its area,
// weighted by that area and taken as the window shows them, so an edge against a backdrop exposed past white still
// blends in proportion to its coverage. A renderer constructed at a scale draws exactly what one set to it draws,
// returning to 1 restores the first frame exactly, scales outside 0.25 to 2 are rejected, and the replaced targets
// outlive the frames in flight under validation. No scale change recreates the swapchain, which CTest checks in the log
// between STABLE_SWAPCHAIN_BEGIN and STABLE_SWAPCHAIN_END. A failure to create the scene targets, injected through
// RendererFailureStage::scene_targets, keeps the previous ones on a scale change and none after a swapchain recreation,
// and either way leaves the renderer usable at a lower scale.
#include "custom_materials.hpp"
#include "gpu_checks.hpp"
#include "lod.hpp"
#include "rejection.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <anima/desktop/vulkan_renderer.hpp>
#include <anima/mesh_placements.hpp>
#include <anima/scene.hpp>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace render_scale_test {
inline void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
using rejection::rejects;
constexpr std::string_view invalid_scale = "Render scale must be finite and from 0.25 to 2";
constexpr float half = .5F, unscaled = 1, doubled = 2;
// A scale above 1 at which each window pixel covers parts of some scene target pixels.
constexpr float fractional = 1.5F;
constexpr int window_width = 640, window_height = 480;
// Frames drawn at each scale, so that the targets that a change replaces are destroyed while later frames run.
constexpr int frames_per_scale = 3;

// Scales that are not finite or lie outside VulkanRenderer::min_render_scale to max_render_scale.
inline std::vector<float> invalid_scales() {
    constexpr auto infinity = std::numeric_limits<float>::infinity();
    return {.2F,
            0,
            -1,
            std::nextafter(anima::VulkanRenderer::min_render_scale, 0.F),
            std::nextafter(anima::VulkanRenderer::max_render_scale, infinity),
            infinity,
            -infinity,
            std::numeric_limits<float>::quiet_NaN()};
}

// Construction rejects a RendererOptions::render_scale that is not finite or lies outside 0.25 to 2, after the frames
// in flight and before it needs a window or GPU, and accepts the scene targets' failure stage, which draw() fires.
inline void reject_invalid_render_scale() {
    const auto construct = [](float scale, std::uint32_t frames) {
        anima::RendererOptions options;
        options.render_scale = scale;
        options.frames_in_flight = frames;
        anima::VulkanRenderer renderer(nullptr, options);
    };
    for (const auto scale : invalid_scales())
        rejects<std::invalid_argument>([&] { construct(scale, 2); }, invalid_scale);
    for (const auto scale : {anima::VulkanRenderer::min_render_scale, half, unscaled, doubled})
        rejects<std::invalid_argument>([&] { construct(scale, 2); }, "Renderer requires an SDL window");
    // The frames in flight are checked first.
    rejects<std::invalid_argument>([&] { construct(std::numeric_limits<float>::quiet_NaN(), 3); },
                                   "Frames in flight must be 1 or 2");
    rejects<std::invalid_argument>(
        [] {
            anima::RendererOptions options;
            options.fail_after = anima::RendererFailureStage::scene_targets;
            anima::VulkanRenderer renderer(nullptr, options);
        },
        "Renderer requires an SDL window");
}

// A square turned on screen, so that its edges follow no pixel row or column.
struct TurnedSquare {
    anima::Vec3 center{};
    // Its corners, in order around it.
    std::array<anima::Vec3, 4> corners{};
};
// Adds to @p scene an unlit square of @p color centered on @p center, facing the eye, with sides of twice
// @p half_side.
inline TurnedSquare add_turned_square(anima::Scene &scene, const blending_test::Color &color, anima::Vec3 center,
                                      float half_side) {
    constexpr float turn = std::numbers::pi_v<float> / 7;
    const anima::Vec3 u{std::cos(turn) * half_side, std::sin(turn) * half_side, 0},
        v{-std::sin(turn) * half_side, std::cos(turn) * half_side, 0};
    (void)scene.add(blending_test::quad(blending_test::opaque(color), center, u, v));
    return {center, {center - u - v, center + u - v, center + u + v, center - u + v}};
}
// The box of @p square's corners through @p view in @p image, grown by a pixel on each side.
struct PixelBox {
    blending_test::Pixel low{}, high{};
};
inline PixelBox box_around(const TurnedSquare &square, const anima::Mat4 &view, const gpu_check::Image &image) {
    auto low = blending_test::pixel_of(view, square.corners[0], image), high = low;
    for (const auto &corner : square.corners) {
        const auto at = blending_test::pixel_of(view, corner, image);
        for (std::size_t axis = 0; axis < at.size(); ++axis) {
            low[axis] = std::min(low[axis], at[axis]);
            high[axis] = std::max(high[axis], at[axis]);
        }
    }
    return {{low[0] - 1, low[1] - 1}, {high[0] + 1, high[1] + 1}};
}
// The window pixels that one of @p square's sides spans across @p image through @p view.
inline std::size_t side_pixels(const TurnedSquare &square, const anima::Mat4 &view, const gpu_check::Image &image) {
    return blending_test::pixel_of(view, square.corners[1], image)[0] -
           blending_test::pixel_of(view, square.corners[0], image)[0];
}

// Seen from blending_test::perspective_view(): unlit backdrops that fill the left and right halves, water in the middle
// that reads their depth and color, as custom_material_test::check_water draws it, and above the water on the left an
// unlit square turned on screen, whose edges follow no pixel row or column.
struct Pool {
    static constexpr blending_test::Color near_color{.9, .5, .1}, far_color{.1, .6, .9}, square_color{.3, .8, .4};
    static constexpr anima::Vec3 water_center{0, 0, -2};
    // Points that the water shows over each backdrop, and points of each backdrop away from every edge.
    static constexpr std::array<anima::Vec3, 4> samples{
        {{-.25F, 0, water_center.z}, {.25F, 0, water_center.z}, {-.8F, -1.4F, -4}, {3.6F, 2.7F, -9}}};
    std::shared_ptr<anima::Scene> scene = std::make_shared<anima::Scene>();
    TurnedSquare square;
    Pool() {
        (void)scene->add(blending_test::facing(blending_test::opaque(near_color), {-1.5F, 0, -4}, 1.5F, 2));
        (void)scene->add(blending_test::facing(blending_test::opaque(far_color), {3, 0, -9}, 3, 4));
        (void)custom_material_test::add(*scene, custom_material_test::surface(water_center, .6F, .4F),
                                        custom_material_test::water_material({{.02, .15, .2}, .35}));
        constexpr float half_side = .2F;
        square = add_turned_square(*scene, square_color, {-1.33F, 1.05F, -3.5F}, half_side);
    }
};

// Pixels around @p square, within the box of its corners through @p view grown by a pixel, whose color differs by more
// than the blending tolerance from both the square's and the backdrop's in @p reference: blends of the two, which only
// a scaled view draws, since the rasterizer covers each pixel of the scene targets or not.
inline std::size_t blended_pixels(const TurnedSquare &square, const anima::Mat4 &view,
                                  const gpu_check::Image &reference, const gpu_check::Image &image) {
    const auto box = box_around(square, view, reference);
    const auto center = blending_test::pixel_of(view, square.center, reference);
    const auto shown = gpu_check::pixel(reference, center[0], center[1]),
               backdrop = gpu_check::pixel(reference, box.low[0], box.low[1]);
    std::size_t count = 0;
    for (auto y = box.low[1]; y <= box.high[1]; ++y)
        for (auto x = box.low[0]; x <= box.high[0]; ++x) {
            const auto color = gpu_check::pixel(image, x, y);
            count += gpu_check::difference(color, shown) > blending_test::tolerance &&
                     gpu_check::difference(color, backdrop) > blending_test::tolerance;
        }
    return count;
}

// Draws @p frames frames of @p renderer, reading the last back into @p captures as @p name when it is not empty, and
// returns the resource counters of the last.
inline anima::ResourceStats draw(anima::VulkanRenderer &renderer, gpu_check::Captures &captures,
                                 const std::string &name, int frames = frames_per_scale) {
    const auto started = std::chrono::steady_clock::now();
    for (int frame = 0; frame < frames; ++frame) {
        const bool capture = !name.empty() && frame + 1 == frames;
        if (capture)
            renderer.request_capture();
        // A draw() that returns false presented nothing, so the frame is drawn again.
        for (;;) {
            require(std::chrono::steady_clock::now() - started < gpu_check::watchdog, "Render scale watchdog expired");
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                        "Render scale check interrupted");
                if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                    renderer.request_resize();
            }
            if (renderer.draw())
                break;
            SDL_Delay(5);
        }
        if (capture)
            captures.add(name, gpu_check::take(renderer));
    }
    return renderer.resource_stats();
}

// Requires @p scaled bytes to be @p factor times @p reference bytes within a tenth, which allows the allocations'
// alignment.
inline void require_bytes(std::uint64_t reference, std::uint64_t scaled, double factor, const std::string &what) {
    constexpr double allowance = .1;
    const double ratio = double(scaled) / double(reference);
    std::cout << "RENDER SCALE " << what << ": " << scaled << " bytes, " << ratio << " times " << reference << '\n';
    require(reference > 0 && std::abs(ratio - factor) <= factor * allowance,
            what + " are " + std::to_string(ratio) + " times those at scale 1, not about " + std::to_string(factor));
}

// The main-view counters that the chosen levels of detail decide.
struct Levels {
    std::uint64_t draw_calls{}, lod_draws{}, submitted_indices{};
    explicit Levels(const anima::ResourceStats &stats)
        : draw_calls(stats.draw_calls), lod_draws(stats.lod_draws), submitted_indices(stats.submitted_indices) {}
    bool operator==(const Levels &) const = default;
    [[nodiscard]] std::string text() const {
        return std::to_string(draw_calls) + " calls, " + std::to_string(lod_draws) + " at a level, " +
               std::to_string(submitted_indices) + " indices";
    }
};

// At a render scale of 0.5 or 2 the LOD threshold chooses the levels that twice or half of it chooses at 1, since the
// scene targets' height, which projects each level's error to pixels, halves or doubles exactly.
inline void check_levels(anima::VulkanRenderer &renderer, gpu_check::Captures &captures, float aspect) {
    const auto sphere = anima::Mesh::compile(lod_test::sphere_asset(), anima::TexelRetention::keep, {6});
    std::vector<anima::Mat4> field;
    constexpr int rows = 20;
    constexpr float spacing = 3;
    for (int z = 0; z < rows; ++z)
        for (int x = 0; x < rows; ++x) {
            auto m = anima::identity();
            anima::set_translation(m, {spacing * float(x), 0, spacing * float(z)});
            field.push_back(m);
        }
    auto scene = std::make_shared<anima::Scene>();
    scene->create("field", sphere).renderer().set_placements(anima::MeshPlacements::create(sphere, field));
    renderer.set_scenes({scene});
    renderer.set_view(anima::perspective(std::numbers::pi_v<float> / 4, aspect, .1F, 300) *
                      anima::look_at({28.5F, 14, 62}, {28.5F, 0, 30}));
    const auto levels = [&](float scale, float threshold) {
        renderer.set_render_scale(scale);
        renderer.set_lod_threshold(threshold);
        const Levels chosen(draw(renderer, captures, {}));
        std::cout << "RENDER SCALE levels at scale " << scale << ", threshold " << threshold << ": " << chosen.text()
                  << '\n';
        return chosen;
    };
    constexpr float threshold = 1;
    const auto unscaled_levels = levels(unscaled, threshold), coarser = levels(unscaled, threshold * 2),
               finer = levels(unscaled, threshold / 2);
    require(coarser != unscaled_levels && finer != unscaled_levels,
            "Halving or doubling the LOD threshold did not change the levels, so the check cannot tell scales apart");
    const auto at_half = levels(half, threshold), at_double = levels(doubled, threshold);
    require(at_half == coarser, "At render scale 0.5 the LOD threshold chose " + at_half.text() +
                                    ", not the levels of twice the threshold at 1: " + coarser.text());
    require(at_double == finer, "At render scale 2 the LOD threshold chose " + at_double.text() +
                                    ", not the levels of half the threshold at 1: " + finer.text());
    renderer.set_render_scale(unscaled);
}

// Above scale 1 display conversion averages the scene target pixels that each window pixel's area covers, weighted by
// the area each covers, as the window shows them. A black square over a white backdrop exposed far past white, with
// tone mapping off, then shows in linear light, at each window pixel around the square's edges, the share of its area
// that the backdrop covers: a multiple of a quarter at scale 2, where it covers 2x2 target pixels, and of a ninth at
// 1.5, where it covers one target pixel whole, two by halves and one by a quarter. Averaging radiance before the clamp
// would show white wherever the backdrop covers any of the area, and bilinear weights would give sixteenths at 1.5.
inline void check_bright_edges(anima::VulkanRenderer &renderer, gpu_check::Captures &captures, float aspect) {
    constexpr float exposure = 16;
    auto scene = std::make_shared<anima::Scene>();
    (void)scene->add(blending_test::facing(blending_test::opaque({1, 1, 1}), {0, 0, -4}, 3, 2.5F));
    constexpr float half_side = .3F;
    const auto square = add_turned_square(*scene, blending_test::black, {0, 0, -3}, half_side);
    anima::Environment bright;
    bright.exposure = exposure;
    renderer.set_environment(bright);
    renderer.set_scenes({scene});
    const auto view = blending_test::perspective_view(aspect);
    renderer.set_view(view);
    // A scale, and the number of equal parts of a window pixel's area of which the backdrop's share is a multiple.
    struct Case {
        float scale;
        int parts;
        std::string name;
    };
    for (const auto &test : {Case{doubled, 4, "bright-2"}, Case{fractional, 9, "bright-1.5"}}) {
        require(exposure / float(test.parts) > 1,
                "The bright edge check needs a backdrop beyond white in one part of a pixel");
        renderer.set_render_scale(test.scale);
        (void)draw(renderer, captures, test.name);
        const auto &image = captures[test.name];
        // Whether @p pixel shows white over @p covered parts of its area, and black over the rest.
        const auto shows = [&](const gpu_check::Rgb &pixel, int covered) {
            const auto expected = blending_test::encoded(double(covered) / test.parts);
            return std::all_of(pixel.begin(), pixel.end(),
                               [&](int channel) { return std::abs(expected - channel) <= blending_test::tolerance; });
        };
        const auto box = box_around(square, view, image);
        std::size_t partial = 0;
        for (auto y = box.low[1]; y <= box.high[1]; ++y)
            for (auto x = box.low[0]; x <= box.high[0]; ++x) {
                const auto actual = gpu_check::pixel(image, x, y);
                int covered = 0;
                while (covered <= test.parts && !shows(actual, covered))
                    ++covered;
                captures.require(covered <= test.parts,
                                 test.name + " shows " + gpu_check::text(actual) + " at pixel " + std::to_string(x) +
                                     ", " + std::to_string(y) + ", which no share of white in " +
                                     std::to_string(test.parts) + " parts gives",
                                 {test.name});
                partial += covered > 0 && covered < test.parts;
            }
        std::cout << "RENDER SCALE pixels of the square's edges that blend it with white at " << test.name << ": "
                  << partial << '\n';
        captures.require(partial > side_pixels(square, view, image),
                         test.name + " blends the square's edges with a backdrop beyond white in only " +
                             std::to_string(partial) + " pixels",
                         {test.name});
        captures.discard({test.name});
    }
    renderer.set_environment({});
    renderer.set_render_scale(unscaled);
}

// RendererFailureStage::scene_targets fails each creation of scene targets above scale 1, as a failed allocation of
// them does. A scale change that fails keeps the previous targets, which draw the frame they drew before, and the next
// draw() tries again; a swapchain recreation, which releases the targets, leaves none, without making the renderer
// fatal; and a lower scale then creates them. @p options select the pool scene.
inline void check_failures(SDL_Window *window, anima::RendererOptions options, gpu_check::Captures &captures,
                           const anima::Mat4 &view) {
    constexpr std::string_view injected = "Injected initialization failure after scene-targets";
    options.fail_after = anima::RendererFailureStage::scene_targets;
    anima::VulkanRenderer renderer(window, options);
    renderer.set_view(view);
    const auto kept = draw(renderer, captures, "failure-1");
    renderer.set_render_scale(doubled);
    // The second attempt shows that the failure left the size to create again.
    for (int attempt = 0; attempt < 2; ++attempt)
        rejects<anima::SceneResourceError>([&] { (void)renderer.draw(); }, injected);
    require(renderer.render_scale() == doubled &&
                renderer.resource_stats().world_target_bytes == kept.world_target_bytes,
            "A failure to create scene targets changed the render scale or the targets");
    renderer.set_render_scale(unscaled);
    (void)draw(renderer, captures, "failure-1-kept");
    captures.require_same("failure-1", "failure-1-kept", "The scene targets kept after a failure drew another frame");
    renderer.set_render_scale(doubled);
    renderer.request_resize();
    rejects<anima::SceneResourceError>([&] { (void)renderer.draw(); }, injected);
    require(renderer.resource_stats().world_target_bytes == 0,
            "Scene targets remained after a swapchain recreation that failed to create them");
    rejects<anima::SceneResourceError>([&] { (void)renderer.draw(); }, injected);
    renderer.set_render_scale(half);
    (void)draw(renderer, captures, "failure-0.5");
    captures.require_same("scale-0.5", "failure-0.5",
                          "Scene targets created after failures drew another frame than those of scale 0.5");
    const auto stats = renderer.shutdown();
    require(!stats.validation_errors && !stats.validation_warnings,
            "Failures to create scene targets had validation warnings or errors");
    captures.discard({"failure-1", "failure-1-kept", "failure-0.5"});
}

inline int run(int argc, char **argv) {
    require(argc == 3 || (argc == 5 && std::string_view(argv[3]) == "--frames-in-flight"),
            "Usage: consumer --render-scale OUTPUT [--frames-in-flight COUNT]");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    gpu_check::Video video;
    const auto window = gpu_check::window("Anima render scale verification", window_width, window_height);
    int width = 0, height = 0;
    require(SDL_GetWindowSizeInPixels(window.get(), &width, &height) && width > 0 && height > 0,
            "Render scale window has no drawable size");
    // Halving the height exactly keeps the LOD comparison exact.
    require(height % 2 == 0, "The render scale window's pixel height is odd");
    const float aspect = float(width) / float(height);
    const auto view = blending_test::perspective_view(aspect);
    const Pool pool;
    gpu_check::Captures captures(argv[2]);
    anima::RendererOptions options;
    options.validation = true;
    options.scenes = {pool.scene};
    if (argc == 5)
        options.frames_in_flight = static_cast<std::uint32_t>(std::stoul(argv[4]));

    // A renderer constructed at 0.5 creates its first scene targets at that scale.
    anima::ResourceStats constructed_stats;
    {
        auto constructed = options;
        constructed.render_scale = half;
        anima::VulkanRenderer renderer(window.get(), constructed);
        require(renderer.render_scale() == half, "RendererOptions::render_scale did not set the render scale");
        renderer.set_view(view);
        constructed_stats = draw(renderer, captures, "constructed-0.5");
        const auto stats = renderer.shutdown();
        require(!stats.validation_errors && !stats.validation_warnings,
                "The renderer constructed at scale 0.5 had validation warnings or errors");
    }

    anima::VulkanRenderer renderer(window.get(), options);
    require(renderer.render_scale() == unscaled, "The render scale does not start at 1");
    for (const auto scale : invalid_scales())
        rejects<std::invalid_argument>([&] { renderer.set_render_scale(scale); }, invalid_scale);
    require(renderer.render_scale() == unscaled, "A rejected render scale replaced the previous one");
    renderer.set_view(view);
    const auto unscaled_stats = draw(renderer, captures, "scale-1");
    require(unscaled_stats.opaque_inputs, "The water did not read opaque depth and color");
    // Scale changes replace only the scene targets: CTest fails the check on a swapchain created between the markers.
    std::cout << "STABLE_SWAPCHAIN_BEGIN\n";
    const auto scaled = [&](float scale, const std::string &name) {
        renderer.set_render_scale(scale);
        require(renderer.render_scale() == scale, "set_render_scale() did not set the render scale");
        return draw(renderer, captures, name);
    };
    const auto half_stats = scaled(half, "scale-0.5");
    const auto fractional_stats = scaled(fractional, "scale-1.5");
    const auto double_stats = scaled(doubled, "scale-2");
    const auto restored_stats = scaled(unscaled, "scale-1-again");
    // Two changes between draws leave only the second.
    renderer.set_render_scale(doubled);
    const auto twice_stats = scaled(half, "scale-0.5-again");

    // The scene targets and the opaque copies follow the square of the scale; captures keep the window's size.
    require_bytes(unscaled_stats.world_target_bytes, half_stats.world_target_bytes, half * half,
                  "Scene target bytes at scale 0.5");
    require_bytes(unscaled_stats.world_target_bytes, fractional_stats.world_target_bytes, fractional * fractional,
                  "Scene target bytes at scale 1.5");
    require_bytes(unscaled_stats.world_target_bytes, double_stats.world_target_bytes, doubled * doubled,
                  "Scene target bytes at scale 2");
    require_bytes(unscaled_stats.opaque_input_bytes, half_stats.opaque_input_bytes, half * half,
                  "Opaque copy bytes at scale 0.5");
    require_bytes(unscaled_stats.opaque_input_bytes, double_stats.opaque_input_bytes, doubled * doubled,
                  "Opaque copy bytes at scale 2");
    require(restored_stats.world_target_bytes == unscaled_stats.world_target_bytes &&
                twice_stats.world_target_bytes == half_stats.world_target_bytes &&
                constructed_stats.world_target_bytes == half_stats.world_target_bytes,
            "Scene targets of one scale differ in size");
    for (const auto *name :
         {"constructed-0.5", "scale-1", "scale-0.5", "scale-1.5", "scale-2", "scale-1-again", "scale-0.5-again"})
        captures.require(captures[name].width == std::uint32_t(width) && captures[name].height == std::uint32_t(height),
                         std::string(name) + " is not the window's size", {name});
    captures.require_same("scale-1", "scale-1-again", "Returning to scale 1 did not restore the frame");
    captures.require_same("scale-0.5", "scale-0.5-again", "Scale 0.5 drew another frame after a change through 2");
    captures.require_same("constructed-0.5", "scale-0.5",
                          "A renderer constructed at scale 0.5 drew another frame than one set to it");

    // Each scale changes the edges, and only the edges. The backdrops, and the water over each of them, show the colors
    // of scale 1 within the blending tolerance. At 1 the square's edges are hard, and at 0.5, 1.5 and 2 its edge pixels
    // blend it with the backdrop, in more pixels than one of its sides spans across the frame. Most of the frame
    // matches scale 1.
    const auto &reference = captures["scale-1"];
    const auto side = side_pixels(pool.square, view, reference);
    const auto hard = blended_pixels(pool.square, view, reference, reference);
    std::cout << "RENDER SCALE blended pixels at the square's edges at scale-1: " << hard << '\n';
    captures.require(hard == 0,
                     "At scale 1 the square's edges blend with the backdrop in " + std::to_string(hard) + " pixels",
                     {"scale-1"});
    for (const auto *name : {"scale-0.5", "scale-1.5", "scale-2"}) {
        for (const auto point : Pool::samples) {
            const auto at = blending_test::pixel_of(view, point, reference);
            const auto expected = gpu_check::pixel(reference, at[0], at[1]),
                       actual = gpu_check::pixel(captures[name], at[0], at[1]);
            std::cout << "RENDER SCALE " << name << " at pixel " << at[0] << ", " << at[1] << ": "
                      << gpu_check::text(actual) << ", scale-1 " << gpu_check::text(expected) << '\n';
            captures.require(gpu_check::difference(expected, actual) <= blending_test::tolerance,
                             std::string(name) + " shows " + gpu_check::text(actual) + " at pixel " +
                                 std::to_string(at[0]) + ", " + std::to_string(at[1]) + ", where scale-1 shows " +
                                 gpu_check::text(expected),
                             {"scale-1", name});
        }
        const auto blended = blended_pixels(pool.square, view, reference, captures[name]);
        std::cout << "RENDER SCALE blended pixels at the square's edges at " << name << ": " << blended << '\n';
        captures.require(blended > side,
                         std::string(name) + " blends the square's edges in only " + std::to_string(blended) +
                             " pixels",
                         {"scale-1", name});
        const auto difference = gpu_check::parity(reference, captures[name]);
        std::cout << "RENDER SCALE " << name << " against scale-1: mean channel difference " << difference.mean
                  << ", pixels beyond 16 levels " << difference.large << '\n';
        constexpr double mean_limit = 1, large_limit = .02;
        captures.require(difference.mean < mean_limit && difference.large < large_limit,
                         std::string(name) + " differs from scale-1 beyond its edges: mean channel difference " +
                             std::to_string(difference.mean) + ", pixels beyond 16 levels " +
                             std::to_string(difference.large),
                         {"scale-1", name});
    }
    check_bright_edges(renderer, captures, aspect);
    check_levels(renderer, captures, aspect);
    std::cout << "STABLE_SWAPCHAIN_END\n";
    const auto stats = renderer.shutdown();
    require(!stats.validation_errors && !stats.validation_warnings,
            "Render scale changes had validation warnings or errors");
    constexpr std::string_view shut_down = "Renderer is shut down";
    rejects<std::logic_error>([&] { renderer.set_render_scale(unscaled); }, shut_down);
    // The renderer's state is checked before the scale.
    rejects<std::logic_error>([&] { renderer.set_render_scale(-1); }, shut_down);

    check_failures(window.get(), options, captures, view);
    std::cout << "PASS render scale: scales 0.5, 1.5 and 2 render the window-sized frame of scale 1 from scene targets "
                 "of the square of the scale times its bytes, scale 2 averages each block as the window shows it, "
                 "water samples its opaque copies where scale 1 does, the LOD threshold counts the scene targets' "
                 "pixels, construction and set_render_scale() agree, invalid scales are rejected, and failures to "
                 "create scene targets leave the renderer usable; validation_warnings=0 validation_errors=0\n";
    return 0;
}
} // namespace render_scale_test
