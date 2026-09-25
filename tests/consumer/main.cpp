#ifdef CONSUMER_RUNTIME
#include "runtime.hpp"
#endif
#ifdef CONSUMER_AUDIO_OUTPUT
#include "audio_output.hpp"
#endif
#ifdef CONSUMER_PHYSICS2D
#include "physics2d.hpp"
#endif
#if defined(CONSUMER_UI_DOCUMENTS) || defined(CONSUMER_UI_SCENE)
#include "documents.hpp"
#endif
#ifdef CONSUMER_PHYSICS
#include "physics.hpp"
#endif
#ifdef CONSUMER_PHYSICS_SCENE
#include <anima/physics_scene.hpp>
#endif
// A complete tiny application using public targets/headers, copied out of the
// repository by the consumer test. There are no includes of engine implementation.
#include <anima/audio.hpp>
#include <anima/core/capabilities.hpp>
#include <anima/core/fixed_step.hpp>
#include <anima/core/heightfield.hpp>
#include <array>
#include <iostream>
#include <stdexcept>
#ifdef CONSUMER_ASSETS
#include "audio_scene.hpp"
#include "camera.hpp"
#include "lifecycle.hpp"
#include "lighting.hpp"
#include "prefab_variant.hpp"
#include "presentation.hpp"
#include "references.hpp"
#include "scene_objects.hpp"
#include "scene_set.hpp"
#include <anima/assets/render_visibility.hpp>
#include <anima/scene.hpp>
#endif
#include "input.hpp"
#include "navigation.hpp"
#ifdef CONSUMER_DESKTOP
#include "culling.hpp"
#include "environment.hpp"
#include "foliage.hpp"
#include "replacement.hpp"
#include "resources.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <anima/desktop/vulkan_renderer.hpp>
#include <chrono>
#endif
#ifdef CONSUMER_UI
#include "ui.hpp"
#endif

namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
#ifdef CONSUMER_ASSETS
std::shared_ptr<const anima::Asset> triangle() {
    auto asset = std::make_shared<anima::Asset>();
    asset->nodes.resize(1);
    asset->nodes[0].name = "triangle";
    asset->materials.push_back({"surface", {1, 1, 1}, -1});
    anima::SourcePrimitive primitive;
    primitive.material = 0;
    for (const auto position : {anima::Vec3{-.4F, 0, 0}, anima::Vec3{.4F, 0, 0}, anima::Vec3{0, 1, 0}}) {
        anima::SourceVertex vertex;
        vertex.position = position;
        vertex.normal = {0, 0, 1};
        primitive.vertices.push_back(vertex);
    }
    asset->primitives.push_back(primitive);
    return asset;
}
#endif
#ifdef CONSUMER_DESKTOP
void render(anima::SceneSet &scenes, anima::SceneRef instances, anima::Scene::Id left, anima::Scene::Id right,
            const std::shared_ptr<const anima::Asset> &asset, const std::filesystem::path &output) {
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    require(SDL_Init(SDL_INIT_VIDEO), "SDL initialization failed");
    struct Quit {
        ~Quit() { SDL_Quit(); }
    } quit;
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window{
        SDL_CreateWindow("Anima external consumer smoke", 800, 600, SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY),
        SDL_DestroyWindow};
    require(bool(window), "SDL window creation failed");
    std::filesystem::create_directories(output);
    const auto bounds = instances->bounds();
    auto overlay = scenes.create("overlay");
    auto right_object = overlay->create("Right", instances->object(right).renderer().mesh());
    right_object.transform().set_matrix(instances->object(right).world_matrix());
    instances->object(right).destroy();
    auto cameras = scenes.create("cameras");
    auto eye = cameras->create("eye");
    auto lens = eye.add_component<anima::Camera>();
    auto selection = cameras->create("view").add_component<anima::CameraView>();
    selection->camera = eye;
    anima::RendererOptions options;
    options.scenes = scenes.render_scenes();
    options.validation = true;
    options.capture = output / "consumer-start.ppm";
    anima::VulkanRenderer renderer(window.get(), options);
    anima::OrbitCamera camera;
    camera.frame(bounds.minimum, bounds.maximum);
    int width = 0, height = 0;
    require(SDL_GetWindowSizeInPixels(window.get(), &width, &height) && width > 0 && height > 0,
            "Drawable dimensions unavailable");
    eye.set_world_matrix(anima::inverse(anima::look_at(camera.position(), camera.target)));
    anima::CameraSettings settings;
    settings.near_plane = camera.radius * .01F;
    settings.far_plane = camera.radius * 50;
    lens->configure(settings);
    const auto aspect = float(width) / float(height);
    renderer.set_view(anima::view_matrix(scenes, aspect));
    anima::ComponentCodecs codecs;
    anima::add_camera_component_codecs(codecs);
    const auto saved_camera = anima::serialize_scene(cameras.get(), {}, codecs);
    const auto started = std::chrono::steady_clock::now();
    unsigned frames = 0;
    bool requested = false;
    while (frames < 30) {
        require(std::chrono::steady_clock::now() - started < std::chrono::seconds(20), "Consumer GPU watchdog expired");
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                throw std::runtime_error("Consumer smoke interrupted");
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                renderer.request_resize();
        }
        if (frames == 15 && !requested) {
            requested = true;
            // Moving an empty parent must update the child's GPU palette/bounds.
            auto parent = instances->object(left).parent();
            require(parent.has_value(), "Consumer hierarchy lost its parent");
            parent->transform().set_position({0, .1F, 0});
            if (!asset->animations.empty()) {
                const auto &clip = asset->animations.back();
                instances->object(left).renderer().set_pose(anima::sample_pose(*asset, &clip, clip.duration * .5));
            } else {
                auto pose = anima::sample_pose(*asset);
                pose.world[0][13] = .25F;
                instances->object(left).renderer().set_pose(pose);
            }
            instances->object(left).renderer().set_material_factor(0, {.8F, .3F, .2F});
            renderer.request_capture(output / "consumer-updated.ppm");
        }
        if (renderer.draw())
            ++frames;
        else
            SDL_Delay(10);
    }
    require(right_object.renderer().mesh() == instances->instance(left).asset,
            "Scenes stopped sharing geometry during rendering");
    require(renderer.resource_stats().mesh_uploads == 1, "Multi-scene selection uploaded a shared mesh twice");
    auto capture = [&](const char *name) {
        renderer.request_capture(output / name);
        const auto end = frames + 10;
        while (frames < end) {
            require(std::chrono::steady_clock::now() - started < std::chrono::seconds(20),
                    "Unload GPU watchdog expired");
            if (renderer.draw())
                ++frames;
            else
                SDL_Delay(10);
        }
    };
    settings.projection = anima::CameraProjection::orthographic;
    settings.orthographic_height = camera.radius * 2.5F;
    lens->configure(settings);
    renderer.set_view(anima::view_matrix(scenes, aspect));
    capture("consumer-orthographic.ppm");
    eye.set_position(eye.position() + anima::Vec3{.5F, 0, 0});
    renderer.set_view(anima::view_matrix(scenes, aspect));
    capture("consumer-camera-moved.ppm");
    cameras = scenes.replace(cameras, saved_camera, {}, codecs);
    require(!eye.valid() && !selection && !lens, "Camera replacement retained runtime handles");
    renderer.set_view(anima::view_matrix(scenes, aspect));
    capture("consumer-camera-restored.ppm");
    selection = cameras->components<anima::CameraView>().front();
    selection->camera = eye; // Stale selection must fail before renderer publication.
    bool rejected = false;
    try {
        renderer.set_view(anima::view_matrix(scenes, aspect));
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "Stale camera selection was accepted");
    capture("consumer-camera-rejected.ppm");
    selection->camera = cameras->components<anima::Camera>().front().object();
    scenes.unload(instances);
    require(!instances && options.scenes.front()->size() == 0 && right_object.valid(),
            "Unloading one scene affected another scene or retained objects");
    capture("consumer-partial.ppm");
    require(renderer.resource_stats().instances == 1, "Retained selection lost the surviving scene");
    renderer.set_scenes({overlay.render_scene()});
    capture("consumer-overlay.ppm");
    scenes.unload(overlay);
    capture("consumer-unloaded.ppm");
    renderer.set_scenes({});
    capture("consumer-cleared.ppm");
    const auto resources = renderer.resource_stats();
    require(resources.instances == 0 && resources.draw_calls == 0 && resources.shadow_draw_calls == 0,
            "Unloaded scenes still submitted mesh draws");
    const auto stats = renderer.shutdown();
    require(stats.presented_frames == 110 && stats.capture_count == 10 && !stats.validation_errors &&
                !stats.validation_warnings,
            "External consumer GPU validation failed");
    std::cout << "PASS external GPU consumer: 110 frames, 10 captures, scene cameras and multi-scene partial unload; "
                 "validation_warnings=0 validation_errors=0\n";
}
#endif
} // namespace
int main(int argc, char **argv) {
#ifdef CONSUMER_RUNTIME
    runtime_consumer::run();
#endif
#if defined(CONSUMER_UI_DOCUMENTS) || defined(CONSUMER_UI_SCENE)
    documents_consumer::run();
#endif

#ifdef CONSUMER_PHYSICS
    consume_physics();
#endif
#ifdef CONSUMER_PHYSICS_SCENE
    {
        anima::physics::World physics;
        anima::Scene scene;
        auto object = scene.create("independent physics object");
        object.add_component<anima::physics::RigidBody>(physics);
        anima::physics::step(scene, physics, 1. / 60);
        object.destroy();
        if (physics.size())
            return 1;
    }
#endif

    try {
        consume_navigation();
        consume_input();
#ifdef CONSUMER_AUDIO_OUTPUT
        consume_audio_output();
#endif
#ifdef CONSUMER_PHYSICS2D
        consume_physics2d();
#endif
        anima::Audio audio(8000);
        auto sound = audio.sound(anima::AudioClip::pcm({.25F, -.25F}, 2, 8000));
        sound.play();
        std::array<float, 2> mixed{};
        audio.render(mixed);
        require(mixed[0] == .25F && mixed[1] == -.25F && !sound.playing(),
                "Independent core consumer could not mix stereo audio");
#ifdef CONSUMER_UI
        ui_test::test_attribute_conversion();
        if (argc > 1 && std::string_view(argv[1]) == "--ui")
            return ui_test::run(argc, argv);
#endif
#ifdef CONSUMER_DESKTOP
        if (argc > 1 && std::string_view(argv[1]) == "--environment")
            return environment_test::run(argc, argv);
        if (argc > 1 && std::string_view(argv[1]) == "--foliage")
            return foliage_test::run(argc, argv);
        if (argc > 1 && std::string_view(argv[1]) == "--culling")
            return culling_test::run(argc, argv);
        if (argc > 1 && std::string_view(argv[1]) == "--resources")
            return resource_test::run(argc, argv);
        if (argc > 1 && std::string_view(argv[1]) == "--replace")
            return replacement_test::run(argc, argv);
#endif
        anima::FixedStepClock clock;
        const std::array variants{anima::ActionVariant{"observe", {}}, anima::ActionVariant{"signal", {"beacon"}}};
        require(anima::resolve_action(variants, {"beacon"}) == "signal" &&
                    anima::resolve_action(variants, {}) == "observe",
                "Public core capability selection failed");
        require(clock.advance(clock.step() * 3).steps == 3, "Public core target failed");
        const anima::Heightfield terrain{2, 2, 0, 0, 1, 1, {0, 1, 0, 1}};
        anima::validate_heightfield(terrain.view());
        const auto ground = anima::sample_heightfield(terrain.view(), .5F, .5F);
        const auto hit = anima::intersect_heightfield(terrain.view(), {.5F, 2, .5F}, {.5F, -1, .5F});
        const auto move = anima::move_on_heightfield(terrain.view(), {0, 0, .5F}, {1, 0, .5F}, .5);
        require(ground && ground->height == .5F && hit && hit->fraction == .5 && std::abs(move.distance - .5) < 1e-6,
                "Public headless terrain API failed");
#ifdef CONSUMER_ASSETS
        presentation_test::run();
        scene_objects_test::run();
        lifecycle_test::run();
        references_test::run();
        scene_set_test::run();
        prefab_variant_test::run();
        consume_audio_scene();
        camera_consumer::run();
        lighting_consumer::run();
        auto asset = triangle();
        // Also exercise public resource bounds/frustum queries without requiring
        // a desktop backend or engine tools in a downstream application.
        const auto compiled = anima::Mesh::compile(*asset);
        anima::Scene resource_scene;
        const auto instance = resource_scene.add(compiled);
        const anima::RenderFrustum frustum(anima::identity());
        require(frustum.intersects(resource_scene.instance(instance).bounds),
                "Public frustum rejected the visible resource");
        auto outside = anima::identity();
        outside[12] = 4;
        resource_scene.set_pose(instance, compiled->rest_pose(), outside);
        require(!frustum.intersects(resource_scene.instance(instance).bounds),
                "Public frustum missed changed instance bounds");
#ifdef CONSUMER_DESKTOP
        if (argc > 2)
            asset = anima::load_asset(argv[2]);
#endif
        anima::SceneSet scenes;
        auto instances = scenes.create("instances");
        const auto mesh = anima::Mesh::compile(*asset);
        auto left = instances->create("Left", mesh), right = instances->create("Right", mesh);
        left.set_parent(instances->create("Left group"));
        left.transform().set_position({-.8F, 0, 0});
        right.transform().set_position({.8F, 0, 0});
        left.renderer().set_material_factor(0, {.2F, .7F, .9F});
        const auto a = left.id(), b = right.id();
        require(instances->instances().size() == 2, "Public scene composition failed");
        require(instances->instance(a).factors[0].x == .2F &&
                    instances->instance(b).factors[0].x == asset->materials[0].factor.x,
                "Instance appearance isolation failed");
#ifdef CONSUMER_DESKTOP
        if (argc > 1)
            render(scenes, instances, a, b, asset, argv[1]);
#endif
#endif
        (void)argc;
        (void)argv;
        std::cout << "PASS external consumer using public Anima targets\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
