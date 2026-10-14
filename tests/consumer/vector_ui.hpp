#pragma once
// Vector art on a device through anima::vector_ui. A VectorAtlas's frames draw as sprites one texel to a framebuffer
// pixel at the display scale and, after set_scale(), at one and a half times it, and the renderer releases the
// replaced sheet. Magnified and a quarter pixel off the pixel grid over white, a one-color drawing's edges blend
// toward white through its own color alone: they neither darken, as transparent black texels would make them, nor take
// the color of the frame beside it in the sheet. The same drawings registered with set_ui_images() measure their size
// in dp in a document and draw one texel to a framebuffer pixel. Each expected pixel follows from the texels of the
// sheet or of render_vector_art() and the documented compositing alone.
#include "blending.hpp"
#include "gpu_checks.hpp"
#include "vector.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <anima/sprite.hpp>
#include <anima/vector_ui.hpp>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vector_ui_test {
using blending_test::Color;
// Sprites lie in this plane, between the view's near and far planes.
constexpr float plane = -5;
// Largest difference, in display levels, between a UI pixel of partial alpha and its expected value; opaque and
// transparent ones match exactly, as in the UI check.
constexpr int ui_alpha_tolerance = 3;

inline void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
// The linear light of the 8-bit sRGB value @p encoded.
inline double linear(double encoded) {
    encoded /= 255;
    return encoded <= .04045 ? encoded / 12.92 : std::pow((encoded + .055) / 1.055, 2.4);
}
using Texel = std::array<std::uint8_t, 4>;
inline Texel texel_of(const anima::Image &image, std::uint32_t x, std::uint32_t y) {
    const auto *p = &image.rgba[(std::size_t{y} * image.width + x) * 4];
    return {p[0], p[1], p[2], p[3]};
}

// Draws through a view of one world unit to a framebuffer pixel and keeps each frame read back by name.
class Harness {
  public:
    explicit Harness(const std::filesystem::path &output)
        : window_(gpu_check::window("Anima vector art verification", 640, 480, SDL_WINDOW_HIGH_PIXEL_DENSITY)),
          renderer_(window_.get(), options()), images(output) {
        require(SDL_GetWindowSizeInPixels(window_.get(), &width, &height) && width > 0 && height > 0,
                "The vector art window has no drawable size");
        view = anima::orthographic(float(width) / float(height), float(height), .1F, 50) *
               anima::look_at(blending_test::origin, {0, 0, -1});
        renderer_.set_view(view);
        set_background({0, 0, 0});
    }
    [[nodiscard]] SDL_Window *window() const noexcept { return window_.get(); }
    [[nodiscard]] anima::VulkanRenderer &renderer() noexcept { return renderer_; }
    /// Clears to linear @p color behind everything drawn.
    void set_background(const Color &color) {
        anima::Environment environment;
        environment.background = {float(color[0]), float(color[1]), float(color[2])};
        renderer_.set_environment(environment);
        background = color;
    }
    /// A sprite of @p frame of @p sheet, @p magnification times its texels' size, whose top left corner lies at
    /// framebuffer coordinates (@p x, @p y), from the top left of the frame.
    [[nodiscard]] anima::Sprite sprite(const anima::SpriteSheet &sheet, std::uint32_t frame, double x, double y,
                                       double magnification = 1) const {
        const auto &rectangle = sheet.definition().frames.at(frame);
        const double w = rectangle.width * magnification, h = rectangle.height * magnification;
        anima::Sprite result;
        result.position = {float(x - width / 2.), float(height / 2. - (y + h)), plane};
        result.size = {float(w), float(h)};
        result.pivot = {0, 0};
        result.frame = frame;
        return result;
    }
    /// Draws @p batches and reads the frame back as @p name.
    void render(const std::string &name, std::vector<anima::SpriteBatch> batches) {
        renderer_.set_sprites(std::move(batches));
        renderer_.request_capture();
        const auto started = std::chrono::steady_clock::now();
        for (;;) {
            require(std::chrono::steady_clock::now() - started < gpu_check::watchdog, "Vector art watchdog");
            pump();
            if (renderer_.draw())
                break;
            SDL_Delay(5);
        }
        images.add(name, gpu_check::take(renderer_));
        stats = renderer_.resource_stats();
    }
    /// Fails on a request to quit, which would end the check early.
    static void pump() {
        SDL_Event event{};
        while (SDL_PollEvent(&event))
            require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                    "Vector art check interrupted");
    }
    /// The capture pixel at framebuffer coordinates (@p x, @p y), through the view as the renderer draws it.
    [[nodiscard]] gpu_check::Rgb at(const std::string &name, std::uint32_t x, std::uint32_t y) const {
        const auto &image = images[name];
        const anima::Vec3 world{float(x + .5 - width / 2.), float(height / 2. - (y + .5)), plane};
        const auto pixel = blending_test::pixel_of(view, world, image);
        return gpu_check::pixel(image, pixel[0], pixel[1]);
    }
    /// Shuts the renderer down and requires clean validation.
    void finish() {
        const auto result = renderer_.shutdown();
        require(!result.validation_errors && !result.validation_warnings, "Vector art GPU validation failed");
    }

  private:
    static anima::RendererOptions options() {
        anima::RendererOptions settings;
        settings.validation = true;
        settings.log = gpu_check::log;
        return settings;
    }
    gpu_check::Video video_;
    gpu_check::Window window_;
    anima::VulkanRenderer renderer_;

  public:
    int width = 0, height = 0;
    anima::Mat4 view{};
    Color background{};
    gpu_check::Captures images;
    anima::ResourceStats stats{};
};

// Draws every frame of @p atlas's sheet one texel to a pixel in a row from (16, 16), and requires each pixel of each to
// show the texel of its art that render_vector_art() renders at the atlas's scale composited over the background,
// within blending_test::tolerance levels: the frame holds the art upright, and the sprite draws it texel for texel.
inline void check_frames(Harness &harness, const anima::VectorAtlas &atlas, const std::string &name) {
    const auto &sheet = *atlas.sheet();
    const auto &frames = sheet.definition().frames;
    std::vector<anima::Sprite> sprites;
    std::vector<std::uint32_t> left;
    std::uint32_t x = 16;
    constexpr std::uint32_t top = 16, spacing = 8;
    for (std::uint32_t frame = 0; frame < frames.size(); ++frame) {
        left.push_back(x);
        sprites.push_back(harness.sprite(sheet, frame, x, top));
        x += frames[frame].width + spacing;
    }
    harness.render(name, {{atlas.sheet(), anima::SpriteBlend::blended, std::move(sprites)}});
    for (std::uint32_t frame = 0; frame < frames.size(); ++frame) {
        const auto &rectangle = frames[frame];
        const auto art = anima::render_vector_art(atlas.art()[frame], atlas.scale());
        require(art.width == rectangle.width && art.height == rectangle.height,
                "Frame " + std::to_string(frame) + " is not its art's size at the atlas's scale");
        for (std::uint32_t ty = 0; ty < rectangle.height; ++ty)
            for (std::uint32_t tx = 0; tx < rectangle.width; ++tx) {
                const auto texel = texel_of(art, tx, ty);
                const double alpha = texel[3] / 255.;
                const auto actual = harness.at(name, left[frame] + tx, top + ty);
                bool matched = true;
                std::string wanted;
                for (std::size_t c = 0; c < 3; ++c) {
                    const auto target =
                        blending_test::encoded(linear(texel[c]) * alpha + harness.background[c] * (1 - alpha));
                    matched = matched && std::abs(actual[c] - target) <= blending_test::tolerance;
                    wanted += (c ? " " : "") + std::to_string(target);
                }
                harness.images.require(matched,
                                       "Frame " + std::to_string(frame) + " texel " + std::to_string(tx) + ", " +
                                           std::to_string(ty) + " of " + name + " shows " + gpu_check::text(actual) +
                                           ", expected " + wanted,
                                       {name});
            }
        // The pixels just outside the sprite show the background.
        const auto outside = harness.at(name, left[frame] + rectangle.width, top);
        harness.images.require(gpu_check::difference(outside, {0, 0, 0}) <= blending_test::tolerance,
                               "Frame " + std::to_string(frame) + " of " + name + " drew past its right edge", {name});
    }
}

// The coin and the gem, each of one color, magnified one and a half times a quarter pixel off the grid over white:
// each pixel keeps the full value of each channel that the art's color has full, and equal values of the channels that
// it lacks, which is the art's color over white at some alpha. A dark fringe lowers the full channel, and the color of
// a neighboring frame unbalances the others.
inline void check_edges(Harness &harness, const anima::VectorAtlas &atlas) {
    const auto &sheet = *atlas.sheet();
    const auto &frames = sheet.definition().frames;
    const auto coin = atlas.frame("art/coin"), gem = atlas.frame("art/gem");
    // The check means something only where the two frames lie side by side in the sheet, their borders between them.
    const auto &a = frames[coin], &b = frames[gem];
    const auto apart = [](std::uint32_t first, std::uint32_t length, std::uint32_t other, std::uint32_t other_length) {
        constexpr auto gap = 2 * anima::VectorAtlas::padding;
        return std::uint64_t{first} + length + gap == other || std::uint64_t{other} + other_length + gap == first;
    };
    const auto overlapping = [](std::uint32_t first, std::uint32_t length, std::uint32_t other,
                                std::uint32_t other_length) {
        return std::uint64_t{first} < std::uint64_t{other} + other_length &&
               std::uint64_t{other} < std::uint64_t{first} + length;
    };
    require((apart(a.x, a.width, b.x, b.width) && overlapping(a.y, a.height, b.y, b.height)) ||
                (apart(a.y, a.height, b.y, b.height) && overlapping(a.x, a.width, b.x, b.width)),
            "The coin and the gem do not lie two borders apart in the sheet");
    harness.set_background({1, 1, 1});
    constexpr double magnification = 1.5, offset = .25;
    constexpr std::uint32_t top = 16;
    const std::array<std::pair<std::uint32_t, std::array<bool, 3>>, 2> drawn{
        {{coin, {true, false, false}}, {gem, {false, false, true}}}};
    std::vector<anima::Sprite> sprites;
    std::vector<std::uint32_t> left;
    std::uint32_t x = 16;
    for (const auto &[frame, full] : drawn) {
        left.push_back(x);
        sprites.push_back(harness.sprite(sheet, frame, x + offset, top + offset, magnification));
        x += std::uint32_t(std::ceil(frames[frame].width * magnification)) + 16;
    }
    harness.render("edges", {{atlas.sheet(), anima::SpriteBlend::blended, std::move(sprites)}});
    for (std::size_t i = 0; i < drawn.size(); ++i) {
        const auto &[frame, full] = drawn[i];
        const auto right = left[i] + std::uint32_t(std::ceil(frames[frame].width * magnification)) + 2,
                   bottom = top + std::uint32_t(std::ceil(frames[frame].height * magnification)) + 2;
        std::size_t covered = 0;
        for (auto y = top - 2; y < bottom; ++y)
            for (auto px = left[i] - 2; px < right; ++px) {
                const auto actual = harness.at("edges", px, y);
                int lacking = -1;
                bool balanced = true, bright = true;
                for (std::size_t c = 0; c < 3; ++c) {
                    if (full[c])
                        bright = bright && actual[c] >= 255 - int(blending_test::tolerance);
                    else if (lacking < 0)
                        lacking = actual[c];
                    else
                        balanced = balanced && std::abs(actual[c] - lacking) <= int(blending_test::tolerance);
                }
                // Mostly the art's color: alpha over about 0.8.
                if (lacking < 128)
                    ++covered;
                harness.images.require(bright && balanced,
                                       "Pixel " + std::to_string(px) + ", " + std::to_string(y) +
                                           " at the edge of frame " + std::to_string(frame) + " shows " +
                                           gpu_check::text(actual) + ", which is not the art's color over white",
                                       {"edges"});
            }
        harness.images.require(covered > 0, "Frame " + std::to_string(frame) + " drew nothing opaque", {"edges"});
    }
    harness.set_background({0, 0, 0});
}

// The drawings as UI images: each img element, sized by its image, measures VectorArt::size in dp, and where that is a
// whole number of framebuffer pixels, as at the display scales 1 and 2 of CI and of high-density displays, it shows
// render_vector_art()'s texels one to a pixel, premultiplied and blended over the background in linear light as the UI
// shader does.
inline void check_ui(Harness &harness, const anima::VectorAtlas &atlas) {
    harness.renderer().set_sprites({});
    anima::UiContext ui(harness.window(), harness.renderer());
    const float scale = ui.context().GetDensityIndependentPixelRatio();
    require(scale == SDL_GetWindowDisplayScale(harness.window()), "The UI's dp ratio is not the display scale");
    anima::set_ui_images(ui, atlas);
    std::string body;
    std::vector<std::uint32_t> left;
    std::uint32_t x = 16;
    constexpr std::uint32_t top = 16;
    for (const auto &art : atlas.art()) {
        left.push_back(x);
        body += "<img id='" + art.name + "' src='memory:" + art.name +
                "' style='position: absolute; left: " + std::to_string(x) + "px; top: " + std::to_string(top) +
                "px;'/>";
        x += std::uint32_t(std::ceil(art.size.x * scale)) + 8;
    }
    auto document = ui.documents().from_memory("<rml><head></head><body>" + body + "</body></rml>");
    require(document.valid(), "The vector art document did not load");
    document.show();
    harness.renderer().request_capture();
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        require(std::chrono::steady_clock::now() - started < gpu_check::watchdog, "Vector art UI watchdog");
        harness.renderer().wait_for_frame();
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                    "Vector art check interrupted");
            (void)ui.process_event(event);
        }
        ui.update();
        if (ui.render())
            break;
        SDL_Delay(5);
    }
    harness.images.add("ui", gpu_check::take(harness.renderer()));
    const auto &shot = harness.images["ui"];
    // Far from every image.
    const auto background = gpu_check::pixel(shot, shot.width - 1, shot.height - 1);
    std::size_t exact = 0;
    for (std::size_t i = 0; i < atlas.art().size(); ++i) {
        const auto &art = atlas.art()[i];
        auto *element = document.native().GetElementById(art.name);
        require(element != nullptr, "The vector art document lost an image");
        const auto box = element->GetBox().GetSize();
        const auto pixels = anima::render_vector_art(art, scale);
        const float measured_x = std::max(1.F, std::round(float(pixels.width) / scale)),
                    measured_y = std::max(1.F, std::round(float(pixels.height) / scale));
        require(box.x == measured_x * scale && box.y == measured_y * scale,
                art.name + " measures " + std::to_string(box.x) + " by " + std::to_string(box.y) +
                    " framebuffer pixels, not its size in dp");
        if (box.x != float(pixels.width) || box.y != float(pixels.height))
            continue;
        ++exact;
        for (std::uint32_t ty = 0; ty < pixels.height; ++ty)
            for (std::uint32_t tx = 0; tx < pixels.width; ++tx) {
                const auto texel = texel_of(pixels, tx, ty);
                const double alpha = texel[3] / 255.;
                gpu_check::Rgb expected = background;
                if (texel[3])
                    for (std::size_t c = 0; c < 3; ++c) {
                        const auto premultiplied = (texel[c] * texel[3] + 127) / 255;
                        expected[c] = int(
                            std::lround(blending_test::encoded(linear(double(premultiplied) * 255 / texel[3]) * alpha +
                                                               linear(background[c]) * (1 - alpha))));
                    }
                const int tolerance = texel[3] == 0 || texel[3] == 255 ? 0 : ui_alpha_tolerance;
                const auto actual = gpu_check::pixel(shot, left[i] + tx, top + ty);
                harness.images.require(gpu_check::difference(actual, expected) <= tolerance,
                                       art.name + " texel " + std::to_string(tx) + ", " + std::to_string(ty) +
                                           " shows " + gpu_check::text(actual) + " instead of " +
                                           gpu_check::text(expected),
                                       {"ui"});
            }
    }
    std::cout << "VECTOR UI scale " << scale << ": " << exact << " of " << atlas.art().size()
              << " images compared texel by texel\n";
    require(ui.stats().log_warnings == 0 && ui.stats().log_errors == 0, "The vector art document logged warnings");
    ui.shutdown();
}

inline int run(int argc, char **argv) {
    require(argc == 3, "Usage: consumer --vector OUTPUT");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    Harness harness(argv[2]);
    const float display = SDL_GetWindowDisplayScale(harness.window());
    require(std::isfinite(display) && display > 0, "SDL reports no display scale");
    anima::VectorAtlas atlas(vector_consumer::art(), display);
    check_frames(harness, atlas, "display-scale");
    // A new scale renders a new sheet; batches built again from it draw the same frames sharp at their new size, and
    // the renderer releases the previous sheet once nothing else holds it.
    auto previous = atlas.sheet();
    atlas.set_scale(display * 1.5F);
    require(atlas.sheet() != previous, "A new scale did not render a new sheet");
    previous.reset();
    check_frames(harness, atlas, "larger-scale");
    harness.render("larger-scale-again", {{atlas.sheet(), anima::SpriteBlend::blended, {}}});
    require(harness.stats.cached_sprite_sheets == 1, "The renderer kept the replaced sheet");
    check_edges(harness, atlas);
    harness.images.discard({"display-scale", "larger-scale", "larger-scale-again", "edges"});
    check_ui(harness, atlas);
    harness.finish();
    std::cout << "PASS vector art: frames one texel to a pixel at display scale " << display << " and at "
              << atlas.scale() << ", the replaced sheet released, edges without fringes or bleeding, and UI images "
              << "measuring their size in dp\n";
    return 0;
}
} // namespace vector_ui_test
