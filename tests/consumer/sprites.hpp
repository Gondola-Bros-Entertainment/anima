#pragma once
// Sprites against VulkanRenderer::set_sprites() and anima/sprite.hpp, through an orthographic view down -Z: placement,
// rotation, mirroring and pivots; tint and texel alpha; sprite and batch order; blended and additive compositing; depth
// against opaque meshes; drawing after a frame's blended draws when the frame copies opaque inputs; ten thousand
// sprites in one draw call; the GPU cache's release of sheets; rejected batches, which keep the previous sprites; and a
// sprite shader of the application's own, custom_sprite_runes.frag, compiled to SPIR-V by glslc in this application's
// build, whose frames match a reference that this file computes for every pixel. The sheet's texels are exact colors
// sampled with nearest filtering, so each expected pixel follows from that contract alone.
#include "blending.hpp"
#include "custom_materials.hpp"
#include "gpu_checks.hpp"
#include "rejection.hpp"
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
#include <iterator>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sprite_test {
constexpr std::uint32_t runes_fragment[] =
#include "custom_sprite_runes.frag.inc"
    ;
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
    definition.shader = custom_material_test::shader(custom_material_test::surface_vertex,
                                                     custom_material_test::depth_fragment, anima::CustomBlend::blended);
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
    require(harness.stats.sprite_buffer_bytes >= std::uint64_t{side} * side * 80,
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

// The runes of custom_sprite_runes.frag around its ring.
constexpr double rune_count = 6;
inline std::shared_ptr<const anima::SpriteShader> runes_shader() {
    return std::make_shared<const anima::SpriteShader>(
        std::vector<std::uint32_t>(std::begin(runes_fragment), std::end(runes_fragment)));
}
// The texel of @p sheet that nearest filtering samples at texture coordinates @p u and @p v: its decoded linear color,
// then its alpha.
inline std::array<double, 4> texel_at(const anima::SpriteSheet &sheet, double u, double v) {
    const auto &image = *sheet.definition().texture.image;
    const auto index = [](double coordinate, std::uint32_t texels) {
        return std::size_t(std::clamp(std::floor(coordinate * texels), 0., double(texels - 1)));
    };
    const auto *rgba = &image.rgba[(index(v, image.height) * image.width + index(u, image.width)) * 4];
    return {custom_material_test::decoded(rgba[0]), custom_material_test::decoded(rgba[1]),
            custom_material_test::decoded(rgba[2]), rgba[3] / 255.};
}
// What @p batch's shader writes for @p sprite at the point @p corner of its quad, from 0 to 1 along the quad's own
// axes from its -X, -Y corner, at shader time @p time: premultiplied color, then alpha, computed in double from the
// shader's source, as custom_sprite_runes.frag or the built-in shader states it.
inline std::array<double, 4> shaded(const anima::SpriteBatch &batch, const anima::Sprite &sprite,
                                    std::array<double, 2> corner, double time) {
    const auto &sheet = *batch.sheet;
    const auto &frame = sheet.definition().frames[sprite.frame];
    const auto &image = *sheet.definition().texture.image;
    // As sprite.vert: the frame's first row lies along the quad's +Y edge.
    const double first_u = double(frame.x) / image.width, first_v = double(frame.y) / image.height,
                 last_u = (double(frame.x) + frame.width) / image.width,
                 last_v = (double(frame.y) + frame.height) / image.height;
    const auto texel =
        texel_at(sheet, first_u + (last_u - first_u) * corner[0], last_v + (first_v - last_v) * corner[1]);
    double alpha = texel[3] * sprite.alpha;
    if (batch.shader) {
        const auto &ring = sprite.parameters;
        const double x = corner[0] * 2 - 1, y = (1 - corner[1]) * 2 - 1;
        const double band = std::clamp(1 - std::abs(std::hypot(x, y) - ring[0]) / ring[1], 0., 1.);
        const double angle = std::atan2(y, x) + double(ring[2]) * time + ring[3];
        alpha *= band * (.5 + .5 * std::cos(rune_count * angle));
    }
    return {texel[0] * sprite.color.x * alpha, texel[1] * sprite.color.y * alpha, texel[2] * sprite.color.z * alpha,
            alpha};
}
// How capture @p name, of @p batches over @p backdrop at shader time @p time, compares with the reference.
struct Comparison {
    std::size_t compared{}, shaded{}, mismatched{};
    double largest{};
};
// Compares every pixel of capture @p name with the reference: the backdrop's linear color, then each sprite of each
// batch, in order, composited as its batch's blend says. Pixels within 1.5 pixels of an edge of a texel of a covering
// sprite's frame, the quad's own edges included, are left out, since there the GPU's sampling and coverage may round
// either way.
inline Comparison compare(const Harness &harness, const std::string &name,
                          const std::vector<anima::SpriteBatch> &batches, const Color &backdrop, double time) {
    const auto &image = harness.images[name];
    const auto inverse = anima::inverse(harness.view);
    // The view is 4 units high.
    const double pixels_per_unit = image.height / 4.;
    constexpr double margin = 1.5;
    Comparison result;
    for (std::size_t py = 0; py < image.height; ++py)
        for (std::size_t px = 0; px < image.width; ++px) {
            const double ndc_x = (double(px) + .5) / image.width * 2 - 1,
                         ndc_y = (double(py) + .5) / image.height * 2 - 1;
            const auto world = [&](unsigned row) {
                return double(inverse[row]) * ndc_x + double(inverse[4 + row]) * ndc_y + double(inverse[8 + row]) * .5 +
                       double(inverse[12 + row]);
            };
            const double w = world(3), wx = world(0) / w, wy = world(1) / w;
            auto color = backdrop;
            bool covered = false, ambiguous = false, lit = false;
            for (const auto &batch : batches)
                for (const auto &sprite : batch.sprites) {
                    const double c = std::cos(double(sprite.rotation)), s = std::sin(double(sprite.rotation));
                    const double dx = wx - sprite.position.x, dy = wy - sprite.position.y;
                    const std::array<double, 2> corner{(c * dx + s * dy) / sprite.size.x + sprite.pivot.x,
                                                       (c * dy - s * dx) / sprite.size.y + sprite.pivot.y};
                    const auto &frame = batch.sheet->definition().frames[sprite.frame];
                    const std::array<double, 2> texels{double(frame.width), double(frame.height)},
                        extent{std::abs(sprite.size.x) * pixels_per_unit, std::abs(sprite.size.y) * pixels_per_unit};
                    // Only points within the margin of the quad can be ambiguous.
                    bool near = true;
                    for (std::size_t axis = 0; axis < 2; ++axis)
                        near =
                            near && corner[axis] > -margin / extent[axis] && corner[axis] < 1 + margin / extent[axis];
                    if (!near)
                        continue;
                    for (std::size_t axis = 0; axis < 2; ++axis) {
                        const double along = corner[axis] * texels[axis];
                        const double nearest = std::clamp(std::round(along), 0., texels[axis]);
                        ambiguous = ambiguous || std::abs(along - nearest) / texels[axis] * extent[axis] < margin;
                    }
                    if (corner[0] < 0 || corner[0] > 1 || corner[1] < 0 || corner[1] > 1)
                        continue;
                    covered = true;
                    const auto value = shaded(batch, sprite, corner, time);
                    lit = lit || value[3] > 0;
                    for (std::size_t channel = 0; channel < color.size(); ++channel)
                        color[channel] =
                            value[channel] +
                            color[channel] * (batch.blend == anima::SpriteBlend::additive ? 1 : 1 - value[3]);
                }
            if (!covered || ambiguous)
                continue;
            ++result.compared;
            result.shaded += lit;
            const auto actual = gpu_check::pixel(image, px, py);
            double difference = 0;
            for (std::size_t channel = 0; channel < color.size(); ++channel)
                difference = std::max(difference, std::abs(actual[channel] - blending_test::encoded(color[channel])));
            result.largest = std::max(result.largest, difference);
            result.mismatched += difference > blending_test::tolerance;
        }
    return result;
}

// A batch drawn with the application's sprite shader matches the reference in every pixel that its sprites cover: three
// additive sprites of the shader, a blended batch of the built-in shader, then two blended sprites of the shader, each
// of the shader's five sprites with its own color, size, rotation, mirroring and parameters, which reach the shader
// without a draw call per sprite. At two shader times the runes turn and both frames match. A custom material's
// fragment shader is refused as a sprite shader, and the GPU cache keeps the shader while the check holds it and
// releases it after.
inline void check_shader(Harness &harness) {
    rejection::rejects<std::invalid_argument>(
        [] {
            (void)anima::SpriteShader(std::vector<std::uint32_t>(std::begin(custom_material_test::effect_fragment),
                                                                 std::end(custom_material_test::effect_fragment)));
        },
        "The sprite shader declares set 2 binding 0, which the sprite shader interface does not provide");
    auto shader = runes_shader();
    const auto atlas = sheet();
    // Each sprite's ring: radius and half width as fractions of half its size, then turn and phase.
    const auto rune = [](float x, float y, float width, float height, float rotation, const Color &color, float alpha,
                         std::array<float, 4> ring) {
        auto result = sprite(x, y, quadrants, width, height, color, alpha);
        result.rotation = rotation;
        result.parameters = ring;
        return result;
    };
    std::vector<anima::SpriteBatch> batches{
        {atlas,
         anima::SpriteBlend::additive,
         {rune(-1.85F, .95F, 1.3F, 1.3F, 0, {1, .8, .5}, 1, {.7F, .2F, 1, 0}),
          rune(0, .95F, 1.2F, -1.2F, .5F, {.4, 1, .6}, .8F, {.55F, .3F, -2, 1}),
          rune(1.8F, .95F, 1, 1.3F, -1.2F, {.6, .7, 1}, 1, {.8F, .15F, .5F, 2})},
         shader},
        {atlas, anima::SpriteBlend::blended, {sprite(0, -1.05F, quadrants, 1, 1, white, .7F)}},
        {atlas,
         anima::SpriteBlend::blended,
         {rune(-1.85F, -1.05F, 1.3F, 1.3F, .3F, white, .9F, {.6F, .25F, 3, 0}),
          rune(1.85F, -1.05F, -1.3F, 1.3F, 0, {1, .5, .9}, 1, {.45F, .4F, 0, .5F})},
         shader}};
    const Color backdrop{.05, .1, .15};
    auto scene = std::make_shared<anima::Scene>();
    (void)scene->create({}, blending_test::facing(blending_test::opaque(backdrop), {0, 0, plane - 1}, 4, 3));
    for (const float time : {0.F, .7F}) {
        const auto name = "shader-" + std::to_string(time);
        harness.renderer().set_time(time);
        harness.render(name, batches, {scene});
        require(harness.stats.sprite_draw_calls == batches.size(),
                "A batch of a sprite shader took more than one call");
        require(harness.stats.cached_sprite_shaders == 1, "Two batches of one sprite shader did not share it");
        const auto result = compare(harness, name, batches, backdrop, time);
        const auto report = name + ": " + std::to_string(result.mismatched) + " of " + std::to_string(result.compared) +
                            " covered pixels, " + std::to_string(result.shaded) +
                            " of them shaded, differ from the reference by more than " +
                            std::to_string(blending_test::tolerance) + " levels; the largest difference is " +
                            std::to_string(result.largest);
        std::cout << "SPRITES " << report << '\n';
        harness.images.require(result.compared > 0 && result.shaded > result.compared / 4 && result.mismatched == 0,
                               "A sprite shader's frame differs from the reference: " + report, {name});
    }
    harness.images.require_changed("shader-0.000000", "shader-0.700000", .01, "The runes did not turn with time");
    harness.renderer().set_time(0);
    batches.clear();
    harness.render("shader-cleared", {});
    require(harness.stats.cached_sprite_shaders == 1, "The cache released a sprite shader that the check still holds");
    shader.reset();
    harness.redraw("shader-released");
    require(harness.stats.cached_sprite_shaders == 0, "The cache kept a sprite shader that only the renderer held");
    harness.images.discard({"shader-0.000000", "shader-0.700000", "shader-cleared", "shader-released"});
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
    check_shader(harness);
    harness.finish();
    std::cout
        << "PASS sprites: placement, rotation, mirroring and pivots, tint and texel alpha, sprite and batch order "
           "with blended and additive compositing, depth against opaque meshes, drawing after a split frame's "
           "blended draws, ten thousand sprites in one call, sheet release, rejected batches and a sprite shader "
           "that matches its reference with per-sprite parameters, with "
        << frames << " frames in flight\n";
    return 0;
}
} // namespace sprite_test
