#pragma once
// Standalone consumer: public APIs only. No game rules or engine implementation.
#include "gpu_checks.hpp"
#include "rejection.hpp"
#include <SDL3/SDL.h>
#include <anima/desktop/vulkan_renderer.hpp>
#include <anima/scene.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <typeinfo>
#include <vector>

namespace replacement_test {
inline void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
using rejection::rejects;
// What every call but shutdown() throws once the renderer is fatal.
constexpr std::string_view fatal_renderer = "Renderer has a fatal failure; only shutdown is legal";
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
        asset.textures.push_back(
            {std::make_shared<anima::Image>(anima::Image{2, 1, {shade, 200, 240, 255, shade, 200, 240, 255}}), {}});
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
// Construction rejects a RendererOptions::lod_threshold that is not finite or is negative, after the failure stage and
// the anisotropy and before it needs a window or GPU.
inline void reject_invalid_lod_threshold() {
    constexpr std::string_view invalid = "LOD threshold must be finite and nonnegative";
    const auto construct = [](float threshold, float anisotropy, anima::RendererFailureStage stage) {
        anima::RendererOptions options;
        options.lod_threshold = threshold;
        options.max_anisotropy = anisotropy;
        options.fail_after = stage;
        anima::VulkanRenderer renderer(nullptr, options);
    };
    constexpr auto no_failure = anima::RendererFailureStage::none;
    constexpr auto infinity = std::numeric_limits<float>::infinity();
    for (const auto value : {-1.F, -std::numeric_limits<float>::denorm_min(), -infinity, infinity,
                             std::numeric_limits<float>::quiet_NaN()})
        rejects<std::invalid_argument>([&] { construct(value, 16, no_failure); }, invalid);
    for (const auto value : {0.F, -0.F, 1.F, 1000.F})
        rejects<std::invalid_argument>([&] { construct(value, 16, no_failure); }, "Renderer requires an SDL window");
    // The failure stage and the anisotropy are checked first.
    rejects<std::invalid_argument>([&] { construct(-1, 16, anima::RendererFailureStage::vertex); },
                                   "Unknown initialization failure stage");
    rejects<std::invalid_argument>([&] { construct(-1, 0, no_failure); },
                                   "Maximum anisotropy must be finite and at least 1");
}
// A swapchain that fails after its predecessor was released leaves nothing to present, so the renderer
// becomes fatal instead of rebuilding it on every draw.
inline void reject_failed_swapchain(bool disable_present_fences) {
    const auto window = gpu_check::window("Anima swapchain failure consumer", 320, 240, SDL_WINDOW_HIGH_PIXEL_DENSITY);
    anima::RendererOptions options;
    options.validation = true;
    options.disable_present_fences = disable_present_fences;
    options.fail_after = anima::RendererFailureStage::swapchain;
    anima::VulkanRenderer renderer(window.get(), options);
    const auto started = std::chrono::steady_clock::now();
    rejects<anima::RendererFatalError>(
        [&] {
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
        },
        "Injected initialization failure after swapchain");
    rejects<anima::RendererFatalError>([&] { (void)renderer.draw(); }, fatal_renderer);
    rejects<anima::RendererFatalError>([&] { renderer.set_view(anima::identity()); }, fatal_renderer);
    const auto stats = renderer.shutdown();
    require(!stats.validation_errors && !stats.validation_warnings && !stats.swapchain_generations &&
                !stats.presented_frames,
            "Failed swapchain cleanup failed");
}
// A capture that cannot be written consumes its request: the draw that submits the frame reports it once, the
// frame counts if it was presented, and later draws present and count without writing again.
inline void reject_unwritable_capture(const std::filesystem::path &output, bool disable_present_fences) {
    constexpr unsigned later_frames = 3; // Enough to show the write is not retried.
    const auto window = gpu_check::window("Anima capture failure consumer", 320, 240, SDL_WINDOW_HIGH_PIXEL_DENSITY);
    anima::RendererOptions options;
    options.validation = true;
    options.disable_present_fences = disable_present_fences;
    anima::VulkanRenderer renderer(window.get(), options);
    renderer.set_view(anima::identity());
    const auto started = std::chrono::steady_clock::now();
    std::uint64_t presented = 0;
    const auto frame = [&] {
        for (;;) {
            require(std::chrono::steady_clock::now() - started < std::chrono::seconds(10),
                    "Capture failure watchdog expired");
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
            }
            if (renderer.draw()) {
                ++presented;
                return;
            }
            SDL_Delay(5);
        }
    };
    frame();
    const auto directory = output / "unwritable-capture"; // A directory cannot be opened as the capture file.
    std::filesystem::create_directories(directory);
    renderer.request_capture(directory);
    const auto unwritable = "Cannot open capture output: " + directory.string();
    bool reported = false;
    try {
        frame();
    } catch (const std::runtime_error &error) {
        require(typeid(error) == typeid(std::runtime_error) && error.what() == unwritable,
                "An unwritable capture was reported as another failure");
        reported = true;
    }
    require(reported, "An unwritable capture was not reported");
    try {
        for (unsigned i = 0; i < later_frames; ++i)
            frame();
    } catch (const std::runtime_error &error) {
        throw std::runtime_error("A reported capture failure recurred: " + std::string(error.what()));
    }
    // The reporting draw() also presented and counted its frame, unless presentation reported the swapchain out
    // of date. It cannot return that outcome, but an out-of-date frame makes the next draw() recreate the
    // swapchain once more.
    constexpr std::uint32_t capture_swapchains = 2; // The first swapchain, then one recreated for the capture.
    const auto stats = renderer.shutdown();
    const bool counted = stats.presented_frames == presented + 1;
    const bool out_of_date = stats.presented_frames == presented && stats.swapchain_generations > capture_swapchains;
    require((counted || out_of_date) && !stats.captured && !stats.capture_count && !stats.validation_errors &&
                !stats.validation_warnings,
            "A failed capture changed frame or capture statistics");
    std::filesystem::remove(directory);
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
    gpu_check::Video video;
    reject_failed_swapchain(options.disable_present_fences);
    reject_unwritable_capture(output, options.disable_present_fences);
    const auto window = gpu_check::window("Anima scene replacement consumer", 640, 480,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    const auto window_id = SDL_GetWindowID(window.get());
    options.validation = true;
    anima::VulkanRenderer renderer(window.get(), options);
    renderer.set_view(anima::identity());
    renderer.request_capture(); // The first frame, before any scene is selected.
    gpu_check::Captures images(output);
    const auto started = std::chrono::steady_clock::now();
    unsigned frames = 0, captures = 1, generations = 1, rollbacks = 0, mutation_rejections = 0;
    const auto pump = [&] {
        require(std::chrono::steady_clock::now() - started < gpu_check::watchdog, "Replacement watchdog expired");
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
        renderer.request_capture();
        frame();
        images.add(name, gpu_check::take(renderer));
        ++captures;
    };
    const auto replace = [&](std::vector<std::shared_ptr<const anima::Scene>> selection) {
        renderer.set_scenes(std::move(selection));
        ++generations;
    };
    frame();
    images.add("empty-start", gpu_check::take(renderer));
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
        // The timeout reports the failed wait; the lost device is reported once the upload is retired.
        rejects<anima::RendererFatalError>(
            [&] { renderer.set_scenes({b}, {anima::parse_renderer_failure_stage(fatal)}); },
            fatal == "upload-timeout" ? "Injected upload timeout failed (VkResult 2)"
                                      : "Device lost while retiring resource upload");
        rejects<anima::RendererFatalError>([&] { (void)renderer.draw(); }, fatal_renderer);
        rejects<anima::RendererFatalError>([&] { renderer.set_scenes({}); }, fatal_renderer);
        rejects<anima::RendererFatalError>([&] { renderer.set_view(anima::identity()); }, fatal_renderer);
        rejects<anima::RendererFatalError>([&] { renderer.request_capture(output / "after-fatal.ppm"); },
                                           fatal_renderer);
        rejects<anima::RendererFatalError>([&] { renderer.request_capture(); }, fatal_renderer);
        const auto stats = renderer.shutdown();
        require(!stats.validation_errors && !stats.validation_warnings && stats.scene_generations == generations &&
                    generations == 2,
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
            [&] { renderer.set_scenes({scene(b_data)}, {anima::parse_renderer_failure_stage(stage)}); },
            "Injected resource preparation failure after " + std::string(stage));
        ++rollbacks;
        capture(std::string("rollback-") + stage);
    }
    // Mesh::compile rejects invalid content before any renderer sees it, so these are not rollbacks.
    auto invalid = b_data;
    invalid.primitives[0].material = std::numeric_limits<int>::max();
    rejects<std::invalid_argument>([&] { (void)scene(invalid); }, "Invalid render primitive material");
    invalid = b_data;
    auto truncated = *invalid.textures[0].image;
    truncated.rgba.pop_back();
    invalid.textures[0].image = std::make_shared<anima::Image>(std::move(truncated));
    rejects<std::invalid_argument>([&] { (void)scene(invalid); },
                                   "MeshSnapshot texture byte count does not match dimensions");
    // A valid mesh that the device cannot hold fails inside set_scenes, which keeps the previous selection. The
    // width assumes a 2D image limit below 2^20 texels; Vulkan requires at least 4,096.
    constexpr std::uint32_t beyond_image_limit = 1U << 20;
    auto oversized = b_data;
    oversized.textures[0].image = std::make_shared<anima::Image>(
        anima::Image{beyond_image_limit, 1, std::vector<std::uint8_t>(std::size_t{beyond_image_limit} * 4, 255)});
    const auto unrenderable = scene(oversized);
    frame(); // Old scene can still have graphics work in flight.
    rejects<std::invalid_argument>([&] { renderer.set_scenes({unrenderable}); },
                                   "Resource texture exceeds device image dimensions");
    ++rollbacks;
    capture("rollback-invalid");
    auto view = anima::identity();
    view[0] = std::numeric_limits<float>::quiet_NaN();
    rejects<std::invalid_argument>([&] { renderer.set_view(view); },
                                   anima::math_error_message(anima::MathErrorCode::nonfinite_projection));
    rejects<std::invalid_argument>([&] { renderer.set_view({}); },
                                   anima::math_error_message(anima::MathErrorCode::singular_projection));
    const auto mutation = [&](auto edit, std::string_view expected) {
        rejects<std::invalid_argument>(edit, expected);
        ++mutation_rejections;
        frame(); // Rejected input must leave both the scene and frame usable.
    };
    auto malformed = saved;
    malformed.world.pop_back();
    mutation([&] { a->set_pose(id, malformed); }, "Pose does not match render asset");
    malformed = saved;
    malformed.world[0][0] = std::numeric_limits<float>::quiet_NaN();
    mutation([&] { a->set_pose(id, malformed); }, "Non-finite instance transform");
    mutation([&] { a->set_material_factor(id, 0, {-1, 0, 0}); }, "Invalid render material factor");
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
    require(SDL_SyncWindow(window.get()), "Resize did not settle");
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
    constexpr std::string_view shut_down = "Renderer is shut down";
    rejects<std::logic_error>([&] { renderer.set_scenes({}); }, shut_down);
    rejects<std::logic_error>([&] { (void)renderer.draw(); }, shut_down);
    rejects<std::logic_error>([&] { renderer.request_capture(output / "after-shutdown.ppm"); }, shut_down);
    rejects<std::logic_error>([&] { renderer.request_capture(); }, shut_down);
    require(renderer.shutdown().captured == stats.captured, "A capture request after shutdown changed statistics");
    const unsigned imported = asset.empty() ? 0 : 1;
    constexpr unsigned expected_rollbacks = 7, expected_mutation_rejections = 3, expected_generations = 32,
                       expected_captures = 18;
    require(rollbacks == expected_rollbacks && mutation_rejections == expected_mutation_rejections &&
                stats.scene_generations == expected_generations + imported && stats.swapchain_generations >= 2 &&
                captures == expected_captures + imported && images.size() == captures,
            "Replacement counted unexpected rollbacks, rejections, selections, swapchains or captures");
    // Empty selections show only the background, and every rejected or rolled-back change leaves the accepted
    // scene's image exactly as it was.
    images.require_clear("empty-start", "The first frame drew geometry");
    images.require_clear("empty-end", "An empty scene drew geometry");
    images.require_clear("empty-final", "An empty selection drew geometry");
    images.require_same("empty-start", "empty-end", "An empty scene differs from no selection");
    for (const auto *preserved :
         {"restored", "rollback-vertex", "rollback-index", "rollback-texture", "rollback-texture-upload",
          "rollback-descriptors", "rollback-ready", "rollback-invalid", "rollback-update"})
        images.require_same(preserved, "scene-a", "The old scene was not preserved");
    constexpr double least_change = .01;
    images.require_changed("scene-a", "updated", least_change, "Updating the instance had no visible effect");
    images.require_changed("scene-a", "scene-b", least_change, "Replacing the scene had no visible effect");
    images.require_same("scene-b", "scene-b-repeat", "Repeated replacement changed the scene's image");
    images.require(!gpu_check::same_size(images["resized"], images["scene-a"]),
                   "The resized window kept its capture size", {"resized", "scene-a"});
    for (const auto *drawn : {"scene-a", "scene-b", "resized", "restored-window"})
        images.require_foreground(drawn, "Expected geometry is not visible");
    if (imported)
        images.require_foreground("external-asset", "The imported asset is not visible");
    std::cout
        << "RESULT {\"frames\":" << frames << ",\"captures\":" << captures << ",\"scene_generations\":" << generations
        << ",\"swapchain_generations\":" << stats.swapchain_generations << ",\"recoverable_rollbacks\":" << rollbacks
        << ",\"mutation_rejections\":" << mutation_rejections
        << ",\"same_window\":true,\"minimized_replacement\":true,\"validation_warnings\":0,\"validation_errors\":0}\n";
    return 0;
}
} // namespace replacement_test
