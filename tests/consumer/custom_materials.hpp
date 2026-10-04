#pragma once
// Custom materials through the public API, supplied as an application supplies them: GLSL of its own
// (custom_surface.vert, custom_water.frag, custom_effect.frag, custom_probe.frag, custom_shadow.vert and the
// custom_fade shaders), compiled to SPIR-V by glslc in this application's build, for a water that reads opaque depth
// and color, an effect whose intensity follows the caller's time, a probe that writes the frame inputs as colors, and
// a surface that fades across its visibility range. Every expected pixel follows from those shaders and the
// renderer's contract: the frame inputs are the renderer's settings, opaque custom materials draw with the opaque
// meshes, blended and additive ones sort with the blended meshes, opaque inputs are copied only on frames that draw a
// material that reads them, skinned draws blend their joints, only a depth-only variant casts shadows, and the fading
// helpers dissolve and cast as the standard material does.
#include "blending.hpp"
#include "gpu_checks.hpp"
#include "rejection.hpp"
#include <algorithm>
#include <anima/custom_material.hpp>
#include <anima/scene.hpp>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace custom_material_test {
constexpr std::uint32_t surface_vertex[] =
#include "custom_surface.vert.inc"
    ;
constexpr std::uint32_t water_fragment[] =
#include "custom_water.frag.inc"
    ;
constexpr std::uint32_t effect_fragment[] =
#include "custom_effect.frag.inc"
    ;
constexpr std::uint32_t shadow_vertex[] =
#include "custom_shadow.vert.inc"
    ;
constexpr std::uint32_t probe_fragment[] =
#include "custom_probe.frag.inc"
    ;
constexpr std::uint32_t fade_vertex[] =
#include "custom_fade.vert.inc"
    ;
constexpr std::uint32_t fade_fragment[] =
#include "custom_fade.frag.inc"
    ;
constexpr std::uint32_t fade_shadow_vertex[] =
#include "custom_fade_shadow.vert.inc"
    ;
constexpr std::uint32_t depth_fragment[] =
#include "custom_depth.frag.inc"
    ;
using blending_test::Color;
using blending_test::over;
using rejection::rejects;
constexpr Color black{0, 0, 0}, red{1, 0, 0}, blue{0, 0, 1}, gray{.2, .2, .2};

inline void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <std::size_t Count> std::vector<std::uint32_t> words(const std::uint32_t (&code)[Count]) {
    return {code, code + Count};
}
// The std140 bytes of a block of vec4 members.
inline std::vector<std::byte> parameters(std::initializer_list<std::array<float, 4>> members) {
    std::vector<std::byte> bytes(members.size() * sizeof(std::array<float, 4>));
    std::size_t offset = 0;
    for (const auto &member : members) {
        std::memcpy(bytes.data() + offset, member.data(), sizeof(member));
        offset += sizeof(member);
    }
    return bytes;
}
// The effect's texture: one sRGB texel, which every coordinate samples.
constexpr std::array<std::uint8_t, 4> pattern_texel{255, 200, 160, 255};
inline double decoded(std::uint8_t value) {
    const double encoded = value / 255.0;
    return encoded <= .04045 ? encoded / 12.92 : std::pow((encoded + .055) / 1.055, 2.4);
}
inline anima::Texture pattern() {
    anima::Texture texture;
    texture.image = std::make_shared<anima::Image>(
        anima::Image{1, 1, std::vector<std::uint8_t>(pattern_texel.begin(), pattern_texel.end())});
    texture.sampler.mag = texture.sampler.min = anima::Filter::nearest;
    texture.sampler.u = texture.sampler.v = anima::Wrap::clamp;
    texture.sampler.mipmapped = false;
    return texture;
}

// Parameters of the effect shader.
struct Effect {
    // Linear color, before the texture.
    Color color;
    // Alpha that a blended effect composites with; 1 otherwise.
    double coverage = 1;
    // Intensity base + amplitude * sin(frequency * time).
    double base = 1, amplitude = 0, frequency = 0;
    // The straight linear color that the effect writes at @p time for an object whose factor is @p factor.
    [[nodiscard]] Color shaded(double time = 0, const Color &factor = {1, 1, 1}) const {
        const auto intensity = base + amplitude * std::sin(frequency * time);
        Color result{};
        for (std::size_t c = 0; c < result.size(); ++c)
            result[c] = color[c] * decoded(pattern_texel[c]) * factor[c] * intensity;
        return result;
    }
};
inline anima::CustomMaterialDefinition effect_definition(const std::string &name, anima::CustomBlend blend,
                                                         const Effect &effect) {
    anima::CustomMaterialDefinition definition;
    definition.name = name;
    definition.vertex_shader = words(surface_vertex);
    definition.fragment_shader = words(effect_fragment);
    definition.blend = blend;
    definition.parameters =
        parameters({{float(effect.color[0]), float(effect.color[1]), float(effect.color[2]), float(effect.coverage)},
                    {float(effect.base), float(effect.amplitude), float(effect.frequency), 0}});
    definition.textures = {pattern()};
    return definition;
}
// The effect in @p blend mode, with the depth-only variant when @p shadows.
inline std::shared_ptr<const anima::CustomMaterial> effect_material(const std::string &name, anima::CustomBlend blend,
                                                                    const Effect &effect, bool shadows = false) {
    auto definition = effect_definition(name, blend, effect);
    if (shadows)
        definition.shadow_vertex_shader = words(shadow_vertex);
    return std::make_shared<const anima::CustomMaterial>(std::move(definition));
}
// Parameters of the water shader.
struct Water {
    // Linear color of infinitely deep water.
    Color deep;
    // Absorption per unit of distance through the water.
    double absorption{};
};
inline std::shared_ptr<const anima::CustomMaterial>
water_material(const Water &water, anima::CustomBlend blend = anima::CustomBlend::blended) {
    anima::CustomMaterialDefinition definition;
    definition.name = "water";
    definition.vertex_shader = words(surface_vertex);
    definition.fragment_shader = words(water_fragment);
    definition.blend = blend;
    definition.parameters = parameters(
        {{float(water.deep[0]), float(water.deep[1]), float(water.deep[2]), 0}, {float(water.absorption), 0, 0, 0}});
    return std::make_shared<const anima::CustomMaterial>(std::move(definition));
}

// A quad facing +Z whose Material is white, so that its authored factor, which reaches custom shaders, is 1.
inline std::shared_ptr<const anima::Mesh> surface(anima::Vec3 center, float half_width, float half_height) {
    return blending_test::facing(blending_test::opaque({1, 1, 1}), center, half_width, half_height);
}
// Adds an object that draws @p mesh with @p material.
inline anima::Scene::Id add(anima::Scene &scene, std::shared_ptr<const anima::Mesh> mesh,
                            std::shared_ptr<const anima::CustomMaterial> material) {
    const auto id = scene.add(std::move(mesh));
    scene.set_custom_material(id, 0, std::move(material));
    return id;
}

// Draws selections in one window and keeps each frame read back by name.
class Harness {
  public:
    explicit Harness(const std::filesystem::path &output)
        : window_(gpu_check::window("Anima custom material verification", 800, 600)),
          renderer_(window_.get(), options()), images(output) {}
    [[nodiscard]] float aspect() const {
        int width = 0, height = 0;
        require(SDL_GetWindowSizeInPixels(window_.get(), &width, &height) && width > 0 && height > 0,
                "Custom material window has no drawable size");
        return float(width) / float(height);
    }
    [[nodiscard]] anima::VulkanRenderer &renderer() { return renderer_; }
    /// Draws @p scenes through @p view_projection in @p environment at shader time @p time, and reads the frame
    /// back as @p name.
    void render(const std::string &name, std::vector<std::shared_ptr<const anima::Scene>> scenes,
                const anima::Mat4 &view_projection, const anima::Environment &environment = {}, float time = 0) {
        renderer_.set_environment(environment);
        renderer_.set_time(time);
        renderer_.set_scenes(std::move(scenes));
        renderer_.set_view(view_projection);
        renderer_.request_capture();
        const auto started = std::chrono::steady_clock::now();
        for (;;) {
            require(std::chrono::steady_clock::now() - started < gpu_check::watchdog, "Custom material watchdog");
            SDL_Event event{};
            while (SDL_PollEvent(&event))
                require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                        "Custom material test interrupted");
            if (renderer_.draw())
                break;
            SDL_Delay(5);
        }
        images.add(name, gpu_check::take(renderer_));
        stats = renderer_.resource_stats();
    }
    /// Requires the pixel of capture @p name that shows @p world through @p view_projection to show linear
    /// @p expected, within blending_test::tolerance levels per channel of its display encoding.
    void expect(const std::string &name, const anima::Mat4 &view_projection, anima::Vec3 world, const Color &expected,
                const std::string &what) const {
        const auto &image = images[name];
        const auto at = blending_test::pixel_of(view_projection, world, image);
        const auto actual = gpu_check::pixel(image, at[0], at[1]);
        bool matched = true;
        std::string wanted;
        for (std::size_t c = 0; c < expected.size(); ++c) {
            const auto target = blending_test::encoded(expected[c]);
            require(expected[c] < 1, what + ": the expected color reaches display white, which would hide errors");
            matched = matched && std::abs(actual[c] - target) <= blending_test::tolerance;
            wanted += (c ? " " : "") + std::to_string(target);
        }
        const auto report = what + ": pixel " + std::to_string(at[0]) + ", " + std::to_string(at[1]) + " of " + name +
                            " shows " + gpu_check::text(actual) + ", expected " + wanted + " (linear " +
                            blending_test::text(expected) + ")";
        std::cout << "CUSTOM " << report << '\n';
        images.require(matched, report, {name});
    }
    /// Shuts the renderer down and requires clean validation.
    void finish() {
        const auto result = renderer_.shutdown();
        require(!result.validation_errors && !result.validation_warnings, "Custom material GPU validation failed");
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

// Invalid time and materials that the application's own SPIR-V makes invalid are rejected before drawing.
inline void check_rejections(Harness &harness) {
    rejects<std::invalid_argument>([&] { harness.renderer().set_time(std::numeric_limits<float>::quiet_NaN()); },
                                   "Shader time must be finite");
    rejects<std::invalid_argument>(
        [] { (void)water_material({black, 1}, anima::CustomBlend::opaque); },
        "The fragment shader declares opaque depth (set 0, binding 1), but only blended and additive custom "
        "materials can read it");
    rejects<std::invalid_argument>(
        [] {
            auto definition = effect_definition("short", anima::CustomBlend::opaque, {});
            definition.parameters.resize(16);
            (void)anima::CustomMaterial(std::move(definition));
        },
        "The fragment shader declares a parameter block of 32 bytes, but the material supplies 16");
    rejects<std::invalid_argument>(
        [] {
            auto definition = effect_definition("untextured", anima::CustomBlend::opaque, {});
            definition.textures.clear();
            (void)anima::CustomMaterial(std::move(definition));
        },
        "The fragment shader declares texture 0 (set 2, binding 1), but the material supplies 0 textures");
}

// Probes that write the frame inputs as colors show the camera, lights, fog and viewport that the renderer was given.
// Fourteen materials share the probe's SPIR-V, each selecting one input through its parameters.
inline void check_inputs(Harness &harness) {
    const anima::Vec3 eye{1, 2, 3};
    const auto view = [&] {
        return anima::perspective(std::numbers::pi_v<float> / 4, harness.aspect(), .1F, 50) *
               anima::look_at(eye, eye + anima::Vec3{0, 0, -1});
    }();
    anima::Environment environment;
    environment.sun = {{.3F, .8F, .2F}, {.9F, .6F, .3F}};
    environment.fill = {{-.5F, .2F, .7F}, {.2F, .4F, .6F}};
    environment.ambient_sky = {.11F, .22F, .33F};
    environment.ambient_ground = {.3F, .2F, .1F};
    environment.ambient_specular = {.05F, .1F, .15F};
    environment.fog.color = {.4F, .5F, .6F};
    environment.fog.density = .07F;
    environment.fog.height = .3F;
    environment.fog.falloff = .4F;
    environment.fog.sky_distance = .5F;
    environment.fog.sun_anisotropy = .45F;
    environment.fog.sun_scattering = {.5F, .25F, 1};
    const auto color = [](anima::Vec3 value) { return Color{value.x, value.y, value.z}; };
    const auto direction = [](anima::Vec3 value) {
        const auto unit = anima::normalized(value);
        return Color{unit.x * .5 + .5, unit.y * .5 + .5, unit.z * .5 + .5};
    };
    const auto aspect = double(harness.aspect());
    const std::vector<std::pair<std::string, Color>> inputs{
        {"sun irradiance", color(environment.sun.irradiance)},
        {"fill irradiance", color(environment.fill.irradiance)},
        {"ambient sky", color(environment.ambient_sky)},
        {"ambient ground", color(environment.ambient_ground)},
        {"ambient specular", color(environment.ambient_specular)},
        {"fog color", color(environment.fog.color)},
        {"fog density and viewport", {environment.fog.density, aspect / 2, 1 / aspect}},
        {"sun direction", direction(environment.sun.direction)},
        {"fill direction", direction(environment.fill.direction)},
        {"view origin", {eye.x * .1 + .25, eye.y * .1 + .25, eye.z * .1 + .25}},
        {"view-projection and its inverse", {.25, 0, .5}},
        {"fog height, falloff and sky distance",
         {environment.fog.height, environment.fog.falloff, environment.fog.sky_distance}},
        {"fog sunlight",
         {double(environment.fog.sun_scattering.x) * environment.sun.irradiance.x,
          double(environment.fog.sun_scattering.y) * environment.sun.irradiance.y,
          double(environment.fog.sun_scattering.z) * environment.sun.irradiance.z}},
        {"fog phase asymmetry, and sunlight w", {environment.fog.sun_anisotropy, .25, .5}}};
    auto scene = std::make_shared<anima::Scene>();
    std::vector<anima::Vec3> centers;
    for (std::uint32_t i = 0; i < inputs.size(); ++i) {
        anima::CustomMaterialDefinition definition;
        definition.name = "probe " + std::to_string(i);
        definition.vertex_shader = words(surface_vertex);
        definition.fragment_shader = words(probe_fragment);
        definition.parameters.resize(16);
        std::memcpy(definition.parameters.data(), &i, sizeof(i));
        centers.push_back(eye + anima::Vec3{-1.2F + .6F * float(i % 5), .6F - .6F * float(i / 5), -3});
        (void)add(*scene, surface(centers.back(), .12F, .12F),
                  std::make_shared<const anima::CustomMaterial>(std::move(definition)));
    }
    harness.render("inputs", {scene}, view, environment);
    for (std::size_t i = 0; i < inputs.size(); ++i)
        harness.expect("inputs", view, centers[i], inputs[i].second, "The frame's " + inputs[i].first);
    harness.images.discard({"inputs"});
}

// An opaque custom quad draws with the opaque meshes: it hides a blended quad behind it, a nearer opaque quad
// hides it, and the object's material factor reaches its shader.
inline void check_opaque(Harness &harness) {
    const auto view = blending_test::perspective_view(harness.aspect());
    const Effect effect{.color = {.8, .9, .7}};
    const auto material = effect_material("opaque effect", anima::CustomBlend::opaque, effect);
    auto scene = std::make_shared<anima::Scene>();
    (void)scene->add(blending_test::facing(blending_test::opaque(black), {0, 0, -8}, 4, 3));
    (void)scene->add(blending_test::facing(blending_test::blended(red, .5F), {-1.2F, 0, -4}, .6F, .6F));
    const auto tinted = add(*scene, surface({-.9F, 0, -3}, .2F, .2F), material);
    const Color factor{.5, 1, .25};
    scene->set_material_factor(tinted, 0, {float(factor[0]), float(factor[1]), float(factor[2])});
    (void)add(*scene, surface({.9F, 0, -3}, .4F, .4F), material);
    const Color cover{.2, .6, .3};
    (void)scene->add(blending_test::facing(blending_test::opaque(cover), {.6F, 0, -2}, .1F, .1F));
    // Created before the custom quad it covers, so only the pass order puts it in front.
    (void)scene->add(blending_test::facing(blending_test::blended(blue, .5F), {.6F, .2F, -2}, .04F, .04F));
    harness.render("opaque", {scene}, view);
    require(harness.stats.draw_calls == 6, "The opaque custom material scene did not draw every quad");
    require(!harness.stats.opaque_inputs, "A frame without a material that reads opaque inputs copied them");
    harness.expect("opaque", view, {-.9F, 0, -3}, effect.shaded(0, factor),
                   "An opaque custom quad over a blended one, with its object's factor");
    harness.expect("opaque", view, {-.72F, 0, -4}, over(.5, red, black), "The blended quad beside it");
    harness.expect("opaque", view, {.6F, 0, -2}, cover, "An opaque quad in front of an opaque custom one");
    harness.expect("opaque", view, {.6F, .2F, -2}, over(.5, blue, effect.shaded()),
                   "A blended quad in front of an opaque custom one");
    harness.expect("opaque", view, {1.2F, 0, -3}, effect.shaded(), "The opaque custom quad beside them");
    harness.images.discard({"opaque"});
}

// Blended and additive custom quads sort with blended meshes by distance, whichever kind is nearer, over a gray
// backdrop: the "over" operator on premultiplied color, and addition.
inline void check_sorting(Harness &harness) {
    const auto view = blending_test::perspective_view(harness.aspect());
    const Effect glaze{.color = {.3, .9, .8}, .coverage = .5}, glow{.color = {.35, .5, .6}};
    const auto glaze_material = effect_material("glaze", anima::CustomBlend::blended, glaze);
    const auto glow_material = effect_material("glow", anima::CustomBlend::additive, glow);
    const anima::Vec3 near_center{0, 0, -3}, far_center{0, 0, -4.5F};
    const auto backdrop = blending_test::facing(blending_test::opaque(gray), {0, 0, -8}, 4, 3);
    const auto near_mesh = [&] { return surface(near_center, .35F, .35F); };
    const auto far_mesh = [&] { return surface(far_center, .8F, .8F); };
    const auto near_blended = [&](const Color &color) {
        return blending_test::facing(blending_test::blended(color, .5F), near_center, .35F, .35F);
    };
    const auto far_blended = [&](const Color &color) {
        return blending_test::facing(blending_test::blended(color, .5F), far_center, .8F, .8F);
    };
    const auto add_color = [](const Color &a, const Color &b) { return Color{a[0] + b[0], a[1] + b[1], a[2] + b[2]}; };
    struct Case {
        std::string name, what;
        std::shared_ptr<anima::Scene> scene;
        Color expected;
    };
    std::vector<Case> cases;
    // Each custom quad is created before the blended mesh it sorts against, so only its distance orders them.
    {
        auto scene = std::make_shared<anima::Scene>();
        (void)scene->add(backdrop);
        (void)add(*scene, far_mesh(), glaze_material);
        (void)scene->add(near_blended(red));
        cases.push_back({"sort-blended-over-custom", "A blended quad over a farther blended custom one", scene,
                         over(.5, red, over(glaze.coverage, glaze.shaded(), gray))});
    }
    {
        auto scene = std::make_shared<anima::Scene>();
        (void)scene->add(backdrop);
        (void)scene->add(far_blended(blue));
        (void)add(*scene, near_mesh(), glaze_material);
        cases.push_back({"sort-custom-over-blended", "A blended custom quad over a farther blended one", scene,
                         over(glaze.coverage, glaze.shaded(), over(.5, blue, gray))});
    }
    {
        auto scene = std::make_shared<anima::Scene>();
        (void)scene->add(backdrop);
        (void)add(*scene, far_mesh(), glow_material);
        (void)scene->add(near_blended(blue));
        cases.push_back({"sort-blended-over-additive", "A blended quad over a farther additive custom one", scene,
                         over(.5, blue, add_color(gray, glow.shaded()))});
    }
    {
        auto scene = std::make_shared<anima::Scene>();
        (void)scene->add(backdrop);
        (void)add(*scene, near_mesh(), glow_material);
        (void)scene->add(far_blended(blue));
        cases.push_back({"sort-additive-over-blended", "An additive custom quad over a farther blended one", scene,
                         add_color(over(.5, blue, gray), glow.shaded())});
    }
    for (const auto &c : cases) {
        harness.render(c.name, {c.scene}, view);
        require(harness.stats.draw_calls == 3U, "The " + c.name + " selection did not draw every quad");
        require(!harness.stats.opaque_inputs, "The " + c.name + " selection copied opaque inputs that no shader reads");
        harness.expect(c.name, view, near_center, c.expected, c.what);
        harness.images.discard({c.name});
    }
}

// The effect's intensity follows the time that the application sets: base + amplitude * sin(frequency * time).
inline void check_time(Harness &harness) {
    const auto view = blending_test::perspective_view(harness.aspect());
    const Effect pulse{.color = {.9, .8, .7}, .base = .5, .amplitude = .4, .frequency = 2};
    auto scene = std::make_shared<anima::Scene>();
    (void)scene->add(blending_test::facing(blending_test::opaque(black), {0, 0, -8}, 4, 3));
    (void)add(*scene, surface({0, 0, -3}, .5F, .5F), effect_material("pulse", anima::CustomBlend::opaque, pulse));
    constexpr auto pi = std::numbers::pi_v<float>;
    for (const auto time : {0.F, pi / 4, 3 * pi / 4}) {
        const auto name = "time-" + std::to_string(time);
        harness.render(name, {scene}, view, {}, time);
        harness.expect(name, view, {0, 0, -3}, pulse.shaded(time), "The effect at time " + std::to_string(time));
        harness.images.discard({name});
    }
}

// The water reads opaque depth and color: a near and a far backdrop show through it by the transmittance of the
// water between its surface and each, and the rest of the frame is unchanged. A frame whose water is hidden or
// culled copies nothing. Until a frame copies opaque inputs the view's depth is transient, so the scene targets count
// no more bytes than after, and fewer where the device has lazily allocated memory; the depth that can be copied
// stays until the swapchain is recreated.
inline void check_water(Harness &harness) {
    const auto view = blending_test::perspective_view(harness.aspect());
    const Color near_color{.9, .5, .1}, far_color{.1, .6, .9};
    const Water water{{.02, .15, .2}, .35};
    auto scene = std::make_shared<anima::Scene>();
    (void)scene->add(blending_test::facing(blending_test::opaque(near_color), {-1.5F, 0, -4}, 1.5F, 2));
    (void)scene->add(blending_test::facing(blending_test::opaque(far_color), {3, 0, -9}, 3, 4));
    const anima::Vec3 surface_center{0, 0, -2};
    const auto pool = add(*scene, surface(surface_center, 1, .6F), water_material(water));
    harness.renderer().request_resize();
    scene->set_visible(pool, false);
    harness.render("water-unread", {scene}, view);
    require(!harness.stats.opaque_inputs, "A frame whose water is hidden copied opaque inputs");
    const auto unread_bytes = harness.stats.world_target_bytes;
    scene->set_visible(pool, true);
    harness.render("water", {scene}, view);
    require(harness.stats.opaque_inputs && harness.stats.opaque_input_bytes > 0,
            "A frame that draws water did not copy opaque depth and color");
    const auto read_bytes = harness.stats.world_target_bytes;
    std::cout << "CUSTOM scene target bytes: " << unread_bytes << " with a transient depth, " << read_bytes
              << " with one that can be copied\n";
    require(unread_bytes > 0 && unread_bytes <= read_bytes,
            "The transient depth counted more bytes than the one that opaque inputs copy");
    const auto &image = harness.images["water"];
    const auto inverse = anima::inverse(view);
    for (const auto x : {-.4F, .4F}) {
        const anima::Vec3 point{x, 0, surface_center.z};
        const auto at = blending_test::pixel_of(view, point, image);
        const double back = x < 0 ? -4 : -9;
        const auto thickness = blending_test::distance_to_plane(inverse, blending_test::origin, at, image, back) -
                               blending_test::distance_to_plane(inverse, blending_test::origin, at, image, point.z);
        const auto transmittance = std::exp(-water.absorption * thickness);
        std::cout << "CUSTOM water at x " << x << ": thickness " << thickness << ", transmittance " << transmittance
                  << '\n';
        harness.expect("water", view, point, over(transmittance, x < 0 ? near_color : far_color, water.deep),
                       x < 0 ? "Water over the near backdrop" : "Water over the far backdrop");
    }
    harness.expect("water", view, {-1, 1.5F, -4}, near_color, "The near backdrop beside the water");
    // A new swapchain replaces the world passes and the copies, and the water's pipeline still draws.
    harness.renderer().request_resize();
    harness.render("water-recreated", {scene}, view);
    harness.images.require_same("water", "water-recreated", "Water after swapchain recreation");
    require(harness.stats.world_target_bytes == read_bytes,
            "The water after swapchain recreation counted other scene target bytes");
    harness.images.discard({"water", "water-recreated"});

    scene->set_visible(pool, false);
    harness.render("water-hidden", {scene}, view);
    require(!harness.stats.opaque_inputs, "A frame whose water is hidden copied opaque inputs");
    require(harness.stats.world_target_bytes == read_bytes,
            "A frame that copies no opaque inputs released the depth that they copy");
    harness.expect("water-hidden", view, {-.4F, 0, -2}, near_color, "The near backdrop without the water");
    harness.images.require_same("water-unread", "water-hidden",
                                "The hidden water with a transient depth and with one that can be copied");
    scene->set_visible(pool, true);
    // Behind the camera, the water is culled.
    auto behind = anima::identity();
    behind[14] = 8;
    scene->set_transform(pool, behind);
    harness.render("water-culled", {scene}, view);
    require(!harness.stats.opaque_inputs && harness.stats.culled_draws == 1,
            "A frame whose water is culled copied opaque inputs");
    harness.images.discard({"water-unread", "water-hidden", "water-culled"});
}

// A skinned custom quad follows its joint, through the pose buffer and the joint attributes. It draws after a
// swapchain recreation has released the opaque copies, which a material that does not read them never needs.
inline void check_skinning(Harness &harness) {
    harness.renderer().request_resize();
    const auto view = blending_test::perspective_view(harness.aspect());
    anima::Asset asset;
    asset.nodes.resize(2);
    asset.nodes[1].parent = 0;
    asset.skins.push_back({{1}, {anima::identity()}});
    asset.materials.push_back(blending_test::opaque({1, 1, 1}));
    anima::SourcePrimitive primitive;
    primitive.skin = 0;
    primitive.material = 0;
    const float half = .3F;
    for (const auto corner : {0, 1, 2, 0, 2, 3}) {
        anima::SourceVertex vertex;
        vertex.position = {corner == 0 || corner == 3 ? -half : half, corner < 2 ? -half : half, -3};
        vertex.normal = {0, 0, 1};
        vertex.weights = {1, 0, 0, 0};
        primitive.vertices.push_back(vertex);
    }
    asset.primitives.push_back(std::move(primitive));
    const auto mesh = anima::Mesh::compile(asset);
    const Effect effect{.color = {.6, .8, .9}};
    auto scene = std::make_shared<anima::Scene>();
    (void)scene->add(blending_test::facing(blending_test::opaque(black), {0, 0, -8}, 4, 3));
    const auto skinned = add(*scene, mesh, effect_material("skinned effect", anima::CustomBlend::opaque, effect));
    auto pose = anima::sample_pose(asset);
    pose.world[1][12] = .8F;
    scene->set_pose(skinned, pose);
    harness.render("skinned", {scene}, view);
    harness.expect("skinned", view, {.8F, 0, -3}, effect.shaded(), "A skinned custom quad at its joint");
    harness.expect("skinned", view, {-.2F, 0, -3}, black, "The skinned quad's rest position");
    harness.images.discard({"skinned"});
}

// Under a sun from the upper left, whose shadows fall half a unit in +X per unit of height, an opaque custom quad
// with a depth-only variant shadows the ground, and one without leaves it exactly as it is without the quad.
inline void check_shadows(Harness &harness) {
    const auto aspect = harness.aspect();
    const auto view = [&] {
        return anima::perspective(std::numbers::pi_v<float> / 4, aspect, .1F, 50) *
               anima::look_at({0, 6, 6}, {0, 0, 0});
    }();
    anima::Environment lighting;
    lighting.sun.direction = {-.5F, 1, 0};
    lighting.sun.irradiance = {2.5F, 2.5F, 2.5F};
    lighting.fill.irradiance = {};
    lighting.ambient_sky = lighting.ambient_ground = {.25F, .25F, .25F};
    // One cascade over the 15 m in view, with texels of about 1 cm.
    lighting.shadow_cascades.enabled = true;
    lighting.shadow_cascades.count = 1;
    lighting.shadow_cascades.distance = 15;
    const Effect effect{.color = {.5, .7, .9}};
    const auto white = blending_test::opaque({1, 1, 1});
    auto scene = std::make_shared<anima::Scene>();
    (void)scene->add(blending_test::horizontal(blending_test::opaque({.6, .6, .6}, true), {0, 0, 0}, 4, 3));
    (void)add(*scene, blending_test::horizontal(white, {-1.5F, 1, 0}, .5F, .5F),
              effect_material("casting effect", anima::CustomBlend::opaque, effect, true));
    const auto silent = add(*scene, blending_test::horizontal(white, {1.5F, 1, 0}, .5F, .5F),
                            effect_material("effect", anima::CustomBlend::opaque, effect));
    harness.render("shadows", {scene}, view, lighting);
    const auto casters = harness.stats.shadow_draw_calls;
    scene->set_visible(silent, false);
    harness.render("shadows-without", {scene}, view, lighting);
    require(casters == 2U && harness.stats.shadow_draw_calls == casters,
            "The ground and the depth-only variant did not both draw into the shadow map, or a custom material "
            "without one did");
    const anima::Vec3 cast{-1, 0, .3F}, silent_shadow{2, 0, .3F}, sunlit{0, 0, 2};
    blending_test::require_shadow(harness.images, "shadows", view, cast, sunlit,
                                  "The depth-only variant cast no visible shadow");
    const auto &with = harness.images["shadows"], &without = harness.images["shadows-without"];
    const auto at = blending_test::pixel_of(view, silent_shadow, with);
    constexpr int radius = 3;
    std::size_t changed = 0;
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx) {
            const auto x = std::size_t(int(at[0]) + dx), y = std::size_t(int(at[1]) + dy);
            if (gpu_check::pixel(with, x, y) != gpu_check::pixel(without, x, y))
                ++changed;
        }
    const auto report = std::to_string(changed) + " of " + std::to_string((2 * radius + 1) * (2 * radius + 1)) +
                        " pixels differ where the quad without a depth-only variant would cast its shadow";
    std::cout << "CUSTOM shadows: " << report << '\n';
    harness.images.require(changed == 0, "A custom material without a depth-only variant cast a shadow: " + report,
                           {"shadows", "shadows-without"});
    harness.images.discard({"shadows", "shadows-without"});
}

// Copies of an opaque custom quad with a depth-only variant, drawn through one object's placements, match the same
// copies as separate objects, their shadows on the ground included: the shaders' animaModelMatrix() composes the
// placement as the standard material does. The translations are exact in float, so the frames are identical.
inline void check_placements(Harness &harness) {
    const auto aspect = harness.aspect();
    const auto view = [&] {
        return anima::perspective(std::numbers::pi_v<float> / 4, aspect, .1F, 50) *
               anima::look_at({0, 6, 6}, {0, 0, 0});
    }();
    anima::Environment lighting;
    lighting.sun.direction = {-.5F, 1, 0};
    lighting.sun.irradiance = {2.5F, 2.5F, 2.5F};
    lighting.fill.irradiance = {};
    lighting.ambient_sky = lighting.ambient_ground = {.25F, .25F, .25F};
    // One cascade over the 15 m in view, with texels of about 1 cm.
    lighting.shadow_cascades.enabled = true;
    lighting.shadow_cascades.count = 1;
    lighting.shadow_cascades.distance = 15;
    const auto ground = blending_test::horizontal(blending_test::opaque({.6, .6, .6}, true), {0, 0, 0}, 4, 3);
    const auto quad = blending_test::horizontal(blending_test::opaque({1, 1, 1}), {0, 0, 0}, .25F, .25F);
    const auto material =
        effect_material("casting effect", anima::CustomBlend::opaque, Effect{.color = {.5, .7, .9}}, true);
    std::vector<anima::Mat4> placed;
    for (const auto x : {-2.F, -1.F, 0.F, 1.F})
        for (const auto z : {-1.5F, -.5F, .5F}) {
            auto m = anima::identity();
            anima::set_translation(m, {x, .75F + .25F * (x + 2), z});
            placed.push_back(m);
        }
    auto separate = std::make_shared<anima::Scene>(), together = std::make_shared<anima::Scene>();
    (void)separate->add(ground);
    (void)together->add(ground);
    for (const auto &m : placed)
        separate->set_transform(add(*separate, quad, material), m);
    const auto field = add(*together, quad, material);
    together->set_placements(field, anima::MeshPlacements::create(quad, placed));
    harness.render("placed-separate", {separate}, view, lighting);
    const auto apart = harness.stats;
    harness.render("placed-together", {together}, view, lighting);
    require(harness.stats.draw_calls == 2 && harness.stats.drawn_copies == placed.size() + 1 &&
                apart.draw_calls == placed.size() + 1,
            "Custom placements did not draw every copy in one call");
    harness.images.require_same("placed-separate", "placed-together",
                                "Placed custom copies differ from separate objects");
    std::cout << "CUSTOM placements: " << placed.size() << " copies in " << harness.stats.draw_calls
              << " main-view and " << harness.stats.shadow_draw_calls << " shadow draw calls match " << apart.draw_calls
              << " and " << apart.shadow_draw_calls << " for separate objects\n";
    harness.images.discard({"placed-separate", "placed-together"});
}

// Quads of a custom material that fades through animaVisibility() and animaDissolved(), as custom_fade.vert and
// custom_fade.frag do, dissolve exactly the pixels that the standard material dissolves at the same distances, across
// the range's begin and end margins, as separate objects and as placed copies, and its depth-only variant
// custom_fade_shadow.vert, which casts while more than half of a quad draws, leaves exactly the standard material's
// shadows, which are those of the quads that more than half of draws, drawn whole. Each quad sits midway between two of
// the dither's thresholds, so rounding in either shader cannot move a pixel across one. The placed copies form one
// cluster, which the range cannot cull, so the shaders alone hide the copies outside it.
inline void check_fading(Harness &harness) {
    const auto aspect = harness.aspect();
    const anima::Vec3 eye{0, 2, 0};
    const auto view = [&] {
        return anima::perspective(std::numbers::pi_v<float> / 4, aspect, .1F, 100) * anima::look_at(eye, {0, 2, -1});
    }();
    // Gone before 4 m, dissolving in until it is whole at 12 m, whole to 24 m, then dissolving out until it is gone
    // at 40 m.
    const anima::VisibilityRange range{4, 40, 8, 16};
    // A row at eye height: whole at 20 m; then 13.75, 9.75, 7.75, 5.75, 3.75 and 1.75 sixteenths visible, where 7.75
    // is too little to cast; then hidden beyond the end. A row above it: hidden before the beginning at 3 m, then 3, 7,
    // 9 and 13 sixteenths visible, where 7 is too little to cast and 9 enough.
    constexpr std::array<float, 8> fading_out{20, 26.25F, 30.25F, 32.25F, 34.25F, 36.25F, 38.25F, 42};
    constexpr std::array<float, 5> fading_in{3, 5.5F, 7.5F, 8.5F, 10.5F};
    const auto share = [&](float d) {
        const auto rise = d < range.begin ? 0. : std::min(1., (double(d) - range.begin) / range.begin_margin);
        const auto fall = d > range.end ? 0. : std::min(1., (double(range.end) - d) / range.end_margin);
        return std::min(rise, fall);
    };
    // A sun from the upper left behind the camera: each quad, which faces the camera, shadows the ground behind it,
    // which the camera sees beneath the quad.
    anima::Environment lighting;
    lighting.sun.direction = {-1, 1, 1};
    lighting.sun.irradiance = {2.5F, 2.5F, 2.5F};
    lighting.fill.irradiance = {};
    lighting.ambient_sky = lighting.ambient_ground = {.25F, .25F, .25F};
    // One cascade over the 60 m of ground in view.
    lighting.shadow_cascades.enabled = true;
    lighting.shadow_cascades.count = 1;
    lighting.shadow_cascades.distance = 60;
    auto unshadowed = lighting;
    unshadowed.shadow_cascades.enabled = false;

    anima::CustomMaterialDefinition definition;
    definition.name = "fade";
    definition.vertex_shader = words(fade_vertex);
    definition.fragment_shader = words(fade_fragment);
    definition.shadow_vertex_shader = words(fade_shadow_vertex);
    const auto fade = std::make_shared<const anima::CustomMaterial>(std::move(definition));
    // A unit quad, which each object or placement scales to cover the same angle at its distance.
    const auto quad = blending_test::facing(blending_test::opaque({.9, .25, .2}, true), {0, 0, 0}, 1, 1);
    const auto ground = blending_test::horizontal(blending_test::opaque({.6, .6, .6}, true), {0, 0, -30}, 30, 30);
    std::vector<anima::Mat4> placed;
    std::vector<float> distances;
    // Places the quads of one row @p elevation radians above eye height, 0.12 radians apart around straight ahead.
    const auto row = [&](std::span<const float> row_distances, float elevation) {
        for (std::size_t i = 0; i < row_distances.size(); ++i) {
            const float angle = (float(i) - float(row_distances.size() - 1) / 2) * .12F, d = row_distances[i];
            auto m = anima::identity();
            m[0] = m[5] = m[10] = .04F * d;
            anima::set_translation(m,
                                   {eye.x + d * std::sin(angle) * std::cos(elevation), eye.y + d * std::sin(elevation),
                                    eye.z - d * std::cos(angle) * std::cos(elevation)});
            placed.push_back(m);
            distances.push_back(d);
        }
    };
    row(fading_out, 0);
    row(fading_in, .22F);
    auto empty = std::make_shared<anima::Scene>(), standard = std::make_shared<anima::Scene>(),
         separate = std::make_shared<anima::Scene>(), together = std::make_shared<anima::Scene>(),
         casters = std::make_shared<anima::Scene>();
    for (const auto &scene : {empty, standard, separate, together, casters})
        (void)scene->add(ground);
    std::vector<anima::Scene::Id> plain;
    for (std::size_t i = 0; i < placed.size(); ++i) {
        plain.push_back(standard->add(quad));
        standard->set_transform(plain.back(), placed[i]);
        standard->set_visibility_range(plain.back(), range);
        const auto custom = add(*separate, quad, fade);
        separate->set_transform(custom, placed[i]);
        separate->set_visibility_range(custom, range);
        // The quads that cast, drawn whole.
        if (share(distances[i]) > .5)
            casters->set_transform(casters->add(quad), placed[i]);
    }
    const auto field = add(*together, quad, fade);
    const auto placements = anima::MeshPlacements::create(quad, placed);
    require(placements->clusters().size() == 1, "The faded copies do not form one cluster");
    together->set_placements(field, placements);
    together->set_visibility_range(field, range);

    // Without shadows, a capture differs from the frame without quads only where a quad draws.
    harness.render("fade-empty", {empty}, view, unshadowed);
    const auto covered = [&](const std::string &name) {
        const auto &image = harness.images[name], &nothing = harness.images["fade-empty"];
        std::vector<bool> mask(image.width * image.height);
        for (std::size_t y = 0; y < image.height; ++y)
            for (std::size_t x = 0; x < image.width; ++x)
                mask[y * image.width + x] = gpu_check::pixel(image, x, y) != gpu_check::pixel(nothing, x, y);
        return mask;
    };
    for (const auto id : plain)
        standard->set_visibility_range(id, {});
    harness.render("fade-whole", {standard}, view, unshadowed);
    const auto whole = covered("fade-whole");
    for (const auto id : plain)
        standard->set_visibility_range(id, range);
    harness.render("fade-standard", {standard}, view, unshadowed);
    const auto culled = harness.stats.range_culled;
    const auto quads = covered("fade-standard");
    const auto drawn = std::count(quads.begin(), quads.end(), true);
    harness.images.require(drawn > 0 && drawn < std::count(whole.begin(), whole.end(), true),
                           "The visibility range dissolved no pixel of the standard quads", {"fade-standard"});
    harness.render("fade-separate", {separate}, view, unshadowed);
    require(culled == 2 && harness.stats.range_culled == 2,
            "The quads before the range's beginning and beyond its end were not culled whole");
    harness.render("fade-placed", {together}, view, unshadowed);
    require(harness.stats.range_culled == 0, "The range culled the cluster of faded copies");
    for (const std::string name : {"fade-separate", "fade-placed"}) {
        const auto mask = covered(name);
        std::size_t mismatched = 0;
        for (std::size_t i = 0; i < mask.size(); ++i)
            mismatched += mask[i] != quads[i];
        std::cout << "CUSTOM fading: " << name << " covers " << std::count(mask.begin(), mask.end(), true)
                  << " pixels, the standard material " << drawn << ", " << mismatched << " differently\n";
        harness.images.require(mismatched == 0,
                               name + " dissolves " + std::to_string(mismatched) +
                                   " pixels differently from the standard material",
                               {"fade-standard", name});
    }

    // With shadows, the pixels outside the quads differ from the unshadowed frame only by shadows on the ground,
    // which every material must leave alike, and which the casting quads drawn whole leave too. Outside the footprint
    // of every quad drawn whole, nothing but the shadows can differ.
    harness.render("fade-standard-shadowed", {standard}, view, lighting);
    harness.render("fade-separate-shadowed", {separate}, view, lighting);
    harness.render("fade-placed-shadowed", {together}, view, lighting);
    harness.render("fade-casters-shadowed", {casters}, view, lighting);
    const auto &without = harness.images["fade-standard"], &reference = harness.images["fade-standard-shadowed"];
    std::size_t shadowed = 0;
    for (std::size_t y = 0; y < reference.height; ++y)
        for (std::size_t x = 0; x < reference.width; ++x)
            shadowed +=
                !quads[y * reference.width + x] && gpu_check::pixel(reference, x, y) != gpu_check::pixel(without, x, y);
    harness.images.require(shadowed > 0, "The standard quads cast no shadow", {"fade-standard-shadowed"});
    for (const std::string name : {"fade-separate-shadowed", "fade-placed-shadowed", "fade-casters-shadowed"}) {
        const bool cast_whole = name == "fade-casters-shadowed";
        const auto &outside = cast_whole ? whole : quads;
        const auto &image = harness.images[name];
        std::size_t mismatched = 0;
        for (std::size_t y = 0; y < image.height; ++y)
            for (std::size_t x = 0; x < image.width; ++x)
                mismatched +=
                    !outside[y * image.width + x] && gpu_check::pixel(image, x, y) != gpu_check::pixel(reference, x, y);
        std::cout << "CUSTOM fading: outside the quads, " << name << " differs from the standard material in "
                  << mismatched << " pixels, where the standard quads shadow " << shadowed << '\n';
        harness.images.require(mismatched == 0,
                               (cast_whole ? "The quads that more than half of draws, drawn whole, cast shadows that "
                                             "differ from the faded quads' in "
                                           : name + " casts shadows that differ from the standard material's in ") +
                                   std::to_string(mismatched) + " pixels",
                               {"fade-standard-shadowed", name});
    }
    harness.images.discard({"fade-empty", "fade-whole", "fade-standard", "fade-separate", "fade-placed",
                            "fade-standard-shadowed", "fade-separate-shadowed", "fade-placed-shadowed",
                            "fade-casters-shadowed"});
}

// Two quads of one shape and place hand over across 12 to 20 m, as two models of one thing do: a red one, whole
// nearer, dissolves out across its range's end margin while a blue one dissolves in across its begin margin. At 13, 16
// and 19 m, where the red one draws 14, 8 and 2 sixteenths and the blue one the rest, each midway between two of the
// dither's thresholds, the two keep complementary pixels: drawn alone, they cover disjoint parts of the quads'
// footprint that together fill it, drawn together every pixel of the footprint shows one of them, and in each whole 4
// by 4 cell of the dither the red one keeps as many pixels as its share gives. The standard material and the custom
// fade material hand over alike.
inline void check_crossfade(Harness &harness) {
    const auto aspect = harness.aspect();
    const anima::VisibilityRange outgoing{0, 20, 0, 8}, incoming{12, std::numeric_limits<float>::infinity(), 8, 0};
    const anima::Vec3 red_factor{.8F, .1F, .1F}, blue_factor{.1F, .2F, .8F};
    const auto quad = blending_test::facing(blending_test::opaque({1, 1, 1}), {0, 0, 0}, 2, 2);
    anima::CustomMaterialDefinition definition;
    definition.name = "fade";
    definition.vertex_shader = words(fade_vertex);
    definition.fragment_shader = words(fade_fragment);
    const auto fade = std::make_shared<const anima::CustomMaterial>(std::move(definition));
    for (const bool custom : {false, true}) {
        const std::string path = custom ? "custom" : "standard";
        // Adds the quad with @p factor and @p range, drawn by the fade material on the custom path.
        const auto add_quad = [&](anima::Scene &scene, anima::Vec3 factor, const anima::VisibilityRange &range) {
            const auto id = scene.add(quad);
            scene.set_material_factor(id, 0, factor);
            if (custom)
                scene.set_custom_material(id, 0, fade);
            scene.set_visibility_range(id, range);
        };
        auto empty = std::make_shared<anima::Scene>(), whole = std::make_shared<anima::Scene>(),
             out = std::make_shared<anima::Scene>(), in = std::make_shared<anima::Scene>(),
             both = std::make_shared<anima::Scene>();
        add_quad(*whole, red_factor, {});
        add_quad(*out, red_factor, outgoing);
        add_quad(*in, blue_factor, incoming);
        add_quad(*both, red_factor, outgoing);
        add_quad(*both, blue_factor, incoming);
        for (const int distance : {13, 16, 19}) {
            const auto view = anima::perspective(std::numbers::pi_v<float> / 4, aspect, .1F, 100) *
                              anima::look_at({0, 0, float(distance)}, {0, 0, 0});
            const auto name = "crossfade-" + path + "-" + std::to_string(distance);
            for (const auto &[suffix, scene] : {std::pair{"-empty", empty}, std::pair{"-whole", whole},
                                                std::pair{"-out", out}, std::pair{"-in", in}, std::pair{"-both", both}})
                harness.render(name + suffix, {scene}, view);
            const auto &nothing = harness.images[name + "-empty"], &footprint = harness.images[name + "-whole"],
                       &fading_out = harness.images[name + "-out"], &fading_in = harness.images[name + "-in"],
                       &together = harness.images[name + "-both"];
            const auto names = std::vector<std::string>{name + "-whole", name + "-out", name + "-in", name + "-both"};
            const auto width = nothing.width, height = nothing.height;
            const auto drawn = [&](const gpu_check::Image &image, std::size_t x, std::size_t y) {
                return gpu_check::pixel(image, x, y) != gpu_check::pixel(nothing, x, y);
            };
            std::size_t covered = 0, both_drawn = 0, neither = 0, stray = 0, wrong = 0;
            for (std::size_t y = 0; y < height; ++y)
                for (std::size_t x = 0; x < width; ++x) {
                    const bool inside = drawn(footprint, x, y), out_drawn = drawn(fading_out, x, y),
                               in_drawn = drawn(fading_in, x, y);
                    covered += inside;
                    both_drawn += out_drawn && in_drawn;
                    neither += inside && !out_drawn && !in_drawn;
                    stray += !inside && (out_drawn || in_drawn || drawn(together, x, y));
                    // Together, each pixel shows the quad that draws it alone.
                    wrong += inside && gpu_check::pixel(together, x, y) !=
                                           gpu_check::pixel(out_drawn ? fading_out : fading_in, x, y);
                }
            // In each 4 by 4 cell of the dither inside the footprint, the red quad keeps one pixel per sixteenth of its
            // share.
            const auto expected = std::size_t(2 * (20 - distance));
            std::size_t cells = 0, uneven = 0;
            for (std::size_t top = 0; top + 4 <= height; top += 4)
                for (std::size_t left = 0; left + 4 <= width; left += 4) {
                    std::size_t inside = 0, out_drawn = 0;
                    for (std::size_t y = top; y < top + 4; ++y)
                        for (std::size_t x = left; x < left + 4; ++x) {
                            inside += drawn(footprint, x, y);
                            out_drawn += drawn(fading_out, x, y);
                        }
                    if (inside < 16)
                        continue;
                    ++cells;
                    uneven += out_drawn != expected;
                }
            const auto report = name + ": of " + std::to_string(covered) + " footprint pixels, " +
                                std::to_string(both_drawn) + " drawn by both quads, " + std::to_string(neither) +
                                " by neither and " + std::to_string(wrong) + " showing the wrong quad together; " +
                                std::to_string(stray) + " drawn outside it; " + std::to_string(uneven) + " of " +
                                std::to_string(cells) + " whole cells without " + std::to_string(expected) +
                                " red pixels";
            std::cout << "CUSTOM crossfade " << report << '\n';
            harness.images.require(covered > 0 && cells > 0 && both_drawn == 0 && neither == 0 && wrong == 0 &&
                                       stray == 0 && uneven == 0,
                                   "Crossfading quads do not keep complementary pixels: " + report, names);
            if (custom)
                for (const auto *suffix : {"-out", "-in", "-both"}) {
                    const auto standard_name = "crossfade-standard-" + std::to_string(distance) + suffix;
                    harness.images.require_same(standard_name, name + suffix,
                                                "The custom fade material crossfades unlike the standard material");
                }
        }
    }
    for (const int distance : {13, 16, 19})
        for (const std::string path : {"standard", "custom"})
            for (const auto *suffix : {"-empty", "-whole", "-out", "-in", "-both"})
                harness.images.discard({"crossfade-" + path + "-" + std::to_string(distance) + suffix});
}

// A blended custom material that reads raw opaque depth sees the reversed depth that the renderer stored: 0 where
// nothing opaque drew, and for an opaque plane at distance z from the eye along the view, n (f - z) / ((f - n) z) for
// the near and far planes n and f, which is greater for a nearer plane than for a farther one.
inline void check_opaque_depth(Harness &harness) {
    const auto view = blending_test::perspective_view(harness.aspect());
    constexpr double near_plane = .1, far_plane = 50, scale = 20;
    const auto depth_at = [&](double z) { return near_plane * (far_plane - z) / ((far_plane - near_plane) * z); };
    anima::CustomMaterialDefinition definition;
    definition.name = "depth probe";
    definition.vertex_shader = words(surface_vertex);
    definition.fragment_shader = words(depth_fragment);
    definition.blend = anima::CustomBlend::blended;
    const auto probe = std::make_shared<const anima::CustomMaterial>(std::move(definition));
    require(probe->reads_opaque_depth(), "The depth probe does not read opaque depth");
    auto scene = std::make_shared<anima::Scene>();
    (void)scene->add(blending_test::facing(blending_test::opaque({.9, .5, .1}), {-1.5F, 0, -4}, 1, 2));
    (void)scene->add(blending_test::facing(blending_test::opaque({.1, .6, .9}), {1.5F, 0, -9}, 1, 2));
    (void)add(*scene, surface({0, 0, -2}, 1.2F, .6F), probe);
    harness.render("opaque-depth", {scene}, view);
    require(harness.stats.opaque_inputs, "A frame that draws the depth probe did not copy opaque depth");
    const auto near_depth = depth_at(4), far_depth = depth_at(9);
    require(near_depth > far_depth && far_depth > 0, "The expected depths are not ordered");
    // Seen through the probe, the point 0.4 m left of center lies on the near plane, 0.4 m right on the far one, and
    // 0.45 m up on neither.
    const anima::Vec3 on_near{-.4F, 0, -2}, on_far{.4F, 0, -2}, on_nothing{0, .45F, -2};
    harness.expect("opaque-depth", view, on_near, {scale * near_depth, 0, 0}, "Opaque depth of the plane 4 m away");
    harness.expect("opaque-depth", view, on_far, {scale * far_depth, 0, 0}, "Opaque depth of the plane 9 m away");
    harness.expect("opaque-depth", view, on_nothing, {0, .5, 0}, "Opaque depth where nothing opaque drew");
    const auto &image = harness.images["opaque-depth"];
    const auto at_near = blending_test::pixel_of(view, on_near, image),
               at_far = blending_test::pixel_of(view, on_far, image);
    const auto nearer = gpu_check::pixel(image, at_near[0], at_near[1]),
               farther = gpu_check::pixel(image, at_far[0], at_far[1]);
    harness.images.require(nearer[0] > farther[0] && farther[0] > 0,
                           "Opaque depth is not greater for the nearer plane: red " + std::to_string(nearer[0]) +
                               " at 4 m, " + std::to_string(farther[0]) + " at 9 m",
                           {"opaque-depth"});
    harness.images.discard({"opaque-depth"});
}

inline int run(int argc, char **argv) {
    require(argc == 3, "Usage: consumer --custom-materials OUTPUT");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    Harness harness(argv[2]);
    check_rejections(harness);
    check_inputs(harness);
    check_opaque(harness);
    check_sorting(harness);
    check_time(harness);
    check_water(harness);
    check_opaque_depth(harness);
    check_skinning(harness);
    check_shadows(harness);
    check_placements(harness);
    check_fading(harness);
    check_crossfade(harness);
    harness.finish();
    std::cout << "PASS custom materials: application SPIR-V reading the frame inputs, in the opaque pass and sorted "
                 "with blended draws, a time-driven effect, water that reads opaque depth and color copied only when "
                 "drawn, raw reversed opaque depth, skinning, shadows only from a depth-only variant, placed copies, "
                 "and fading that dissolves, casts and crossfades as the standard material does\n";
    return 0;
}
} // namespace custom_material_test
