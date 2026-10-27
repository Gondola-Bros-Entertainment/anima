#pragma once
// Bloom (anima::Bloom) through VulkanRenderer::set_bloom(), in an orthographic view down -Z over a black background.
// The checks of bloom's light turn dithering off, so that frames compare exactly; check_dither() covers it.
//
// A field of one color, a sprite that covers the view, passes every downsample and upsample unchanged but for the
// rounding of the chain's stores, so with bloom each of its pixels must show its exposed color plus the intensity times
// the light that the threshold keeps of it, within 2 levels of what that light gives exactly or through stores rounded,
// to nearest or toward zero, in the chain's coarser possible format: above the knee, in it, with a hard threshold and
// with none, after exposure, at radii that sum one level, several with a fractional coarsest weight and every level,
// and at render scales 0.5, 1 and 2, in the RGBA16F and the packed scene targets. A field whose light the threshold
// keeps none of shows exactly as without bloom. Bright light from an additive sprite, a blended sprite, an emissive
// mesh and a custom material spreads into the black around each, and farther with a larger radius, while a scene of
// light under the knee, an intensity of 0, a threshold above the scene's light and an exposure that lowers the light
// under the knee all show exactly as without bloom. Disabling bloom restores the frame exactly and releases the chain
// on the next draw(); the chain follows render scale changes and window resizes, and a window resized back shows its
// first frame again exactly.
//
// Dithering, at render scales 1 and 2, leaves a dark horizontal gradient, whose levels are each many pixels wide, with
// no band: no edge, a step of half a level between the mean levels of adjacent columns, for a band to lie between, and
// it brings the column means at least a quarter level closer to the gradient's exact encoded values than quantization
// alone; without dithering the same measures find an edge at most levels. A dithered frame drawn again is the same,
// black stays black, and bloom without dithering shows the gradient exactly as without bloom.
//
// Invalid settings are rejected, keeping the previous ones, and validation, synchronization validation included, stays
// clean. reject_invalid_bloom() checks construction's rejections without a window.
#include "blending.hpp"
#include "custom_materials.hpp"
#include "gpu_checks.hpp"
#include "rejection.hpp"
#include "sprites.hpp"
#include "tone_mapping.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <anima/assets/asset.hpp>
#include <anima/desktop/vulkan_renderer.hpp>
#include <anima/environment.hpp>
#include <anima/mesh.hpp>
#include <anima/scene.hpp>
#include <anima/sprite.hpp>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bloom_test {
using blending_test::Color;
using rejection::rejects;

inline void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

// The window's size in pixels before and during the resize check.
constexpr int width = 800, height = 600, resized_width = 640, resized_height = 480;
// The view's height, in units: 150 pixels per unit in a window 600 pixels high.
constexpr float view_height = 4;
// Half the side of each square light source.
constexpr float half_side = .15F;
// How far outside a source's edge its glow is read: 15 pixels in a window 600 pixels high.
constexpr float glow_gap = .1F;
// The least rise, in display levels, that a bright source's glow must give the black at glow_gap from its edge.
constexpr int least_glow = 8;
// Fire's light, with a brightness of 4: above every default threshold and knee.
constexpr Color fire{4, 2, .5};
// Light under the default knee, which starts at a brightness of 0.5.
constexpr Color dim{.4, .35, .3};
// A threshold above the light of every source in the scene, the emissive mesh's specular highlights included.
constexpr float above_every_source = 10;

// Bloom enabled with @p threshold, @p knee, @p intensity and @p radius, without dithering.
inline anima::Bloom enabled(float threshold = anima::Bloom{}.threshold, float knee = anima::Bloom{}.knee,
                            float intensity = anima::Bloom{}.intensity, float radius = anima::Bloom{}.radius) {
    return {true, threshold, knee, intensity, radius, false};
}

// Construction rejects each invalid field with its message, in the fields' order, before it needs a window.
inline void reject_invalid_bloom() {
    const auto construct = [](anima::Bloom bloom) {
        anima::RendererOptions options;
        options.bloom = bloom;
        anima::VulkanRenderer renderer(nullptr, options);
    };
    constexpr float nan = std::numeric_limits<float>::quiet_NaN(), infinity = std::numeric_limits<float>::infinity();
    for (const float threshold : {-.001F, nan, infinity})
        rejects<std::invalid_argument>([&] { construct(enabled(threshold)); },
                                       "Bloom threshold must be finite and nonnegative");
    for (const float knee : {-.001F, 1.001F, nan})
        rejects<std::invalid_argument>([&] { construct(enabled(1, knee)); },
                                       "Bloom knee must be finite and from 0 to 1");
    for (const float intensity : {-.001F, nan, infinity})
        rejects<std::invalid_argument>([&] { construct(enabled(1, .5F, intensity)); },
                                       "Bloom intensity must be finite and nonnegative");
    for (const float radius : {0.F, -.1F, 1.001F, nan})
        rejects<std::invalid_argument>([&] { construct(enabled(1, .5F, 1, radius)); },
                                       "Bloom radius must be finite, above 0 and at most 1");
    // Fields are checked in order, and a disabled bloom is checked too.
    rejects<std::invalid_argument>([&] { construct({false, nan, 2, -1, 0}); },
                                   "Bloom threshold must be finite and nonnegative");
    for (const auto &valid : {anima::Bloom{}, enabled(), enabled(0, 0, 0, 1), enabled(1e30F, 1, 1e30F, 1e-6F)})
        rejects<std::invalid_argument>([&] { construct(valid); }, "Renderer requires an SDL window");
}

// The levels that the bloom sums, for @p radius in scene targets of @p scene pixels, and the weight of the coarsest, as
// anima::Bloom::radius defines them: `ceil(log2(radius * height))` of them, at least 1 and at most the chain's levels
// from half the targets' size down to 1 pixel, the coarsest weighted by how far that logarithm passes the count less 1.
struct Levels {
    std::uint32_t count{};
    double coarsest_weight{};
};
inline Levels levels_of(float radius, std::array<std::uint32_t, 2> scene) {
    const auto available = double(std::bit_width(std::max({scene[0] / 2, scene[1] / 2, 1U})));
    const double exact = std::log2(double(radius) * scene[1]), count = std::clamp(std::ceil(exact), 1.0, available);
    return {std::uint32_t(count), count == 1 ? 1.0 : std::clamp(exact - (count - 1), 0.0, 1.0)};
}
// The mantissa bits of red, green and blue in VK_FORMAT_B10G11R11_UFLOAT_PACK32, the coarser of the formats that the
// renderer may give the bloom chain, which halves no texel of a field, so that only its stores round.
constexpr std::array<int, 3> chain_bits{6, 6, 5};
// @p value rounded toward zero in a nonnegative float format with @p mantissa_bits explicit mantissa bits, as
// tone_mapping_reference::rounded() rounds to nearest; Vulkan lets an implementation choose either for unsigned floats.
inline double truncated(double value, int mantissa_bits) {
    if (value <= 0)
        return 0;
    int exponent = 0;
    (void)std::frexp(value, &exponent);
    const int step = std::max(exponent - 1, -14) - mantissa_bits;
    return std::ldexp(std::floor(std::ldexp(value, -step)), step);
}
// The mean of a field's levels that the chain's first level holds for the light @p kept that the bright pass keeps in
// each texel, through @p levels whose stores round with @p round: the bright pass stores the kept light, each
// downsample of a field keeps it, and each upsample stores the sum of a level and the level after it, the coarsest
// weighted, which display conversion divides by the levels' total weight.
template <class Round> double level_mean(double kept, Levels levels, Round round) {
    const double down = round(kept);
    double up = down;
    for (std::uint32_t level = levels.count - 1; level-- > 0;)
        up = round(down + (level + 2 == levels.count ? levels.coarsest_weight : 1) * up);
    return up / (double(levels.count - 1) + levels.coarsest_weight);
}
// The least and most linear color, in each channel, that a field shows.
struct Range {
    Color low, high;
};
// The colors that a field of linear @p color shows through @p bloom at @p exposure, without tone mapping, in scene
// targets of @p scene pixels: its exposed color plus the intensity times the light that the threshold keeps of it, as
// anima::Bloom defines that, after the chain's stores: exact, or rounded to nearest or toward zero in chain_bits.
inline Range bloomed(const Color &color, double exposure, const anima::Bloom &bloom,
                     std::array<std::uint32_t, 2> scene) {
    Color exposed{};
    for (std::size_t c = 0; c < exposed.size(); ++c)
        exposed[c] = color[c] * exposure;
    const double brightness = std::max({exposed[0], exposed[1], exposed[2]}), threshold = bloom.threshold,
                 knee = threshold * bloom.knee;
    double share = 0;
    if (bloom.enabled && brightness > 0) {
        const double soft =
            knee > 0 ? std::pow(std::clamp(brightness - threshold + knee, 0., 2 * knee), 2) / (4 * knee) : 0;
        share = std::max(brightness - threshold, soft) / brightness;
    }
    const auto levels = levels_of(bloom.radius, scene);
    Range range{exposed, exposed};
    for (std::size_t c = 0; c < exposed.size(); ++c) {
        if (share == 0)
            continue;
        const double kept = exposed[c] * share;
        const std::array means{
            kept, level_mean(kept, levels, [&](double v) { return tone_mapping_reference::rounded(v, chain_bits[c]); }),
            level_mean(kept, levels, [&](double v) { return truncated(v, chain_bits[c]); })};
        range.low[c] += bloom.intensity * *std::min_element(means.begin(), means.end());
        range.high[c] += bloom.intensity * *std::max_element(means.begin(), means.end());
    }
    return range;
}

// Draws scenes and sprites in one window, with validation, and keeps each frame read back by name.
class Harness {
  public:
    Harness(const std::filesystem::path &output, std::uint32_t frames_in_flight, anima::SceneColorFormat format)
        : window_(gpu_check::window("Anima bloom verification", width, height)),
          renderer_(window_.get(), options(frames_in_flight, format)), images(output) {
        fit_view();
        set_exposure(1);
    }
    [[nodiscard]] anima::VulkanRenderer &renderer() noexcept { return renderer_; }
    /// The window's size in pixels.
    [[nodiscard]] std::array<std::uint32_t, 2> pixels() const {
        int w = 0, h = 0;
        require(SDL_GetWindowSizeInPixels(window_.get(), &w, &h) && w > 0 && h > 0,
                "Bloom window has no drawable size");
        return {std::uint32_t(w), std::uint32_t(h)};
    }
    /// The scene targets' size: the window's pixels times the render scale, each side rounded to the nearest pixel.
    [[nodiscard]] std::array<std::uint32_t, 2> scene() const {
        const auto window = pixels();
        const auto scaled = [&](std::uint32_t side) {
            return std::uint32_t(std::round(double(side) * double(renderer_.render_scale())));
        };
        return {scaled(window[0]), scaled(window[1])};
    }
    /// Width over height of the window's pixels.
    [[nodiscard]] float aspect() const {
        const auto window = pixels();
        return float(window[0]) / float(window[1]);
    }
    /// Resizes the window to @p w by @p h pixels and fits the view to it.
    void resize(int w, int h) {
        require(SDL_SetWindowSize(window_.get(), w, h), "Bloom resize failed");
        require(SDL_SyncWindow(window_.get()), "Bloom resize did not settle");
        renderer_.request_resize();
        fit_view();
    }
    /// Sets the environment's exposure, over a black background without tone mapping.
    void set_exposure(float exposure) {
        anima::Environment environment;
        environment.background = {0, 0, 0};
        environment.exposure = exposure;
        renderer_.set_environment(environment);
    }
    /// Draws @p batches over @p scenes and reads the frame back as @p name.
    void render(const std::string &name, std::vector<anima::SpriteBatch> batches,
                std::vector<std::shared_ptr<const anima::Scene>> scenes = {}) {
        renderer_.set_scenes(std::move(scenes));
        renderer_.set_sprites(std::move(batches));
        redraw(name);
    }
    /// Draws the current selection and sprites again and reads the frame back as @p name.
    void redraw(const std::string &name) {
        renderer_.request_capture();
        const auto started = std::chrono::steady_clock::now();
        for (;;) {
            require(std::chrono::steady_clock::now() - started < gpu_check::watchdog, "Bloom watchdog");
            SDL_Event event{};
            while (SDL_PollEvent(&event))
                require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                        "Bloom test interrupted");
            if (renderer_.draw())
                break;
            SDL_Delay(5);
        }
        images.add(name, gpu_check::take(renderer_));
        stats = renderer_.resource_stats();
    }
    /// The pixel of capture @p name that shows (@p x, @p y) in the view.
    [[nodiscard]] gpu_check::Rgb at(const std::string &name, float x, float y) const {
        const auto &image = images[name];
        const auto pixel = blending_test::pixel_of(view, {x, y, sprite_test::plane}, image);
        return gpu_check::pixel(image, pixel[0], pixel[1]);
    }
    /// Requires the pixel of capture @p name that shows (@p x, @p y) to show a linear color in @p expected, within
    /// blending_test::tolerance levels per channel of the display encodings of its ends.
    void expect(const std::string &name, float x, float y, const Range &expected, const std::string &what) const {
        const auto actual = at(name, x, y);
        bool matched = true;
        std::string wanted;
        for (std::size_t c = 0; c < actual.size(); ++c) {
            const auto low = blending_test::encoded(expected.low[c]), high = blending_test::encoded(expected.high[c]);
            matched =
                matched && actual[c] >= low - blending_test::tolerance && actual[c] <= high + blending_test::tolerance;
            wanted += (c ? " " : "") + std::to_string(low) + " to " + std::to_string(high);
        }
        const auto report = what + ": (" + std::to_string(x) + ", " + std::to_string(y) + ") of " + name + " shows " +
                            gpu_check::text(actual) + ", expected " + wanted + " (linear " +
                            blending_test::text(expected.low) + " to " + blending_test::text(expected.high) + ")";
        std::cout << "BLOOM " << report << '\n';
        images.require(matched, report, {name});
    }
    /// Shuts the renderer down and requires clean validation and no bloom chain.
    void finish() {
        const auto result = renderer_.shutdown();
        require(!result.validation_errors && !result.validation_warnings, "Bloom GPU validation failed");
        require(renderer_.resource_stats().bloom_bytes == 0, "Shutdown kept the bloom chain");
    }

  private:
    static anima::RendererOptions options(std::uint32_t frames_in_flight, anima::SceneColorFormat format) {
        anima::RendererOptions settings;
        settings.validation = true;
        settings.log = gpu_check::log;
        settings.frames_in_flight = frames_in_flight;
        settings.scene_color_format = format;
        settings.profile = true;
        return settings;
    }
    void fit_view() {
        view = anima::orthographic(aspect(), view_height, .1F, 50) * anima::look_at(blending_test::origin, {0, 0, -1});
        renderer_.set_view(view);
    }
    gpu_check::Video video_;
    gpu_check::Window window_;
    anima::VulkanRenderer renderer_;

  public:
    /// The view that every render draws through.
    anima::Mat4 view{};
    gpu_check::Captures images;
    /// Resource counters of the latest render().
    anima::ResourceStats stats{};
};

// A square sprite of linear @p color centered on (@p x, @p y), composited as @p blend.
inline anima::SpriteBatch square(float x, float y, const Color &color, anima::SpriteBlend blend) {
    return {sprite_test::sheet(),
            blend,
            {sprite_test::sprite(x, y, sprite_test::solid, 2 * half_side, 2 * half_side, color)}};
}
// A sprite of linear @p color that covers the whole view.
inline anima::SpriteBatch field(const Color &color) {
    constexpr float covers_view = 20;
    return {sprite_test::sheet(),
            anima::SpriteBlend::additive,
            {sprite_test::sprite(0, 0, sprite_test::solid, covers_view, covers_view, color)}};
}

// The sources of light, each a square centered at its place: an additive sprite, a blended sprite, a lit mesh whose
// black surface emits light, and a custom material that writes its factor times tone_mapping_test::ramp_scale.
struct Sources {
    std::vector<anima::SpriteBatch> sprites;
    std::shared_ptr<const anima::Scene> scene;
    // Each source's center and what it is.
    std::vector<std::pair<std::array<float, 2>, std::string>> places;
};
inline Sources sources(const Color &color, float y) {
    Sources result;
    constexpr float spacing = 1.2F, first = -1.8F;
    result.sprites = {square(first, y, color, anima::SpriteBlend::additive),
                      square(first + spacing, y, color, anima::SpriteBlend::blended)};
    auto scene = std::make_shared<anima::Scene>();
    auto emissive = blending_test::opaque({0, 0, 0}, true);
    emissive.emissive = {float(color[0]), float(color[1]), float(color[2])};
    (void)scene->create(
        {}, blending_test::facing(emissive, {first + 2 * spacing, y, sprite_test::plane}, half_side, half_side));
    auto custom = custom_material_test::add(
        *scene, custom_material_test::surface({first + 3 * spacing, y, sprite_test::plane}, half_side, half_side),
        tone_mapping_test::ramp_material());
    const auto scale = tone_mapping_test::ramp_scale;
    custom.renderer().set_material_factor(0,
                                          {float(color[0] / scale), float(color[1] / scale), float(color[2] / scale)});
    result.scene = std::move(scene);
    result.places = {{{first, y}, "an additive sprite"},
                     {{first + spacing, y}, "a blended sprite"},
                     {{first + 2 * spacing, y}, "an emissive mesh"},
                     {{first + 3 * spacing, y}, "a custom material"}};
    return result;
}
// The points at glow_gap outside each edge of a source centered at @p center.
inline std::array<std::array<float, 2>, 4> beside(std::array<float, 2> center) {
    const float d = half_side + glow_gap;
    return {{{center[0] - d, center[1]},
             {center[0] + d, center[1]},
             {center[0], center[1] - d},
             {center[0], center[1] + d}}};
}
inline int brightest(const gpu_check::Rgb &color) { return std::max({color[0], color[1], color[2]}); }

// The renderer starts with the default bloom, off, and set_bloom() rejects invalid settings, keeping the previous ones.
inline void check_settings(Harness &harness) {
    auto &renderer = harness.renderer();
    const auto same = [](const anima::Bloom &a, const anima::Bloom &b) {
        return a.enabled == b.enabled && a.threshold == b.threshold && a.knee == b.knee && a.intensity == b.intensity &&
               a.radius == b.radius && a.dither == b.dither;
    };
    const anima::Bloom defaults;
    require(!defaults.enabled && defaults.threshold == 1 && defaults.knee == .5F && defaults.intensity == .35F &&
                defaults.radius == .1F && defaults.dither,
            "The default bloom changed");
    require(same(renderer.bloom(), defaults), "The renderer did not start with the default bloom");
    const auto kept = enabled(2, .25F, .5F, .3F);
    renderer.set_bloom(kept);
    require(same(renderer.bloom(), kept), "set_bloom() did not take the bloom");
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    rejects<std::invalid_argument>([&] { renderer.set_bloom(enabled(-1)); },
                                   "Bloom threshold must be finite and nonnegative");
    rejects<std::invalid_argument>([&] { renderer.set_bloom(enabled(1, nan)); },
                                   "Bloom knee must be finite and from 0 to 1");
    rejects<std::invalid_argument>([&] { renderer.set_bloom(enabled(1, .5F, -1)); },
                                   "Bloom intensity must be finite and nonnegative");
    rejects<std::invalid_argument>([&] { renderer.set_bloom(enabled(1, .5F, 1, 2)); },
                                   "Bloom radius must be finite, above 0 and at most 1");
    require(same(renderer.bloom(), kept), "A rejected bloom replaced the previous one");
    renderer.set_bloom({});
}

// Draws a field of @p color, rounded to the scene target's format so that the target holds it exactly whatever rounding
// the device applies, as capture @p name, and requires its pixels at the view's center and near two corners to show
// bloomed() of that color.
inline void expect_field(Harness &harness, const std::string &name, const Color &color, float exposure,
                         const anima::Bloom &bloom, const std::string &what) {
    const auto bits = tone_mapping_test::mantissa_bits(harness.renderer().scene_color_format());
    Color held{};
    for (std::size_t c = 0; c < held.size(); ++c)
        held[c] = tone_mapping_reference::rounded(color[c], bits[c]);
    harness.set_exposure(exposure);
    harness.renderer().set_bloom(bloom);
    harness.render(name, {field(held)});
    const auto expected = bloomed(held, exposure, bloom, harness.scene());
    const float x = .95F * harness.aspect() * view_height / 2, y = .95F * view_height / 2;
    for (const auto &[px, py] : {std::pair{0.F, 0.F}, std::pair{-x, y}, std::pair{x, -y}})
        harness.expect(name, px, py, expected, what);
}
inline void check_fields(Harness &harness, const std::string &suffix) {
    // A threshold of 0.5 with the default knee, so that the knee spans brightness 0.25 to 0.75 and the shown colors
    // stay below 1, which display conversion without tone mapping clamps.
    constexpr float threshold = .5F;
    constexpr Color above{.8, .4, .2}, inside{.6, .3, .15}, below{.2, .15, .1};
    const auto bloom = [&](float radius) { return enabled(threshold, .5F, .5F, radius); };
    // One level at every render scale, several with a fractional coarsest weight, and every level down to 1 pixel.
    for (const float radius : {.001F, .37F, 1.F})
        expect_field(harness, "field-above-knee-radius-" + std::to_string(radius) + "-" + suffix, above, 1,
                     bloom(radius), "A field above the knee, at radius " + std::to_string(radius));
    expect_field(harness, "field-in-knee-" + suffix, inside, 1, bloom(.1F), "A field in the knee");
    expect_field(harness, "field-exposed-" + suffix, {.4, .2, .1}, 2, bloom(.1F),
                 "A field exposed twice, above the knee after exposure");
    expect_field(harness, "field-hard-threshold-" + suffix, inside, 1, enabled(threshold, 0, .5F),
                 "A field above a hard threshold");
    expect_field(harness, "field-no-threshold-" + suffix, below, 1, enabled(0, .5F, 1.5F),
                 "A field without a threshold");
    // Light that the threshold keeps none of shows exactly as without bloom.
    expect_field(harness, "field-below-knee-" + suffix, below, 1, bloom(.1F), "A field below the knee");
    expect_field(harness, "field-off-" + suffix, below, 1, {}, "A field without bloom");
    harness.images.require_same("field-below-knee-" + suffix, "field-off-" + suffix,
                                "A field below the knee did not show exactly as without bloom");
    harness.set_exposure(1);
    harness.renderer().set_bloom({});
}

// Bright sources spread light into the black around them, farther with a larger radius; dim light, an intensity of 0,
// a threshold above the scene's light and an exposure that lowers it under the knee show exactly as without bloom; and
// disabling bloom restores the frame and releases the chain.
inline void check_spread(Harness &harness) {
    auto &renderer = harness.renderer();
    const auto bright = sources(fire, .9F), faint = sources(dim, -.9F);
    const auto render = [&](const std::string &name, const Sources &set) {
        harness.render(name, set.sprites, {set.scene});
    };
    render("bright-off", bright);
    require(harness.stats.bloom_bytes == 0, "A frame without bloom holds a bloom chain");
    renderer.set_bloom(enabled());
    render("bright-on", bright);
    require(harness.stats.bloom_bytes > 0, "A frame with bloom holds no bloom chain");
    const auto profile = renderer.frame_profile();
    if (profile.gpu_available)
        std::cout << "BLOOM gpu_bloom_ms " << profile.gpu_bloom_ms << " of gpu_ms " << profile.gpu_ms << " at " << width
                  << "x" << height << '\n';
    for (const auto &[center, what] : bright.places)
        for (const auto &[x, y] : beside(center)) {
            const auto off = harness.at("bright-off", x, y), on = harness.at("bright-on", x, y);
            std::cout << "BLOOM glow beside " << what << " at (" << x << ", " << y << "): " << gpu_check::text(off)
                      << " without bloom, " << gpu_check::text(on) << " with it\n";
            harness.images.require(brightest(off) == 0, "The black beside " + what + " is not black without bloom",
                                   {"bright-off"});
            harness.images.require(brightest(on) >= least_glow,
                                   "Bloom spread no light beside " + what + ": " + gpu_check::text(on),
                                   {"bright-off", "bright-on"});
        }
    // A wider radius reaches farther: half a unit, 75 pixels, beyond the additive sprite's outer edge.
    const auto &[additive, additive_name] = bright.places.front();
    const float far_x = additive[0] - half_side - .5F, far_y = additive[1];
    renderer.set_bloom(enabled(1, .5F, .35F, .05F));
    render("bright-narrow", bright);
    renderer.set_bloom(enabled(1, .5F, .35F, .4F));
    render("bright-wide", bright);
    const auto narrow = brightest(harness.at("bright-narrow", far_x, far_y)),
               wide = brightest(harness.at("bright-wide", far_x, far_y));
    std::cout << "BLOOM 75 pixels beyond " << additive_name << ": " << narrow << " at radius 0.05, " << wide
              << " at radius 0.4\n";
    harness.images.require(wide >= narrow + least_glow, "A wider radius did not spread light farther",
                           {"bright-narrow", "bright-wide"});
    harness.images.discard({"bright-narrow", "bright-wide"});
    // Each of these keeps no light, so the frame shows exactly as without bloom.
    renderer.set_bloom(enabled(1, .5F, 0));
    render("bright-no-intensity", bright);
    harness.images.require_same("bright-off", "bright-no-intensity", "An intensity of 0 changed the frame");
    renderer.set_bloom(enabled(above_every_source));
    render("bright-high-threshold", bright);
    harness.images.require_same("bright-off", "bright-high-threshold", "A threshold above every source bloomed");
    harness.images.discard({"bright-no-intensity", "bright-high-threshold"});
    // Exposed by 0.1, fire's brightness of 4 falls to 0.4, under the knee, so the threshold applies after exposure.
    constexpr float darkening = .1F;
    harness.set_exposure(darkening);
    renderer.set_bloom({});
    render("dark-off", bright);
    renderer.set_bloom(enabled());
    render("dark-on", bright);
    harness.images.require_same("dark-off", "dark-on", "Light that exposure lowers under the knee bloomed");
    harness.images.discard({"dark-off", "dark-on"});
    harness.set_exposure(1);
    renderer.set_bloom({});
    render("dim-off", faint);
    renderer.set_bloom(enabled());
    render("dim-on", faint);
    harness.images.require_same("dim-off", "dim-on", "Light under the knee bloomed");
    harness.images.discard({"dim-off", "dim-on"});
    // Disabling restores the frame without bloom, and the draw that finds it disabled releases the chain.
    renderer.set_bloom({});
    render("bright-disabled", bright);
    harness.images.require_same("bright-off", "bright-disabled", "Disabling bloom did not restore the frame");
    require(harness.stats.bloom_bytes == 0, "Disabling bloom kept the chain");
    harness.images.discard({"bright-disabled"});
}

// The chain follows the scene targets: render scales of 0.5 and 2 and a smaller window each bloom with a chain of their
// size, and the window resized back shows its first bloomed frame exactly.
inline void check_sizes(Harness &harness) {
    auto &renderer = harness.renderer();
    const auto bright = sources(fire, .9F);
    renderer.set_bloom(enabled());
    harness.render("sizes-first", bright.sprites, {bright.scene});
    const auto first_bytes = harness.stats.bloom_bytes;
    const auto glows = [&](const std::string &name) {
        for (const auto &[center, what] : bright.places)
            for (const auto &[x, y] : beside(center))
                harness.images.require(brightest(harness.at(name, x, y)) >= least_glow,
                                       name + ": bloom spread no light beside " + what, {name});
    };
    std::uint64_t previous_bytes = 0;
    for (const float scale : {.5F, 1.F, 2.F}) {
        renderer.set_render_scale(scale);
        const auto name = "sizes-scale-" + std::to_string(scale);
        harness.redraw(name);
        glows(name);
        std::cout << "BLOOM chain of " << harness.stats.bloom_bytes << " bytes at render scale " << scale << '\n';
        require(harness.stats.bloom_bytes > previous_bytes, "A larger render scale did not enlarge the chain");
        previous_bytes = harness.stats.bloom_bytes;
        harness.images.discard({name});
    }
    renderer.set_render_scale(1);
    harness.resize(resized_width, resized_height);
    harness.redraw("sizes-resized");
    require(harness.images["sizes-resized"].width < harness.images["sizes-first"].width, "The window did not resize");
    glows("sizes-resized");
    require(harness.stats.bloom_bytes > 0 && harness.stats.bloom_bytes < first_bytes,
            "A smaller window did not shrink the chain");
    harness.resize(width, height);
    harness.redraw("sizes-restored");
    harness.images.require_same("sizes-first", "sizes-restored",
                                "The window resized back did not show its first bloomed frame");
    harness.images.discard({"sizes-first", "sizes-resized", "sizes-restored"});
    renderer.set_bloom({});
}

// The dark gradient's linear colors at its left and right edges, which display shows from about level 13 to level 48,
// so that each level spans many pixels; and its half width and half height in the view.
constexpr Color gradient_left{.004, .004, .004}, gradient_right{.03, .03, .03};
constexpr float gradient_half_width = 2.4F, gradient_half_height = 1;
// How far inside the gradient's edges, in pixels and in view units, the measured columns and rows lie, clear of the
// pixels that its edges cover in part.
constexpr int gradient_margin = 2;
constexpr float gradient_row_margin = .1F;
// The banding measures' limits: a step between adjacent columns' mean levels that is a band edge, which the eye sees as
// a contour; and the least that dithering must bring the column means closer to the exact gradient than quantization
// alone, which leaves errors of up to half a level.
constexpr double band_edge = .5, least_mean_gain = .25;

// A quad facing the view, unlit and white, whose vertex colors run from gradient_left along its left edge to
// gradient_right along its right edge, so that its color is linear across the view.
inline std::shared_ptr<const anima::Mesh> gradient_quad() {
    anima::Asset asset;
    asset.nodes.resize(1);
    asset.materials.push_back(blending_test::opaque({1, 1, 1}));
    anima::SourcePrimitive primitive;
    primitive.material = 0;
    const float x = gradient_half_width, y = gradient_half_height;
    const std::array<anima::Vec3, 4> corners{{{-x, -y, sprite_test::plane},
                                              {x, -y, sprite_test::plane},
                                              {x, y, sprite_test::plane},
                                              {-x, y, sprite_test::plane}}};
    for (const auto corner : {0U, 1U, 2U, 0U, 2U, 3U}) {
        anima::SourceVertex vertex;
        vertex.position = corners[corner];
        vertex.normal = {0, 0, 1};
        const auto &color = corners[corner].x < 0 ? gradient_left : gradient_right;
        vertex.color = {float(color[0]), float(color[1]), float(color[2])};
        primitive.vertices.push_back(vertex);
    }
    asset.primitives.push_back(std::move(primitive));
    return anima::Mesh::compile(asset);
}

// What the dither check measures of the red levels of a capture of the gradient, away from its edges: the largest
// distance between a column's mean level over the gradient's rows and the gradient's exact encoded value at the
// column's center; the largest difference between adjacent columns' means, and how many adjacent columns differ by a
// band edge; and the widest band, the most columns between two consecutive band edges, 0 without two edges.
struct Banding {
    double mean_error{}, steepest{};
    int edges{}, widest{};
    [[nodiscard]] std::string text() const {
        return "largest mean error " + std::to_string(mean_error) + " levels, largest step between columns " +
               std::to_string(steepest) + " levels, " + std::to_string(edges) + " band edges, widest band " +
               std::to_string(widest) + " columns";
    }
};
inline Banding banding(const Harness &harness, const std::string &name) {
    const auto &image = harness.images[name];
    const auto &v = harness.view;
    // The window column, as a continuous coordinate, that shows view position @p x on the gradient's plane.
    const auto column_of = [&](float x) {
        const double clip = double(v[0]) * x + double(v[8]) * sprite_test::plane + double(v[12]),
                     w = double(v[3]) * x + double(v[11]) * sprite_test::plane + double(v[15]);
        return (clip / w + 1) / 2 * image.width;
    };
    const double left = column_of(-gradient_half_width), right = column_of(gradient_half_width);
    const auto first_row = blending_test::pixel_of(
                   v, {0, gradient_half_height - gradient_row_margin, sprite_test::plane}, image)[1],
               last_row = blending_test::pixel_of(
                   v, {0, -gradient_half_height + gradient_row_margin, sprite_test::plane}, image)[1];
    const auto first_column = std::size_t(std::ceil(left)) + gradient_margin,
               last_column = std::size_t(std::floor(right)) - 1 - gradient_margin;
    Banding result;
    std::vector<double> means;
    // The column after the latest band edge, once there has been one.
    std::optional<std::size_t> band_start;
    for (auto column = first_column; column <= last_column; ++column) {
        double sum = 0;
        for (auto row = first_row; row <= last_row; ++row)
            sum += gpu_check::pixel(image, column, row)[0];
        means.push_back(sum / double(last_row - first_row + 1));
        const double share = (double(column) + .5 - left) / (right - left),
                     exact = blending_test::encoded(gradient_left[0] + (gradient_right[0] - gradient_left[0]) * share);
        result.mean_error = std::max(result.mean_error, std::abs(means.back() - exact));
        if (means.size() < 2)
            continue;
        const double step = std::abs(means.back() - means[means.size() - 2]);
        result.steepest = std::max(result.steepest, step);
        if (step < band_edge)
            continue;
        ++result.edges;
        if (band_start)
            result.widest = std::max(result.widest, int(column - *band_start));
        band_start = column;
    }
    return result;
}

// Dithering removes the gradient's bands, keeps black black and stays the same from frame to frame, and bloom without
// it shows the gradient exactly as without bloom. A threshold above the gradient's light keeps bloom from adding any.
inline void check_dither(Harness &harness, const std::string &suffix) {
    auto &renderer = harness.renderer();
    auto scene = std::make_shared<anima::Scene>();
    (void)scene->create({}, gradient_quad());
    const std::vector<std::shared_ptr<const anima::Scene>> scenes{scene};
    const auto off = "gradient-off-" + suffix, plain = "gradient-plain-" + suffix,
               dithered = "gradient-dithered-" + suffix, again = "gradient-dithered-again-" + suffix;
    renderer.set_bloom({});
    harness.render(off, {}, scenes);
    renderer.set_bloom(enabled(above_every_source));
    harness.render(plain, {}, scenes);
    harness.images.require_same(off, plain, "Bloom that keeps no light, without dithering, changed the gradient");
    auto dithering = enabled(above_every_source);
    dithering.dither = true;
    renderer.set_bloom(dithering);
    harness.render(dithered, {}, scenes);
    harness.redraw(again);
    harness.images.require_same(dithered, again, "The dithered gradient changed while the view held still");
    // Every pixel of the black above the gradient, clear of the pixels that its edge covers in part.
    const auto &image = harness.images[dithered];
    const auto black_rows = blending_test::pixel_of(
        harness.view, {0, gradient_half_height + gradient_row_margin, sprite_test::plane}, image)[1];
    std::size_t lifted = 0;
    for (std::size_t row = 0; row < black_rows; ++row)
        for (std::size_t column = 0; column < image.width; ++column)
            lifted += brightest(gpu_check::pixel(image, column, row)) != 0;
    harness.images.require(lifted == 0,
                           "Dithering lifted " + std::to_string(lifted) + " pixels of the black above the gradient",
                           {dithered});
    const auto banded = banding(harness, off), smooth = banding(harness, dithered);
    std::cout << "BLOOM gradient " << suffix << " without dithering: " << banded.text() << '\n'
              << "BLOOM gradient " << suffix << " with dithering: " << smooth.text() << '\n';
    // The measures must find the bands that 8-bit levels leave, about one edge per level that the gradient crosses.
    const int crossings =
        int(blending_test::encoded(gradient_right[0])) - int(blending_test::encoded(gradient_left[0]));
    harness.images.require(banded.edges >= crossings / 2,
                           "The banding measures found no bands without dithering: " + banded.text(), {off});
    // No band edge leaves no band, and the column means follow the gradient, but for the device's own error in
    // encoding sRGB, which dithering leaves and quantization adds its half level to.
    harness.images.require(smooth.edges == 0 && smooth.mean_error <= banded.mean_error - least_mean_gain,
                           "The dithered gradient shows bands: " + smooth.text() + "; without dithering, " +
                               banded.text(),
                           {off, dithered});
    harness.images.discard({off, plain, dithered, again});
    renderer.set_bloom({});
}

inline int run(int argc, char **argv) {
    require(argc == 3 || (argc == 5 && std::string_view(argv[3]) == "--frames-in-flight"),
            "Usage: consumer --bloom OUTPUT [--frames-in-flight COUNT]");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    const auto frames =
        argc == 5 ? static_cast<std::uint32_t>(std::stoul(argv[4])) : anima::RendererOptions{}.frames_in_flight;
    {
        Harness harness(argv[2], frames, anima::SceneColorFormat::rgba16f);
        check_settings(harness);
        for (const float scale : {1.F, .5F, 2.F}) {
            harness.renderer().set_render_scale(scale);
            check_fields(harness, "rgba16f-scale-" + std::to_string(scale));
        }
        for (const float scale : {1.F, 2.F}) {
            harness.renderer().set_render_scale(scale);
            check_dither(harness, "scale-" + std::to_string(scale));
        }
        harness.renderer().set_render_scale(1);
        check_spread(harness);
        check_sizes(harness);
        harness.finish();
    }
    {
        // One window at a time: the first renderer and its window close before this one opens.
        Harness harness(argv[2], frames, anima::SceneColorFormat::b10g11r11);
        check_fields(harness, harness.renderer().scene_color_format() == anima::SceneColorFormat::b10g11r11
                                  ? "b10g11r11"
                                  : "b10g11r11-fallback");
        harness.finish();
    }
    std::cout << "PASS bloom: fields show their exposed color plus the intensity times the light that the threshold "
                 "keeps, within 2 levels, across the knee, exposure, radii and render scales 0.5 to 2 in both scene "
                 "formats; additive and blended sprites, emissive meshes and custom materials spread light, farther "
                 "with a larger radius; dim light, no intensity, a high threshold and low exposure show exactly as "
                 "without bloom; dithering leaves a dark gradient without bands, black and steady; disabling restores "
                 "the frame and releases the chain; and the chain follows render scales and resizes, with "
              << frames << " frames in flight\n";
    return 0;
}
} // namespace bloom_test
