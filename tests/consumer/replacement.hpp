#pragma once
// Standalone consumer: public APIs only. No game rules or engine implementation.
#include <SDL3/SDL.h>
#include <anima/desktop/vulkan_renderer.hpp>
#include <anima/scene.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

namespace replacement_test {
inline void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class Error, class F> void rejects(F operation) {
    bool caught = false;
    try {
        operation();
    } catch (const anima::RendererFatalError &) {
        if constexpr (std::is_same_v<Error, anima::RendererFatalError>)
            caught = true;
        else
            throw;
    } catch (const std::exception &error) {
        if (dynamic_cast<const Error *>(&error))
            caught = true;
        else
            throw;
    }
    require(caught, "Expected operation to be rejected");
}
inline anima::Asset geometry(unsigned count) {
    anima::Asset asset;
    asset.nodes.resize(count);
    for (unsigned i = 0; i < count; ++i) {
        const float x = -.85F + float(i) * 1.7F / float(count);
        anima::SourcePrimitive primitive;
        primitive.node = i;
        primitive.material = int(i);
        for (const auto position :
             {anima::Vec3{x, -.6F, .3F}, anima::Vec3{x + .45F, -.6F, .3F}, anima::Vec3{x + .2F, .6F, .3F}}) {
            anima::SourceVertex vertex;
            vertex.position = position;
            vertex.normal = {0, 0, 1};
            vertex.uv = {.5F, .5F};
            primitive.vertices.push_back(vertex);
        }
        asset.primitives.push_back(primitive);
        asset.materials.push_back({"surface", {1, 1, 1}, int(i)});
        const auto shade = static_cast<std::uint8_t>(80 + i * 60);
        asset.textures.push_back({2, 1, {shade, 200, 240, 255, shade, 200, 240, 255}, {}});
    }
    return asset;
}
inline std::shared_ptr<anima::Scene> scene(const anima::Asset &asset) {
    auto result = std::make_shared<anima::Scene>();
    (void)result->add(anima::Mesh::compile(asset));
    return result;
}
// Construction rejects a failure stage that neither it nor draw() can fire, before it needs a window or GPU. The
// texture stages fire only in an initial selection's upload, so they need RendererOptions::scenes.
inline void reject_unfireable_injection() {
    constexpr std::string_view unknown_stage = "Unknown initialization failure stage";
    constexpr std::string_view no_window = "Renderer requires an SDL window";
    using enum anima::RendererFailureStage;
    const auto beyond_enumeration = static_cast<anima::RendererFailureStage>(anima::renderer_failure_names.size());
    const auto construct = [](anima::RendererFailureStage stage, bool selection) {
        anima::RendererOptions options;
        options.fail_after = stage;
        if (selection)
            options.scenes = {std::make_shared<anima::Scene>()};
        try {
            anima::VulkanRenderer renderer(nullptr, options);
        } catch (const std::invalid_argument &error) {
            return std::string(error.what());
        }
        throw std::runtime_error("Renderer construction accepted a null window");
    };
    for (const auto stage : {vertex, index, texture, texture_upload, descriptors, palette, ready, upload_timeout,
                             device_lost, beyond_enumeration})
        if (const auto error = construct(stage, false); error != unknown_stage)
            throw std::runtime_error("Unfireable failure stage was not rejected: " + error);
    for (const auto stage : {texture, texture_upload})
        if (const auto error = construct(stage, true); error != no_window)
            throw std::runtime_error("A texture stage with an initial selection was rejected: " + error);
}
// A swapchain that fails after its predecessor was released leaves nothing to present, so the renderer
// becomes fatal instead of rebuilding it on every draw.
inline void reject_failed_swapchain(bool disable_present_fences) {
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window{
        SDL_CreateWindow("Anima swapchain failure consumer", 320, 240,
                         SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY),
        SDL_DestroyWindow};
    require(bool(window), "SDL window creation failed");
    anima::RendererOptions options;
    options.validation = true;
    options.disable_present_fences = disable_present_fences;
    options.fail_after = anima::RendererFailureStage::swapchain;
    anima::VulkanRenderer renderer(window.get(), options);
    const auto started = std::chrono::steady_clock::now();
    rejects<anima::RendererFatalError>([&] {
        // draw() returns false until the window is drawable; the first swapchain it creates fails.
        for (;;) {
            require(std::chrono::steady_clock::now() - started < std::chrono::seconds(10),
                    "Swapchain failure watchdog expired");
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
            }
            require(!renderer.draw(), "Injected swapchain failure presented a frame");
            SDL_Delay(5);
        }
    });
    rejects<anima::RendererFatalError>([&] { (void)renderer.draw(); });
    rejects<anima::RendererFatalError>([&] { renderer.set_view(anima::identity()); });
    const auto stats = renderer.shutdown();
    require(!stats.validation_errors && !stats.validation_warnings && !stats.swapchain_generations &&
                !stats.presented_frames,
            "Failed swapchain cleanup failed");
}
inline int run(int argc, char **argv) {
    require(argc >= 3, "Usage: consumer --replace OUTPUT [--no-present-fences] [--fatal STAGE] [--asset GLB]");
    const std::filesystem::path output = argv[2];
    std::filesystem::path asset;
    std::string fatal;
    anima::RendererOptions options;
    for (int i = 3; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--no-present-fences")
            options.disable_present_fences = true;
        else if (argument == "--fatal" && i + 1 < argc)
            fatal = argv[++i];
        else if (argument == "--asset" && i + 1 < argc)
            asset = argv[++i];
        else
            throw std::invalid_argument("Unknown replacement consumer option");
    }
    require(fatal.empty() || fatal == "upload-timeout" || fatal == "device-lost", "Unknown fatal injection");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    require(SDL_Init(SDL_INIT_VIDEO), "SDL initialization failed");
    struct Quit {
        ~Quit() { SDL_Quit(); }
    } quit;
    reject_failed_swapchain(options.disable_present_fences);
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window{
        SDL_CreateWindow("Anima scene replacement consumer", 640, 480,
                         SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY),
        SDL_DestroyWindow};
    require(bool(window), "SDL window creation failed");
    const auto window_id = SDL_GetWindowID(window.get());
    std::filesystem::create_directories(output);
    options.validation = true;
    options.capture = output / "empty-start.ppm";
    anima::VulkanRenderer renderer(window.get(), options);
    renderer.set_view(anima::identity());
    const auto started = std::chrono::steady_clock::now();
    unsigned frames = 0, captures = 1, generations = 1, rollbacks = 0, mutation_rejections = 0;
    const auto pump = [&] {
        require(std::chrono::steady_clock::now() - started < std::chrono::seconds(25), "Replacement watchdog expired");
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                    "Replacement smoke interrupted");
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                renderer.request_resize();
        }
        require(SDL_GetWindowID(window.get()) == window_id, "Replacement changed window identity");
    };
    const auto frame = [&] {
        for (;;) {
            pump();
            if (renderer.draw()) {
                ++frames;
                break;
            }
            SDL_Delay(5);
        }
    };
    const auto capture = [&](const std::string &name) {
        renderer.request_capture(output / (name + ".ppm"));
        frame();
        ++captures;
    };
    const auto replace = [&](std::vector<std::shared_ptr<const anima::Scene>> selection) {
        renderer.set_scenes(std::move(selection));
        ++generations;
    };
    frame();
    frame();
    const auto a_data = geometry(2);
    auto b_data = geometry(3);
    b_data.primitives.back().material = -1; // Default white descriptor survives replacements.
    auto a = scene(a_data), b = scene(b_data);
    const auto id = a->instances().front();
    std::cout << "STABLE_SWAPCHAIN_BEGIN\n";
    replace({a});
    capture("scene-a");
    if (!fatal.empty()) {
        frame(); // Leave a graphics submit pending before attempting upload.
        rejects<anima::RendererFatalError>(
            [&] { renderer.set_scenes({b}, {anima::parse_renderer_failure_stage(fatal)}); });
        rejects<anima::RendererFatalError>([&] { (void)renderer.draw(); });
        rejects<anima::RendererFatalError>([&] { renderer.set_scenes({}); });
        rejects<anima::RendererFatalError>([&] { renderer.set_view(anima::identity()); });
        rejects<anima::RendererFatalError>([&] { renderer.request_capture(output / "after-fatal.ppm"); });
        const auto stats = renderer.shutdown();
        require(!stats.validation_errors && !stats.validation_warnings && stats.scene_generations == generations,
                "Fatal replacement cleanup failed");
        require(renderer.shutdown().presented_frames == frames, "Repeated shutdown changed statistics");
        std::cout << "RESULT {\"fatal_injection\":\"" << fatal
                  << "\",\"fatal_rejected\":true,"
                     "\"scene_generations\":"
                  << stats.scene_generations << ",\"validation_warnings\":0,\"validation_errors\":0}\n";
        return 0;
    }
    const auto saved = anima::sample_pose(a_data);
    auto moved = saved;
    moved.world[0][13] += .15F;
    a->set_pose(id, moved);
    a->set_material_factor(id, 0, {1, .2F, .1F});
    a->set_primitive_visible(id, 1, false);
    capture("updated");
    a->set_pose(id, saved);
    a->clear_material_factor(id, 0);
    a->set_primitive_visible(id, 1, true);
    capture("restored");
    for (const auto *stage : {"vertex", "index", "texture", "texture-upload", "descriptors", "ready"}) {
        frame(); // Old scene can still have graphics work in flight.
        rejects<std::runtime_error>(
            [&] { renderer.set_scenes({scene(b_data)}, {anima::parse_renderer_failure_stage(stage)}); });
        ++rollbacks;
        capture(std::string("rollback-") + stage);
    }
    auto invalid = b_data;
    invalid.primitives[0].material = std::numeric_limits<int>::max();
    rejects<std::invalid_argument>([&] { renderer.set_scenes({scene(invalid)}); });
    ++rollbacks;
    invalid = b_data;
    invalid.textures[0].rgba.pop_back();
    rejects<std::invalid_argument>([&] { renderer.set_scenes({scene(invalid)}); });
    ++rollbacks;
    capture("rollback-invalid");
    auto view = anima::identity();
    view[0] = std::numeric_limits<float>::quiet_NaN();
    rejects<std::invalid_argument>([&] { renderer.set_view(view); });
    rejects<std::invalid_argument>([&] { renderer.set_view({}); });
    const auto mutation = [&](auto edit) {
        rejects<std::invalid_argument>(edit);
        ++mutation_rejections;
        frame(); // Rejected input must leave both the scene and frame usable.
    };
    auto malformed = saved;
    malformed.world.pop_back();
    mutation([&] { a->set_pose(id, malformed); });
    malformed = saved;
    malformed.world[0][0] = std::numeric_limits<float>::quiet_NaN();
    mutation([&] { a->set_pose(id, malformed); });
    mutation([&] { a->set_material_factor(id, 0, {-1, 0, 0}); });
    capture("rollback-update");
    replace({b});
    capture("scene-b");
    for (unsigned i = 0; i < 8; ++i) {
        replace({a});
        frame();
        replace({});
        frame();
        replace({b});
        frame();
    }
    capture("scene-b-repeat");
    if (!asset.empty()) {
        auto imported = scene(*anima::load_asset(asset));
        anima::OrbitCamera camera;
        camera.frame(imported->bounds().minimum, imported->bounds().maximum);
        int width = 0, height = 0;
        require(SDL_GetWindowSizeInPixels(window.get(), &width, &height) && height > 0, "No drawable");
        renderer.set_view(camera.matrix(float(width) / float(height)));
        replace({imported});
        capture("external-asset");
        renderer.set_view(anima::identity());
    }
    replace({std::make_shared<anima::Scene>()});
    capture("empty-end");
    std::cout << "STABLE_SWAPCHAIN_END\n";
    replace({a});
    require(SDL_SetWindowSize(window.get(), 760, 520), "Resize failed");
    renderer.request_resize();
    frame();
    capture("resized");
    require(SDL_MinimizeWindow(window.get()), "Minimize failed");
    while (!(SDL_GetWindowFlags(window.get()) & SDL_WINDOW_MINIMIZED)) {
        pump();
        SDL_Delay(5);
    }
    replace({b});
    require(!renderer.draw(), "Minimized window drew a frame");
    require(SDL_RestoreWindow(window.get()), "Restore failed");
    while (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_MINIMIZED) {
        pump();
        SDL_Delay(5);
    }
    renderer.request_resize();
    capture("restored-window");
    replace({});
    capture("empty-final");
    const auto empty = renderer.resource_stats();
    require(!empty.instances && !empty.pose_uploaded_bytes && !empty.candidate_instances && !empty.culled_instances &&
                !empty.candidate_draws && !empty.culled_draws && !empty.draw_calls && !empty.submitted_indices &&
                !empty.shadow_draw_calls && !empty.shadow_submitted_indices,
            "Clearing the scene retained mesh activity in resource statistics");
    // Also release a newly uploaded scene before any draw uses it.
    replace({b});
    const auto stats = renderer.shutdown();
    require(stats.presented_frames == frames && stats.capture_count == captures &&
                stats.scene_generations == generations && !stats.validation_warnings && !stats.validation_errors,
            "Replacement statistics or validation failed");
    require(renderer.shutdown().capture_count == captures, "Repeated shutdown changed statistics");
    rejects<std::logic_error>([&] { renderer.set_scenes({}); });
    rejects<std::logic_error>([&] { (void)renderer.draw(); });
    rejects<std::logic_error>([&] { renderer.request_capture(output / "after-shutdown.ppm"); });
    require(renderer.shutdown().captured == stats.captured, "A capture request after shutdown changed statistics");
    std::cout
        << "RESULT {\"frames\":" << frames << ",\"captures\":" << captures << ",\"scene_generations\":" << generations
        << ",\"swapchain_generations\":" << stats.swapchain_generations << ",\"recoverable_rollbacks\":" << rollbacks
        << ",\"mutation_rejections\":" << mutation_rejections
        << ",\"same_window\":true,\"minimized_replacement\":true,\"validation_warnings\":0,\"validation_errors\":0}\n";
    return 0;
}
} // namespace replacement_test
