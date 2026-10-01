#pragma once
// Blended materials against the renderer's documented compositing: the "over" operator on premultiplied linear
// color after every opaque and masked draw, fog at each surface's own distance and exposure after compositing,
// back-to-front order within and across scenes in perspective and orthographic views, intersecting blended
// surfaces, blended surfaces behind opaque ones, and the shadow policy: blended surfaces cast no shadows and receive
// them. Most quads are unlit, whose output is their base color, so each expected pixel follows from that contract
// alone.
#include "gpu_checks.hpp"
#include <algorithm>
#include <anima/assets/asset.hpp>
#include <anima/scene.hpp>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace blending_test {
inline void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
using Color = std::array<double, 3>;
constexpr Color black{0, 0, 0}, red{1, 0, 0}, green{0, 1, 0}, blue{0, 0, 1};
// Largest difference, in display levels, between a pixel and the display encoding of its expected linear color.
constexpr double tolerance = 2;

// @p alpha * @p source + (1 - @p alpha) * @p destination, the "over" operator on straight colors.
inline Color over(double alpha, const Color &source, const Color &destination) {
    Color result{};
    for (std::size_t c = 0; c < result.size(); ++c)
        result[c] = alpha * source[c] + (1 - alpha) * destination[c];
    return result;
}
// The display encoding of linear @p value, before rounding to a level.
inline double encoded(double value) {
    value = std::clamp(value, 0., 1.);
    return 255 * (value <= .0031308 ? 12.92 * value : 1.055 * std::pow(value, 1 / 2.4) - .055);
}
inline std::string text(const Color &color) {
    return std::to_string(color[0]) + " " + std::to_string(color[1]) + " " + std::to_string(color[2]);
}

// A material of linear @p color and @p alpha, unlit unless @p lit.
inline anima::Material material(const Color &color, float alpha, anima::AlphaMode mode, bool lit = false) {
    anima::Material result;
    result.name = mode == anima::AlphaMode::blend ? "blended" : mode == anima::AlphaMode::mask ? "masked" : "opaque";
    result.factor = {float(color[0]), float(color[1]), float(color[2])};
    result.alpha = alpha;
    result.alpha_mode = mode;
    result.unlit = !lit;
    return result;
}
inline anima::Material blended(const Color &color, float alpha, bool lit = false) {
    return material(color, alpha, anima::AlphaMode::blend, lit);
}
inline anima::Material opaque(const Color &color, bool lit = false) {
    return material(color, 1, anima::AlphaMode::opaque, lit);
}
// A mesh of one quad of @p surface centered on @p center that spans @p u and @p v either way, facing cross(u, v).
inline std::shared_ptr<const anima::Mesh> quad(const anima::Material &surface, anima::Vec3 center, anima::Vec3 u,
                                               anima::Vec3 v) {
    anima::Asset asset;
    asset.nodes.resize(1);
    asset.materials.push_back(surface);
    anima::SourcePrimitive primitive;
    primitive.material = 0;
    const std::array corners{center - u - v, center + u - v, center + u + v, center - u + v};
    for (const auto corner : {0U, 1U, 2U, 0U, 2U, 3U}) {
        anima::SourceVertex vertex;
        vertex.position = corners[corner];
        vertex.normal = anima::normalized(anima::cross(u, v));
        primitive.vertices.push_back(vertex);
    }
    asset.primitives.push_back(std::move(primitive));
    return anima::Mesh::compile(asset);
}
// A quad facing +Z, toward a camera looking down -Z, with half extents @p half_width and @p half_height.
inline std::shared_ptr<const anima::Mesh> facing(const anima::Material &surface, anima::Vec3 center, float half_width,
                                                 float half_height) {
    return quad(surface, center, {half_width, 0, 0}, {0, half_height, 0});
}
// A quad facing +Y with half extents @p half_x and @p half_z.
inline std::shared_ptr<const anima::Mesh> horizontal(const anima::Material &surface, anima::Vec3 center, float half_x,
                                                     float half_z) {
    return quad(surface, center, {0, 0, half_z}, {half_x, 0, 0});
}

using Pixel = std::array<std::size_t, 2>;
// The pixel of @p image that shows @p world through @p view_projection.
inline Pixel pixel_of(const anima::Mat4 &view_projection, anima::Vec3 world, const gpu_check::Image &image) {
    const auto clip = [&](unsigned row) {
        return double(view_projection[row]) * world.x + double(view_projection[4 + row]) * world.y +
               double(view_projection[8 + row]) * world.z + double(view_projection[12 + row]);
    };
    const auto w = clip(3);
    const auto x = std::floor((clip(0) / w + 1) / 2 * image.width),
               y = std::floor((clip(1) / w + 1) / 2 * image.height);
    require(x >= 0 && y >= 0 && x < image.width && y < image.height, "A sample point lies outside the capture");
    return {std::size_t(x), std::size_t(y)};
}
// Distance from @p eye along the ray through the center of @p pixel to the plane z = @p plane, for the inverse of the
// view-projection that @p image was drawn through.
inline double distance_to_plane(const anima::Mat4 &inverse, anima::Vec3 eye, Pixel pixel, const gpu_check::Image &image,
                                double plane) {
    const auto ndc_x = (double(pixel[0]) + .5) / image.width * 2 - 1,
               ndc_y = (double(pixel[1]) + .5) / image.height * 2 - 1;
    const auto unproject = [&](double depth) {
        std::array<double, 4> result{};
        for (unsigned row = 0; row < 4; ++row)
            result[row] = double(inverse[row]) * ndc_x + double(inverse[4 + row]) * ndc_y +
                          double(inverse[8 + row]) * depth + double(inverse[12 + row]);
        return std::array<double, 3>{result[0] / result[3], result[1] / result[3], result[2] / result[3]};
    };
    const auto near_point = unproject(1), far_point = unproject(0); // Reversed depth.
    const auto t = (plane - near_point[2]) / (far_point[2] - near_point[2]);
    double square = 0;
    const std::array<double, 3> from{eye.x, eye.y, eye.z};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto offset = near_point[axis] + (far_point[axis] - near_point[axis]) * t - from[axis];
        square += offset * offset;
    }
    return std::sqrt(square);
}

// Draws selections in one window and keeps each frame read back by name.
class Harness {
  public:
    explicit Harness(const std::filesystem::path &output)
        : window_(gpu_check::window("Anima blending verification", 800, 600)), renderer_(window_.get(), options()),
          images(output) {}
    /// Width over height of the window's pixels.
    [[nodiscard]] float aspect() const {
        int width = 0, height = 0;
        require(SDL_GetWindowSizeInPixels(window_.get(), &width, &height) && width > 0 && height > 0,
                "Blending window has no drawable size");
        return float(width) / float(height);
    }
    /// Draws @p scenes through @p view_projection in @p environment and reads the frame back as @p name.
    void render(const std::string &name, std::vector<std::shared_ptr<const anima::Scene>> scenes,
                const anima::Mat4 &view_projection, const anima::Environment &environment) {
        renderer_.set_environment(environment);
        renderer_.set_scenes(std::move(scenes));
        renderer_.set_view(view_projection);
        renderer_.request_capture();
        const auto started = std::chrono::steady_clock::now();
        for (;;) {
            require(std::chrono::steady_clock::now() - started < gpu_check::watchdog, "Blending watchdog");
            SDL_Event event{};
            while (SDL_PollEvent(&event))
                require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                        "Blending test interrupted");
            if (renderer_.draw())
                break;
            SDL_Delay(5);
        }
        images.add(name, gpu_check::take(renderer_));
        stats = renderer_.resource_stats();
    }
    /// Requires the pixel of capture @p name that shows @p world through @p view_projection to show linear
    /// @p expected, within `tolerance` levels per channel of its display encoding.
    void expect(const std::string &name, const anima::Mat4 &view_projection, anima::Vec3 world, const Color &expected,
                const std::string &what) const {
        const auto &image = images[name];
        const auto at = pixel_of(view_projection, world, image);
        const auto actual = gpu_check::pixel(image, at[0], at[1]);
        bool matched = true;
        std::string wanted;
        for (std::size_t c = 0; c < expected.size(); ++c) {
            const auto target = encoded(expected[c]);
            matched = matched && std::abs(actual[c] - target) <= tolerance;
            wanted += (c ? " " : "") + std::to_string(target);
        }
        const auto report = what + ": pixel " + std::to_string(at[0]) + ", " + std::to_string(at[1]) + " of " + name +
                            " shows " + gpu_check::text(actual) + ", expected " + wanted + " (linear " +
                            text(expected) + ")";
        std::cout << "BLENDING " << report << '\n';
        images.require(matched, report, {name});
    }
    /// Shuts the renderer down and requires clean validation.
    void finish() {
        const auto result = renderer_.shutdown();
        require(!result.validation_errors && !result.validation_warnings, "Blending GPU validation failed");
    }

  private:
    static anima::RendererOptions options() {
        anima::RendererOptions settings;
        settings.validation = true;
        return settings;
    }
    gpu_check::Video video_;
    gpu_check::Window window_;
    anima::VulkanRenderer renderer_;

  public:
    gpu_check::Captures images;
    /// Resource counters of the latest render().
    anima::ResourceStats stats{};
};

// A camera at the origin looking down -Z.
constexpr anima::Vec3 origin{0, 0, 0};
inline anima::Mat4 perspective_view(float aspect) {
    using anima::operator*;
    return anima::perspective(aspect, .1F, 50) * anima::look_at(origin, {0, 0, -1});
}
// An orthographic camera at the origin looking down -Z, 4 units high, with reversed Vulkan depth from 1 at 0.1 to 0
// at 50.
inline anima::Mat4 orthographic_view(float aspect) {
    using anima::operator*;
    constexpr float half_height = 2, near_plane = .1F, far_plane = 50;
    auto projection = anima::identity();
    projection[0] = 1 / (half_height * aspect);
    projection[5] = -1 / half_height; // Vulkan clip space points Y down.
    projection[10] = 1 / (far_plane - near_plane);
    projection[14] = far_plane / (far_plane - near_plane);
    return projection * anima::look_at(origin, {0, 0, -1});
}

// Blended quads at z = -2 over opaque and masked backdrops at z = -4, each blended quad created before its backdrop,
// so only the pass order puts it in front: white at alpha 0.5 over black, a color over another at 0.5, and a color at
// 0.25 over a masked backdrop.
struct Composite {
    anima::Vec3 front, back;
    Color source;
    float alpha;
    Color destination;
};
inline std::array<Composite, 3> composites() {
    return {{{{-.8F, 0, -2}, {-1.6F, 0, -4}, {1, 1, 1}, .5F, black},
             {{0, 0, -2}, {0, 0, -4}, {.7, .25, .05}, .5F, {.05, .35, .6}},
             {{.8F, 0, -2}, {1.6F, 0, -4}, {.1, .8, .4}, .25F, {.6, .1, .3}}}};
}
inline void check_compositing(Harness &harness) {
    const auto view = perspective_view(harness.aspect());
    auto scene = std::make_shared<anima::Scene>();
    const auto cases = composites();
    for (const auto &c : cases)
        (void)scene->add(facing(blended(c.source, c.alpha), c.front, .2F, .2F));
    for (std::size_t i = 0; i < cases.size(); ++i) {
        auto backdrop = opaque(cases[i].destination);
        if (i == 2) {
            backdrop.alpha_mode = anima::AlphaMode::mask; // Alpha 1 passes the default cutoff of 0.5.
            backdrop.name = "masked";
        }
        (void)scene->add(facing(backdrop, cases[i].back, .6F, .6F));
    }
    anima::Environment plain;
    harness.render("composite", {scene}, view, plain);
    require(harness.stats.draw_calls == 2 * cases.size(), "The composite scene did not draw every quad");
    for (const auto &c : cases) {
        harness.expect("composite", view, c.front, over(c.alpha, c.source, c.destination),
                       "A blended quad over an opaque or masked one");
        // Beside the blended quad, the backdrop shows its own color.
        harness.expect("composite", view, c.back + anima::Vec3{0, .5F, 0}, c.destination, "A backdrop");
    }
    // Fog blends each surface toward the fog color by its own transmittance before compositing, and exposure scales
    // the composite.
    anima::Environment fogged;
    fogged.fog_color = {.35F, .3F, .45F};
    fogged.fog_density = .15F;
    fogged.exposure = 1.25F;
    harness.render("composite-fog", {scene}, view, fogged);
    const auto &image = harness.images["composite-fog"];
    const auto inverse = anima::inverse(view);
    const Color fog{fogged.fog_color.x, fogged.fog_color.y, fogged.fog_color.z};
    for (const auto &c : cases) {
        const auto at = pixel_of(view, c.front, image);
        const auto fogged_color = [&](const Color &color, double plane) {
            return over(std::exp(-fogged.fog_density * distance_to_plane(inverse, origin, at, image, plane)), color,
                        fog);
        };
        auto expected = over(c.alpha, fogged_color(c.source, c.front.z), fogged_color(c.destination, c.back.z));
        for (auto &channel : expected) {
            channel *= fogged.exposure;
            require(channel < 1, "The fog check's expected color reaches display white, which would hide errors");
        }
        harness.expect("composite-fog", view, c.front, expected, "A fogged, exposed blended quad");
    }
    harness.images.discard({"composite", "composite-fog"});
}

// Back-to-front order, in perspective and orthographic views: a near and a far blended quad at alpha 0.5 over black,
// the far one created last in one scene, or in a scene selected last; and two quads of equal bounds, whose equal
// keys keep creation order. Each case stands alone at the center of the view.
inline void check_order(Harness &harness) {
    const auto backdrop = facing(opaque(black), {0, 0, -8}, 4, 3);
    const anima::Vec3 near_center{0, 0, -3}, tie_center{0, 0, -3.5F};
    const auto near_quad = [&](const Color &color) { return facing(blended(color, .5F), near_center, .35F, .35F); };
    const auto far_quad = [](const Color &color) { return facing(blended(color, .5F), {0, 0, -4.5F}, .8F, .8F); };
    auto single = std::make_shared<anima::Scene>();
    (void)single->add(backdrop);
    (void)single->add(near_quad(red));
    (void)single->add(far_quad(blue));
    auto nearer = std::make_shared<anima::Scene>(), farther = std::make_shared<anima::Scene>();
    (void)nearer->add(backdrop);
    (void)nearer->add(near_quad(green));
    (void)farther->add(far_quad(red));
    auto tied = std::make_shared<anima::Scene>();
    (void)tied->add(backdrop);
    (void)tied->add(facing(blended(red, .5F), tie_center, .3F, .3F));
    (void)tied->add(facing(blended(green, .5F), tie_center, .3F, .3F));
    struct Case {
        std::string name, what;
        std::vector<std::shared_ptr<const anima::Scene>> scenes;
        anima::Vec3 sample;
        Color expected;
    };
    const std::vector<Case> cases{{"order-one-scene",
                                   "A near blended quad under a far one created after it",
                                   {single},
                                   near_center,
                                   over(.5, red, over(.5, blue, black))},
                                  {"order-two-scenes",
                                   "A near blended quad under a far one in a scene selected after it",
                                   {nearer, farther},
                                   near_center,
                                   over(.5, green, over(.5, red, black))},
                                  {"order-equal-keys",
                                   "Blended quads with equal keys out of creation order",
                                   {tied},
                                   tie_center,
                                   over(.5, green, over(.5, red, black))}};
    const auto aspect = harness.aspect();
    for (const auto &[projection, view] : {std::pair{std::string("perspective"), perspective_view(aspect)},
                                           std::pair{std::string("orthographic"), orthographic_view(aspect)}})
        for (const auto &c : cases) {
            const auto name = c.name + "-" + projection;
            harness.render(name, c.scenes, view, {});
            require(harness.stats.draw_calls == 3U, "The " + name + " selection did not draw every quad");
            harness.expect(name, view, c.sample, c.expected, c.what + " in the " + projection + " view");
            harness.images.discard({name});
        }
}

// Two blended quads that intersect: each contributes on both sides of the intersection, in the one order the keys
// give. A blended quad behind an opaque one is hidden where the opaque one covers it.
inline void check_depth(Harness &harness) {
    using anima::operator*;
    const auto view = perspective_view(harness.aspect());
    constexpr float turn = std::numbers::pi_v<float> / 3; // The second quad turns 60 degrees about +Y.
    const anima::Vec3 across{std::cos(turn), 0, std::sin(turn)}, turned_center{0, 0, -4.2F};
    auto crossing = std::make_shared<anima::Scene>();
    (void)crossing->add(facing(opaque(black), {0, 0, -8}, 4, 3));
    // Created first but nearer by its bounds' center, so it draws second.
    (void)crossing->add(facing(blended(red, .5F), {0, 0, -4}, 1.5F, 1));
    (void)crossing->add(quad(blended(green, .5F), turned_center, across, {0, .8F, 0}));
    harness.render("intersection", {crossing}, view, {});
    const auto expected = over(.5, red, over(.5, green, black));
    // Along the turned quad, behind the facing quad and then in front of it.
    harness.expect("intersection", view, turned_center + across * -.6F, expected,
                   "Intersecting blended quads where the turned one is behind");
    harness.expect("intersection", view, turned_center + across * .8F, expected,
                   "Intersecting blended quads where the turned one is in front");
    harness.images.discard({"intersection"});

    auto hidden = std::make_shared<anima::Scene>();
    (void)hidden->add(facing(opaque(black), {0, 0, -8}, 4, 3));
    (void)hidden->add(facing(blended(red, .5F), {0, 0, -4}, 1, 1));
    const Color cover{.2, .6, .3};
    const anima::Vec3 cover_center{.4F, 0, -3};
    (void)hidden->add(facing(opaque(cover), cover_center, .5F, .5F));
    harness.render("hidden", {hidden}, view, {});
    harness.expect("hidden", view, cover_center, cover, "A blended quad behind an opaque one");
    harness.expect("hidden", view, {-.6F, 0, -4}, over(.5, red, black), "A blended quad beside an opaque one");
    harness.images.discard({"hidden"});
}

// Mean of each channel over the pixels within 2 of the pixel that shows @p world.
inline std::array<double, 3> window_mean(const gpu_check::Image &image, const anima::Mat4 &view, anima::Vec3 world) {
    constexpr int radius = 2;
    const auto at = pixel_of(view, world, image);
    std::array<double, 3> sum{};
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx) {
            const auto color = gpu_check::pixel(image, std::size_t(int(at[0]) + dx), std::size_t(int(at[1]) + dy));
            for (std::size_t c = 0; c < sum.size(); ++c)
                sum[c] += color[c];
        }
    for (auto &value : sum)
        value /= (2 * radius + 1) * (2 * radius + 1);
    return sum;
}
// Levels by which a shadow must darken every channel of a lit surface.
constexpr double shadow_margin = 20;
// Requires every channel around @p shadowed in capture @p name to be darker than around @p lit by more than
// shadow_margin levels.
inline void require_shadow(const gpu_check::Captures &images, const std::string &name, const anima::Mat4 &view,
                           anima::Vec3 shadowed, anima::Vec3 lit, const std::string &message) {
    const auto dark = window_mean(images[name], view, shadowed), bright = window_mean(images[name], view, lit);
    bool darker = true;
    std::string report = name + ": shadowed";
    for (std::size_t c = 0; c < dark.size(); ++c) {
        darker = darker && dark[c] + shadow_margin < bright[c];
        report += " " + std::to_string(std::lround(dark[c]));
    }
    report += ", lit";
    for (const auto value : bright)
        report += " " + std::to_string(std::lround(value));
    std::cout << "BLENDING " << report << '\n';
    images.require(darker, message + ": " + report, {name});
}

// Lit quads under a sun from the upper left, whose shadows fall half a unit in +X per unit of height. A blended quad
// leaves the ground where its shadow would fall exactly as it is without the quad, while an opaque twin darkens its
// own shadow. A lit blended quad at alpha 1 renders as an opaque one that casts nothing does, the shadow of a caster
// above it included.
inline void check_shadows(Harness &harness) {
    const auto aspect = harness.aspect();
    const auto view = [&] {
        using anima::operator*;
        return anima::perspective(aspect, .1F, 50) * anima::look_at({0, 6, 6}, {0, 0, 0});
    }();
    anima::Environment lighting;
    lighting.sun.direction = {-.5F, 1, 0};
    lighting.sun.radiance = {2.5F, 2.5F, 2.5F};
    lighting.fill.radiance = {};
    lighting.ambient_sky = lighting.ambient_ground = {.25F, .25F, .25F};
    lighting.shadow.enabled = true;
    lighting.shadow.extent = 5;
    lighting.shadow.depth = 20;
    lighting.shadow.resolution = 1024;
    constexpr Color gray{.6, .6, .6}, tint{.9, .2, .2};
    const auto ground = horizontal(opaque(gray, true), {0, 0, 0}, 4, 3);

    auto policy = std::make_shared<anima::Scene>();
    (void)policy->add(ground);
    (void)policy->add(horizontal(opaque(tint, true), {-1.5F, 1, 0}, .5F, .5F));
    const auto quad_id = policy->add(horizontal(blended(tint, .5F, true), {1.5F, 1, 0}, .5F, .5F));
    harness.render("shadow-policy", {policy}, view, lighting);
    const auto casters = harness.stats.shadow_draw_calls;
    policy->set_visible(quad_id, false);
    harness.render("shadow-policy-without", {policy}, view, lighting);
    require(casters == 2U && harness.stats.shadow_draw_calls == casters,
            "A blended quad drew into the shadow map, or the ground and the opaque quad did not");
    const auto &with = harness.images["shadow-policy"], &without = harness.images["shadow-policy-without"];
    // Ground in each quad's shadow, which the camera sees past the quads, and ground in the sun.
    const anima::Vec3 opaque_shadow{-1, 0, .3F}, blended_shadow{2, 0, .3F}, sunlit{0, 0, 2};
    require_shadow(harness.images, "shadow-policy", view, opaque_shadow, sunlit,
                   "The opaque quad cast no visible shadow, so the policy check would prove nothing");
    const auto at = pixel_of(view, blended_shadow, with);
    constexpr int radius = 3;
    std::size_t changed = 0;
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx) {
            const auto x = std::size_t(int(at[0]) + dx), y = std::size_t(int(at[1]) + dy);
            if (gpu_check::pixel(with, x, y) != gpu_check::pixel(without, x, y))
                ++changed;
        }
    const auto report = "the ground where the blended quad's shadow would fall shows " +
                        gpu_check::text(gpu_check::pixel(with, at[0], at[1])) + " with the quad and " +
                        gpu_check::text(gpu_check::pixel(without, at[0], at[1])) + " without it; " +
                        std::to_string(changed) + " of " + std::to_string((2 * radius + 1) * (2 * radius + 1)) +
                        " pixels around it differ";
    std::cout << "BLENDING shadow-policy: " << report << '\n';
    harness.images.require(changed == 0, "A blended quad cast a shadow: " + report,
                           {"shadow-policy", "shadow-policy-without"});
    harness.images.discard({"shadow-policy", "shadow-policy-without"});

    // The receiver at alpha 1 composites as its own color, and the caster above it shadows it either way.
    const auto receiver = [&](bool blend) {
        auto scene = std::make_shared<anima::Scene>();
        (void)scene->add(ground);
        (void)scene->add(horizontal(opaque(tint, true), {-2.7F, 1.5F, 0}, .5F, .3F));
        const auto id = scene->add(horizontal(blend ? blended(gray, 1, true) : opaque(gray, true), {-2, .5F, 0}, 1, 1));
        scene->set_casts_shadows(id, false);
        return scene;
    };
    harness.render("receiver-opaque", {receiver(false)}, view, lighting);
    harness.render("receiver-blended", {receiver(true)}, view, lighting);
    // On the receiver, inside and outside the caster's shadow.
    const anima::Vec3 shaded{-2.2F, .5F, 0}, lit{-2.2F, .5F, .7F};
    require_shadow(harness.images, "receiver-opaque", view, shaded, lit,
                   "The caster cast no visible shadow on the receiver, so the receiving check would prove nothing");
    require_shadow(harness.images, "receiver-blended", view, shaded, lit, "A lit blended quad received no shadow");
    // Compiled with its own output, the blended shader may round differently, so the images need only agree.
    const auto agreement = gpu_check::parity(harness.images["receiver-opaque"], harness.images["receiver-blended"]);
    std::cout << "BLENDING receiver-blended against receiver-opaque: mean channel difference " << agreement.mean
              << ", pixels beyond 16 levels " << agreement.large << '\n';
    harness.images.require_parity("receiver-opaque", "receiver-blended");
    harness.images.discard({"receiver-opaque", "receiver-blended"});
}

inline int run(int argc, char **argv) {
    require(argc == 3, "Usage: consumer --blending OUTPUT");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    Harness harness(argv[2]);
    check_compositing(harness);
    check_order(harness);
    check_depth(harness);
    check_shadows(harness);
    harness.finish();
    std::cout << "PASS blending: premultiplied linear compositing over opaque and masked quads, fog and exposure, "
                 "back-to-front order across scenes and projections, intersecting and hidden blended quads, and "
                 "shadows that blended quads receive but do not cast\n";
    return 0;
}
} // namespace blending_test
