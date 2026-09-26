#pragma once
#include "reference.hpp"
#include "resources.hpp"
#include <anima/lighting.hpp>
#include <anima/scene_set.hpp>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <numbers>
#include <string_view>
namespace environment_test {
inline std::shared_ptr<const anima::Asset> fixture(bool sloped = false) {
    auto asset = std::make_shared<anima::Asset>();
    asset->nodes.resize(1);
    anima::Material ground;
    ground.name = "ground";
    ground.factor = {.5F, .5F, .5F};
    asset->materials.push_back(ground);
    anima::Material canopy;
    canopy.name = "masked canopy";
    canopy.factor = {.4F, .1F, .5F};
    canopy.texture = 0;
    canopy.alpha_mode = anima::AlphaMode::mask;
    asset->materials.push_back(canopy);
    anima::Texture alpha{2, 2, {255, 255, 255, 255, 255, 255, 255, 0, 255, 255, 255, 0, 255, 255, 255, 255}, {}};
    alpha.sampler.mag = alpha.sampler.min = anima::Filter::nearest;
    alpha.sampler.mipmapped = false;
    asset->textures.push_back(alpha);
    for (int layer = 0; layer < (sloped ? 1 : 2); ++layer) {
        const float extent = layer == 0 ? 8.F : 1.5F, y = layer == 0 ? 0.F : 3.F;
        anima::SourcePrimitive p;
        p.material = layer;
        for (const auto uv : {std::array<float, 2>{0, 0}, {0, 1}, {1, 1}, {0, 0}, {1, 1}, {1, 0}}) {
            anima::SourceVertex v;
            v.position = {(uv[0] * 2 - 1) * extent, y, (uv[1] * 2 - 1) * extent};
            // Deliberately retain a different shading normal. Shadow receiver
            // depth must follow actual geometry, not a smoothed/normal-mapped normal.
            if (sloped)
                v.position.y = .35F * v.position.x;
            v.normal = {0, 1, 0};
            v.uv = uv;
            v.tangent = {1, 0, 0, -1};
            p.vertices.push_back(v);
        }
        asset->primitives.push_back(std::move(p));
    }
    return asset;
}
inline std::shared_ptr<const anima::Asset> curved_fixture() {
    auto asset = std::make_shared<anima::Asset>();
    asset->nodes.resize(1);
    anima::Material material;
    material.name = "smooth coarse cylinder";
    material.factor = {.5F, .5F, .5F};
    material.roughness = 1;
    asset->materials.push_back(material);
    anima::SourcePrimitive primitive;
    primitive.material = 0;
    constexpr int sides = 12;
    const auto vertex = [](int side, float y) {
        const float angle = side * (2 * std::numbers::pi_v<float> / sides);
        anima::SourceVertex v;
        v.normal = {std::sin(angle), 0, std::cos(angle)};
        v.position = {.3F * v.normal.x, y, .3F * v.normal.z};
        v.tangent = {std::cos(angle), 0, -std::sin(angle), 1};
        return v;
    };
    for (int side = 0; side < sides; ++side) {
        const auto a = vertex(side, -.4F), b = vertex(side + 1, -.4F);
        const auto c = vertex(side + 1, .4F), d = vertex(side, .4F);
        for (const auto &v : {a, b, c, a, c, d})
            primitive.vertices.push_back(v);
        for (const float y : {-.4F, .4F}) {
            anima::SourceVertex center;
            center.position = {0, y, 0};
            center.normal = {0, y > 0 ? 1.F : -1.F, 0};
            auto first = vertex(y > 0 ? side : side + 1, y);
            auto second = vertex(y > 0 ? side + 1 : side, y);
            first.normal = second.normal = center.normal;
            for (const auto &v : {center, first, second})
                primitive.vertices.push_back(v);
        }
    }
    asset->primitives.push_back(std::move(primitive));
    return asset;
}
inline int run(int argc, char **argv) {
    using resource_test::require;
    using anima::operator*;
    require(argc == 3, "Usage: consumer --environment OUTPUT");
    const std::filesystem::path output = argv[2];
    std::filesystem::create_directories(output);
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    require(SDL_Init(SDL_INIT_VIDEO), "Environment SDL initialization failed");
    struct Quit {
        ~Quit() { SDL_Quit(); }
    } quit;
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window{
        SDL_CreateWindow("Anima environment verification", 800, 600, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE),
        SDL_DestroyWindow};
    require(bool(window), "Environment window failed");
    anima::RendererOptions options;
    options.validation = true;
    options.profile = true;
    anima::VulkanRenderer renderer(window.get(), options);
    const auto asset = fixture();
    const auto compiled = anima::Mesh::compile(*asset);
    auto scene = std::make_shared<anima::Scene>();
    const auto id = scene->add(compiled);
    const auto view = anima::perspective(4.F / 3, .05F, 100) * anima::look_at({0, 6, 10}, {0, 0, 0});
    renderer.set_view(view);
    renderer.set_scenes({scene});
    anima::Environment environment;
    environment.sun.direction = {0, 1, 0};
    environment.sun.radiance = {3, 2.7F, 2.3F};
    environment.fill.radiance = {0, 0, 0};
    environment.ambient_sky = {.16F, .18F, .3F};
    environment.ambient_ground = {.08F, .06F, .09F};
    environment.sky = true;
    environment.shadow.extent = 10;
    environment.shadow.depth = 30;
    environment.shadow.resolution = 1024;
    const auto start = std::chrono::steady_clock::now();
    auto capture = [&](const std::string &name) {
        if (!name.empty())
            renderer.request_capture(output / (name + ".ppm"));
        for (;;) {
            require(std::chrono::steady_clock::now() - start < std::chrono::seconds(60), "Environment watchdog");
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                        "Environment test interrupted");
                if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                    renderer.request_resize();
            }
            if (renderer.draw())
                break;
            SDL_Delay(5);
        }
    };
    renderer.set_environment(environment);
    capture("unshadowed");
    environment.shadow.enabled = true;
    renderer.set_environment(environment);
    capture("shadowed");
    require(renderer.resource_stats().shadow_draw_calls == 2, "Missing shadow casters");
    require(renderer.resource_stats().shadow_bytes > 1024, "Shadow allocation missing");
    // Resolve authored lighting through the production component graph. Geometry
    // remains independently selected above; these scenes contain only light data.
    anima::SceneSet lighting;
    auto authored = lighting.create("lighting");
    auto sun = authored->create("sun");
    sun.set_world_matrix({1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1});
    sun.add_component<anima::DirectionalLightComponent>(environment.sun.radiance);
    auto fill = authored->create("fill");
    fill.add_component<anima::DirectionalLightComponent>(environment.fill.radiance);
    auto settings_object = authored->create("environment");
    auto settings = settings_object.add_component<anima::SceneEnvironment>(
        static_cast<const anima::EnvironmentSettings &>(environment));
    settings->sun = sun;
    settings->fill = fill;
    renderer.set_environment(anima::lighting_environment(lighting));
    capture("scene-light-baseline");
    anima::ComponentCodecs light_codecs;
    anima::add_lighting_component_codecs(light_codecs);
    const auto light_document = anima::serialize_scene(authored.get(), {}, light_codecs);
    const auto sun_key = sun.key(), settings_key = settings_object.key();
    sun.set_world_matrix(anima::inverse(anima::look_at({4, 6, 1}, {0, 0, 0})));
    renderer.set_environment(anima::lighting_environment(lighting));
    capture("scene-light-rotated");
    sun.get_component<anima::DirectionalLightComponent>()->set_radiance({.2F, 1, 3});
    renderer.set_environment(anima::lighting_environment(lighting));
    capture("scene-light-tinted");
    auto changed_settings = settings->settings();
    changed_settings.exposure = .4F;
    changed_settings.fog_density = .08F;
    settings->configure(changed_settings);
    renderer.set_environment(anima::lighting_environment(lighting));
    capture("scene-light-settings");
    authored = lighting.replace(authored, light_document, {}, light_codecs);
    require(!sun.valid() && !settings.valid(), "Lighting replacement retained old component handles");
    renderer.set_environment(anima::lighting_environment(lighting));
    capture("scene-light-restored");
    settings = authored->find(settings_key).get_component<anima::SceneEnvironment>();
    settings->sun = sun;
    resource_test::rejects<std::invalid_argument>(
        [&] { renderer.set_environment(anima::lighting_environment(lighting)); });
    capture("scene-light-rejected");
    settings->sun = authored->find(sun_key);
    settings->sun.set_active(false);
    resource_test::rejects<std::invalid_argument>(
        [&] { renderer.set_environment(anima::lighting_environment(lighting)); });
    capture("scene-light-inactive");
    // Subsequent checks use the same accepted immediate environment as before.
    renderer.set_environment(environment);
    // Put the receiver and caster into different scenes, sharing the same mesh.
    const auto uploads = renderer.resource_stats().mesh_uploads;
    anima::SceneSet layers;
    auto caster = layers.create("caster");
    const auto caster_id = caster->add(compiled);
    scene->set_primitive_visible(id, 1, false);
    caster->set_primitive_visible(caster_id, 0, false);
    renderer.set_scenes({scene, caster.render_scene()});
    capture("multi-scene-shadowed");
    require(renderer.resource_stats().mesh_uploads == uploads && renderer.resource_stats().shadow_draw_calls == 2,
            "Cross-scene shadows lost a caster or duplicated resource uploads");
    renderer.set_frustum_culling(false);
    capture("multi-scene-unculled");
    renderer.set_frustum_culling(true);
    resource_test::rejects<std::invalid_argument>([&] { renderer.set_scenes({scene, scene}); });
    resource_test::rejects<std::invalid_argument>([&] { renderer.set_scenes({scene, nullptr}); });
    resource_test::rejects<std::runtime_error>(
        [&] { renderer.set_scenes({caster.render_scene()}, {anima::RendererFailureStage::ready}); });
    capture("multi-scene-rejected");
    layers.unload(caster);
    capture("multi-scene-unloaded");
    require(renderer.resource_stats().shadow_draw_calls == 1, "Unloaded scene retained shadow draws");
    scene->set_primitive_visible(id, 1, true);
    renderer.set_scenes({scene});
    renderer.set_frustum_culling(false);
    capture("shadowed-unculled");
    renderer.set_frustum_culling(true);
    auto bad = environment;
    bad.exposure = std::numeric_limits<float>::quiet_NaN();
    resource_test::rejects<std::invalid_argument>([&] { renderer.set_environment(bad); });
    capture("invalid-preserved");
    renderer.set_scenes({reference_test::scene(anima::make_mesh_snapshot(*asset, anima::sample_pose(*asset)))});
    capture("reference-shadowed");
    renderer.set_scenes({scene});
    scene->set_primitive_visible(id, 1, false);
    capture("caster-hidden");
    scene->set_primitive_visible(id, 1, true);
    environment.shadow.resolution = 512;
    renderer.set_environment(environment);
    capture("resized-shadow");
    environment.shadow.resolution = 1024;
    renderer.set_environment(environment);
    capture("restored-shadow");
    renderer.set_view(anima::perspective(4.F / 3, .05F, 2) * anima::look_at({0, 1, 1}, {0, 0, 0}));
    capture("offscreen-caster");
    require(renderer.resource_stats().culled_draws >= 1 && renderer.resource_stats().shadow_draw_calls == 2,
            "Main camera incorrectly removed an offscreen shadow caster");
    renderer.set_view(view);
    // Move the main camera continuously without moving the light's shadow region.
    // Returning to the original view must reproduce the original shadowed image.
    for (int step = 0; step <= 16; ++step) {
        const float x = step <= 8 ? -.5F * step : -4.F + .5F * (step - 8);
        renderer.set_view(anima::perspective(4.F / 3, .05F, 100) * anima::look_at({x, 6, 10}, {0, 0, 0}));
        capture(step == 8 ? "camera-left" : step == 16 ? "camera-restored" : "");
    }
    // Optional detail region: retain world coverage while refining a small
    // subject, including independent casters outside both the main camera and
    // the original world-shadow volume.
    environment.detail_shadow = environment.shadow;
    environment.detail_shadow.enabled = true;
    environment.detail_shadow.extent = 2;
    environment.detail_shadow.resolution = 1024;
    renderer.set_environment(environment);
    capture("detail-shadowed");
    require(renderer.resource_stats().shadow_draw_calls == 4, "Detail pass lost a caster");
    renderer.set_scenes({reference_test::scene(anima::make_mesh_snapshot(*asset, anima::sample_pose(*asset)))});
    capture("detail-reference-shadowed");
    renderer.set_scenes({scene});
    bad = environment;
    bad.detail_shadow.resolution = std::numeric_limits<std::uint32_t>::max();
    resource_test::rejects<std::invalid_argument>([&] { renderer.set_environment(bad); });
    capture("detail-invalid-preserved");
    environment.detail_shadow.center.x = .0001F;
    renderer.set_environment(environment);
    capture("detail-snapped");
    environment.detail_shadow.center.x = 1.25F;
    renderer.set_environment(environment);
    capture("detail-boundary");
    environment.detail_shadow.center.x = 5;
    renderer.set_environment(environment);
    capture("detail-away");
    environment.detail_shadow.center.x = 0;
    environment.shadow.enabled = false;
    renderer.set_environment(environment);
    capture("detail-only");
    renderer.set_view(anima::perspective(4.F / 3, .05F, 2) * anima::look_at({0, 1, 1}, {0, 0, 0}));
    capture("detail-offscreen-caster");
    require(renderer.resource_stats().culled_draws >= 1 && renderer.resource_stats().shadow_draw_calls == 2,
            "Main camera removed a detail-region caster");
    auto translated = anima::identity();
    translated[12] = 40;
    scene->set_pose(id, anima::sample_pose(*asset), translated);
    environment.shadow.enabled = true;
    environment.detail_shadow.center.x = 40;
    renderer.set_environment(environment);
    renderer.set_view(anima::perspective(4.F / 3, .05F, 100) * anima::look_at({40, 6, 10}, {40, 0, 0}));
    capture("detail-outside-world");
    require(renderer.resource_stats().shadow_draw_calls == 2, "Detail casters depended on world-region visibility");
    scene->set_pose(id, anima::sample_pose(*asset));
    renderer.set_view(view);
    environment.detail_shadow.enabled = false;
    renderer.set_environment(environment);
    capture("detail-disabled");
    require(renderer.resource_stats().shadow_draw_calls == 2, "Disabled detail pass still submitted geometry");
    // A disabled region keeps its 1x1 map, so only enabling it holds its resolution to the device's limits.
    constexpr auto beyond_any_device = std::numeric_limits<std::uint32_t>::max(); // A 2D image edge no GPU offers.
    constexpr std::string_view device_limit = "Shadow resolution exceeds device capabilities";
    const auto placeholder_bytes = renderer.resource_stats().shadow_bytes;
    auto oversized = environment;
    oversized.detail_shadow.resolution = beyond_any_device;
    renderer.set_environment(oversized);
    capture("");
    require(renderer.resource_stats().shadow_bytes == placeholder_bytes, "A disabled region outgrew its 1x1 map");
    oversized.detail_shadow.enabled = true;
    bool limited = false;
    try {
        renderer.set_environment(oversized);
    } catch (const std::invalid_argument &error) {
        limited = error.what() == device_limit;
    }
    require(limited, "Enabling a region beyond the device's limits was not rejected");
    capture("");
    require(renderer.resource_stats().shadow_bytes == placeholder_bytes, "A rejected environment replaced a map");
    renderer.set_environment(environment);
    const auto slope = fixture(true);
    auto slope_scene = std::make_shared<anima::Scene>();
    (void)slope_scene->add(anima::Mesh::compile(*slope));
    renderer.set_scenes({slope_scene});
    environment.sun.direction = {1, .6F, .1F};
    environment.shadow.enabled = false;
    renderer.set_environment(environment);
    capture("slope-unshadowed");
    environment.shadow.enabled = true;
    renderer.set_environment(environment);
    capture("slope-shadowed");
    renderer.set_scenes({reference_test::scene(anima::make_mesh_snapshot(*slope, anima::sample_pose(*slope)))});
    capture("slope-reference-shadowed");
    // A geometric light cosine of about .044 still needs exact receiver-plane
    // correction. Disabling that correction near grazing must not turn an
    // unobstructed, light-facing plane into an acne pattern.
    const auto slope_sun = environment.sun.direction;
    environment.sun.direction = {1, .4F, .1F};
    environment.shadow.enabled = false;
    renderer.set_scenes({slope_scene});
    renderer.set_environment(environment);
    capture("steep-slope-unshadowed");
    environment.shadow.enabled = true;
    renderer.set_environment(environment);
    capture("steep-slope-shadowed");
    renderer.set_scenes({reference_test::scene(anima::make_mesh_snapshot(*slope, anima::sample_pose(*slope)))});
    capture("steep-slope-reference-shadowed");
    environment.sun.direction = slope_sun;
    // The cylinder's 0..30-degree side is nearly tangent to this light, while
    // its interpolated radial normals still face it. No external caster exists.
    // This exposes receiver-plane extrapolation across curved geometry that a
    // single sloped plane cannot exercise.
    const auto curved = curved_fixture();
    auto curved_scene = std::make_shared<anima::Scene>();
    (void)curved_scene->add(anima::Mesh::compile(*curved));
    renderer.set_scenes({curved_scene});
    constexpr anima::Mat4 curved_projection{2 / 1.2F, 0, 0, 0, 0, -2 / .9F, 0, 0, 0, 0, -1.F / 20, 0, 0, 0, 0, 1};
    renderer.set_view(curved_projection * anima::look_at({0, 0, 3}, {0, 0, 0}));
    anima::Environment curved_environment;
    curved_environment.sun.direction = {-4, 0, 1};
    curved_environment.sun.radiance = {3, 3, 3};
    curved_environment.fill.radiance = {};
    curved_environment.ambient_sky = curved_environment.ambient_ground = curved_environment.ambient_specular = {};
    curved_environment.shadow.extent = 25;
    curved_environment.shadow.depth = 70;
    curved_environment.shadow.resolution = 2048;
    curved_environment.shadow.constant_bias = .00035F;
    curved_environment.shadow.slope_bias = .001F;
    renderer.set_environment(curved_environment);
    capture("curved-unshadowed");
    curved_environment.shadow.enabled = true;
    renderer.set_environment(curved_environment);
    capture("curved-shadowed");
    renderer.set_scenes({reference_test::scene(anima::make_mesh_snapshot(*curved, anima::sample_pose(*curved)))});
    capture("curved-reference-shadowed");
    renderer.set_environment(environment);
    renderer.set_view(view);
    renderer.set_scenes({std::make_shared<anima::Scene>()});
    capture("sky");
    environment.shadow.enabled = false;
    renderer.set_environment(environment);
    capture("sky-shadows-disabled");
    const auto stats = renderer.shutdown();
    require(!stats.validation_errors && !stats.validation_warnings, "Environment GPU validation failed");
    std::cout << "PASS environment: shadow casters, detail-pass visibility, invalid-setting rejection, lighting "
                 "replacement and retirement\n";
    return 0;
}
} // namespace environment_test
