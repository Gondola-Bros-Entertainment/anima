#pragma once
// Sprites against VulkanRenderer::set_sprites() and anima/sprite.hpp, through an orthographic view down -Z: placement,
// rotation, mirroring and pivots; tint and texel alpha; sprite and batch order; blended and additive compositing; depth
// against opaque meshes; drawing after a frame's blended draws when the frame copies opaque inputs; ten thousand
// sprites in one draw call; the GPU cache's release of sheets; and rejected batches, which keep the previous sprites.
// The sheet's texels are exact colors sampled with nearest filtering, so each expected pixel follows from that
// contract alone.
#include "blending.hpp"
#include "custom_materials.hpp"
#include "gpu_checks.hpp"
#include <algorithm>
#include <anima/scene.hpp>
#include <anima/sprite.hpp>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sprite_test {
using blending_test::Color;
using blending_test::over;
constexpr Color black{0, 0, 0}, red{1, 0, 0}, green{0, 1, 0}, blue{0, 0, 1}, white{1, 1, 1};
// Sprites lie in this plane, between the view's near and far planes.
constexpr float plane = -5;

inline void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

// The frames of sheet().
enum Frame : std::uint32_t { quadrants, solid, faint };
// Alpha of the faint frame's texels.
constexpr double faint_alpha = 128 / 255.;
// A 12 by 4 sheet, sampled with nearest filtering, clamped and unmipmapped. Frame `quadrants`, columns 0 to 3, is red
// at the top left, blue at the top right, green at the bottom left and white at the bottom right, in 2 by 2 texel
// quadrants, where row 0 is the top. Frame `solid`, columns 6 and 7 of rows 0 and 1, is opaque white, and frame
// `faint`, columns 10 and 11 of rows 0 and 1, red at alpha 128. Every other texel is transparent black.
inline std::shared_ptr<const anima::SpriteSheet> sheet() {
    constexpr std::uint32_t width = 12, height = 4;
    auto image = std::make_shared<anima::Image>();
    image->width = width;
    image->height = height;
    image->rgba.assign(std::size_t{width} * height * 4, 0);
    const auto paint = [&](std::uint32_t x, std::uint32_t y, std::array<std::uint8_t, 4> rgba) {
        std::copy(rgba.begin(), rgba.end(), image->rgba.begin() + std::ptrdiff_t{(y * width + x) * 4});
    };
    for (std::uint32_t y = 0; y < 4; ++y)
        for (std::uint32_t x = 0; x < 4; ++x) {
            const bool top = y < 2, left = x < 2;
            paint(
                x, y,
                top ? (left ? std::array<std::uint8_t, 4>{255, 0, 0, 255} : std::array<std::uint8_t, 4>{0, 0, 255, 255})
                    : (left ? std::array<std::uint8_t, 4>{0, 255, 0, 255}
                            : std::array<std::uint8_t, 4>{255, 255, 255, 255}));
        }
    for (std::uint32_t y = 0; y < 2; ++y)
        for (std::uint32_t x = 0; x < 2; ++x) {
            paint(6 + x, y, {255, 255, 255, 255});
            paint(10 + x, y, {255, 0, 0, 128});
        }
    anima::Texture texture{std::move(image), {}, anima::TextureEncoding::srgb};
    texture.sampler.mag = texture.sampler.min = anima::Filter::nearest;
    texture.sampler.u = texture.sampler.v = anima::Wrap::clamp;
    texture.sampler.mipmapped = false;
    return std::make_shared<const anima::SpriteSheet>(
        anima::SpriteSheetDefinition{std::move(texture), {{0, 0, 4, 4}, {6, 0, 2, 2}, {10, 0, 2, 2}}});
}
// A sprite of @p frame centered on (@p x, @p y) in the sprite plane, @p width by @p height, in linear @p color.
inline anima::Sprite sprite(float x, float y, std::uint32_t frame, float width, float height,
                            const Color &color = white, float alpha = 1) {
    anima::Sprite result;
    result.position = {x, y, plane};
    result.size = {width, height};
    result.frame = frame;
    result.color = {float(color[0]), float(color[1]), float(color[2])};
    result.alpha = alpha;
    return result;
}

// Draws sprites, over scenes when given, in one window and keeps each frame read back by name.
class Harness {
  public:
    Harness(const std::filesystem::path &output, std::uint32_t frames_in_flight)
        : window_(gpu_check::window("Anima sprite verification", 800, 600)),
          renderer_(window_.get(), options(frames_in_flight)), images(output) {
        int width = 0, height = 0;
        require(SDL_GetWindowSizeInPixels(window_.get(), &width, &height) && width > 0 && height > 0,
                "Sprite window has no drawable size");
        // 4 units high, centered on the origin; sprites at z = -5 lie between its planes at 0.1 and 50.
        view = anima::orthographic(float(width) / float(height), 4, .1F, 50) *
               anima::look_at(blending_test::origin, {0, 0, -1});
        renderer_.set_view(view);
        anima::Environment environment;
        environment.background = {0, 0, 0};
        renderer_.set_environment(environment);
    }
    /// The renderer, for calls that a check makes between renders.
    [[nodiscard]] anima::VulkanRenderer &renderer() noexcept { return renderer_; }
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
            require(std::chrono::steady_clock::now() - started < gpu_check::watchdog, "Sprite watchdog");
            SDL_Event event{};
            while (SDL_PollEvent(&event))
                require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                        "Sprite test interrupted");
            if (renderer_.draw())
                break;
            SDL_Delay(5);
        }
        images.add(name, gpu_check::take(renderer_));
        stats = renderer_.resource_stats();
    }
    /// Requires the pixel of capture @p name that shows @p world, in the sprite plane, to show linear @p expected,
    /// within blending_test::tolerance levels per channel of its display encoding.
    void expect(const std::string &name, float x, float y, const Color &expected, const std::string &what) const {
        const auto &image = images[name];
        const auto at = blending_test::pixel_of(view, {x, y, plane}, image);
        const auto actual = gpu_check::pixel(image, at[0], at[1]);
        bool matched = true;
        std::string wanted;
        for (std::size_t c = 0; c < expected.size(); ++c) {
            const auto target = blending_test::encoded(expected[c]);
            matched = matched && std::abs(actual[c] - target) <= blending_test::tolerance;
            wanted += (c ? " " : "") + std::to_string(target);
        }
        const auto report = what + ": pixel " + std::to_string(at[0]) + ", " + std::to_string(at[1]) + " of " + name +
                            " shows " + gpu_check::text(actual) + ", expected " + wanted + " (linear " +
                            blending_test::text(expected) + ")";
        std::cout << "SPRITES " << report << '\n';
        images.require(matched, report, {name});
    }
    /// Shuts the renderer down and requires clean validation.
    void finish() {
        const auto result = renderer_.shutdown();
        require(!result.validation_errors && !result.validation_warnings, "Sprite GPU validation failed");
    }

  private:
    static anima::RendererOptions options(std::uint32_t frames_in_flight) {
        anima::RendererOptions settings;
        settings.validation = true;
        settings.log = gpu_check::log;
        settings.frames_in_flight = frames_in_flight;
        return settings;
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

// Each quadrant of the quadrants frame, at the offset of its center from the sprite's center in the sprite's own axes,
// for a sprite of size @p size.
struct Quadrant {
    float x{}, y{};
    Color color;
};
inline std::array<Quadrant, 4> quadrants_of(float size) {
    const auto q = size / 4;
    return {{{-q, q, red}, {q, q, blue}, {-q, -q, green}, {q, -q, white}}};
}

// The frame's first row lies along the sprite's +Y edge, and its first column along its -X edge; rotation turns the
// sprite counterclockwise about its pivot, a negative size mirrors it, and the pivot is the point that position places.
inline void check_placement(Harness &harness) {
    constexpr float size = 1.2F;
    auto upright = sprite(-1.6F, .9F, quadrants, size, size);
    auto turned = sprite(0, .9F, quadrants, size, size);
    turned.rotation = std::numbers::pi_v<float> / 2;
    auto mirrored = sprite(1.6F, .9F, quadrants, -size, size);
    // Its pivot, the corner of the first column and last row, lies at (-1.6, -1.5), so it covers x from -1.6 to -0.6
    // and y from -1.5 to -0.5.
    auto pivoted = sprite(-1.6F, -1.5F, quadrants, 1, 1);
    pivoted.pivot = {0, 0};
    harness.render("placement", {{sheet(), anima::SpriteBlend::blended, {upright, turned, mirrored, pivoted}}});
    require(harness.stats.sprite_draw_calls == 1, "One batch did not draw in one call");
    require(harness.stats.cached_sprite_sheets == 1, "The drawn sheet is not cached");
    for (const auto &[x, y, color] : quadrants_of(size)) {
        harness.expect("placement", -1.6F + x, .9F + y, color, "An upright sprite's quadrant");
        // A quarter turn counterclockwise takes (x, y) to (-y, x).
        harness.expect("placement", -y, .9F + x, color, "A sprite turned a quarter counterclockwise");
        harness.expect("placement", 1.6F - x, .9F + y, color, "A sprite mirrored along X");
    }
    for (const auto &[x, y, color] : quadrants_of(1))
        harness.expect("placement", -1.1F + x, -1 + y, color, "A sprite placed by its corner");
    harness.expect("placement", -1.7F, -1, black, "Left of a sprite placed by its corner");
    harness.expect("placement", -1.1F, -1.6F, black, "Below a sprite placed by its corner");
    harness.expect("placement", -.8F, .9F, black, "Between sprites");
    harness.images.discard({"placement"});
}

// Within a batch each sprite draws over those before it, and each batch over the batches before it: blended sprites
// with the "over" operator on premultiplied color, additive ones adding their color times alpha. A sprite's color and
// alpha multiply its texels'.
inline void check_compositing(Harness &harness) {
    const auto atlas = sheet();
    const Color tint{.2, .6, .3};
    std::vector<anima::SpriteBatch> batches{
        {atlas,
         anima::SpriteBlend::blended,
         {sprite(0, 0, solid, 2, 2, blue), sprite(-.5F, 0, solid, 1, 2, red, .5F),
          sprite(-2, 1.2F, solid, .6F, .6F, tint), sprite(-2, -1.2F, faint, .6F, .6F, white, .5F)}},
        {atlas, anima::SpriteBlend::additive, {sprite(.5F, .5F, solid, 1, 1, green, .5F)}},
        {atlas, anima::SpriteBlend::blended, {sprite(.75F, .75F, solid, .5F, .5F, white, .25F)}}};
    harness.render("compositing", std::move(batches));
    require(harness.stats.sprite_draw_calls == 3, "Three batches did not draw in three calls");
    const Color added{0, .5, 1};
    harness.expect("compositing", -.5F, -.5F, over(.5, red, blue), "A blended sprite over an earlier one");
    harness.expect("compositing", .5F, -.5F, blue, "A blended sprite alone");
    harness.expect("compositing", .25F, .25F, added, "An additive sprite over an earlier batch");
    harness.expect("compositing", .75F, .75F, over(.25, white, added), "A blended batch over an additive one");
    harness.expect("compositing", -2, 1.2F, tint, "A tinted sprite");
    harness.expect("compositing", -2, -1.2F, over(faint_alpha * .5, red, black), "Texel alpha times sprite alpha");
    harness.expect("compositing", 2.2F, 0, black, "Beside every sprite");
    harness.images.discard({"compositing"});
}

// Sprites test the view's depth without writing it: an opaque quad in front of a sprite hides it, and a sprite in
// front of the quad covers it.
inline void check_depth(Harness &harness) {
    auto scene = std::make_shared<anima::Scene>();
    (void)scene->create({}, blending_test::facing(blending_test::opaque(green), {0, 0, -3}, .5F, .5F));
    auto nearer = sprite(.5F, .5F, solid, .5F, .5F, blue);
    nearer.position.z = -1;
    harness.render("depth", {{sheet(), anima::SpriteBlend::blended, {sprite(0, 0, solid, 2, 2, red), nearer}}},
                   {scene});
    harness.expect("depth", -.25F, -.25F, green, "A sprite behind an opaque quad");
    harness.expect("depth", -.75F, 0, red, "A sprite beside an opaque quad");
    harness.expect("depth", .375F, .375F, blue, "A sprite in front of an opaque quad");
    harness.expect("depth", .625F, .625F, blue, "A sprite in front of an earlier one");
    harness.images.discard({"depth"});
}

// A frame whose blended custom material reads opaque depth draws its blended draws in a second pass after copying the
// opaque targets, and its sprites after them there: over the probe, which writes no depth, though the sprite is
// farther away.
inline void check_after_opaque_inputs(Harness &harness) {
    anima::CustomMaterialDefinition definition;
    definition.name = "depth probe";
    definition.vertex_shader = custom_material_test::words(custom_material_test::surface_vertex);
    definition.fragment_shader = custom_material_test::words(custom_material_test::depth_fragment);
    definition.blend = anima::CustomBlend::blended;
    const auto probe = std::make_shared<const anima::CustomMaterial>(std::move(definition));
    auto scene = std::make_shared<anima::Scene>();
    (void)custom_material_test::add(*scene, custom_material_test::surface({0, 0, -2}, 1, 1), probe);
    harness.render("opaque-inputs", {{sheet(), anima::SpriteBlend::blended, {sprite(.5F, 0, solid, 1, 1, blue)}}},
                   {scene});
    require(harness.stats.opaque_inputs, "A frame that draws the depth probe did not copy opaque inputs");
    // The probe writes green 0.5 where no opaque surface drew.
    harness.expect("opaque-inputs", -.5F, 0, {0, .5, 0}, "The probe beside the sprite");
    harness.expect("opaque-inputs", .5F, 0, blue, "A sprite after the blended draws of a split frame");
    harness.images.discard({"opaque-inputs"});
}

// Ten thousand sprites in one batch take one draw call, and each shows its own color where it lies.
inline void check_many(Harness &harness) {
    constexpr int side = 100;
    constexpr float spacing = .036F, size = .03F, first = -spacing * (side - 1) / 2;
    const auto color_of = [](int column, int row) { return Color{column / 99., row / 99., .5}; };
    std::vector<anima::Sprite> sprites;
    for (int row = 0; row < side; ++row)
        for (int column = 0; column < side; ++column)
            sprites.push_back(sprite(first + spacing * float(column), first + spacing * float(row), solid, size, size,
                                     color_of(column, row)));
    harness.render("many", {{sheet(), anima::SpriteBlend::blended, std::move(sprites)}});
    require(harness.stats.sprite_draw_calls == 1, "Ten thousand sprites in one batch did not draw in one call");
    require(harness.stats.sprite_buffer_bytes >= std::uint64_t{side} * side * 64,
            "The sprite buffers cannot hold ten thousand sprites");
    for (const auto &[column, row] : {std::array{0, 0}, std::array{99, 99}, std::array{50, 25}, std::array{7, 93}})
        harness.expect("many", first + spacing * float(column), first + spacing * float(row), color_of(column, row),
                       "Sprite " + std::to_string(column) + ", " + std::to_string(row) + " of ten thousand");
    harness.images.discard({"many"});
}

// A rejected batch list keeps the previous sprites; the cache keeps a sheet while anything else holds it, and releases
// it once only the renderer would.
inline void check_lifetime(Harness &harness) {
    auto kept = sheet();
    harness.render("kept", {{kept, anima::SpriteBlend::blended, {sprite(0, 0, solid, 1, 1, red)}}});
    auto invalid = sprite(0, 0, solid, 1, 1);
    invalid.frame = 3;
    bool rejected = false;
    try {
        harness.renderer().set_sprites({{kept, anima::SpriteBlend::blended, {invalid}}});
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "A sprite outside its sheet's frames was accepted");
    harness.redraw("after-rejection");
    harness.expect("after-rejection", 0, 0, red, "The sprites before a rejected list");
    harness.render("cleared", {});
    require(harness.stats.sprite_draw_calls == 0, "No batches drew a sprite");
    require(harness.stats.cached_sprite_sheets == 1, "The cache released a sheet that the check still holds");
    harness.expect("cleared", 0, 0, black, "No sprites");
    kept.reset();
    harness.redraw("released");
    require(harness.stats.cached_sprite_sheets == 0, "The cache kept a sheet that only the renderer held");
    harness.images.discard({"kept", "after-rejection", "cleared", "released"});
}

inline int run(int argc, char **argv) {
    require(argc == 3 || (argc == 5 && std::string_view(argv[3]) == "--frames-in-flight"),
            "Usage: consumer --sprites OUTPUT [--frames-in-flight COUNT]");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    const auto frames =
        argc == 5 ? static_cast<std::uint32_t>(std::stoul(argv[4])) : anima::RendererOptions{}.frames_in_flight;
    Harness harness(argv[2], frames);
    check_placement(harness);
    check_compositing(harness);
    check_depth(harness);
    check_after_opaque_inputs(harness);
    check_many(harness);
    check_lifetime(harness);
    harness.finish();
    std::cout
        << "PASS sprites: placement, rotation, mirroring and pivots, tint and texel alpha, sprite and batch order "
           "with blended and additive compositing, depth against opaque meshes, drawing after a split frame's "
           "blended draws, ten thousand sprites in one call, sheet release and rejected batches, with "
        << frames << " frames in flight\n";
    return 0;
}
} // namespace sprite_test
