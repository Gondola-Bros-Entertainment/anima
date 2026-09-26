#include "viewer_application.hpp"
#include <anima/core/fixed_step.hpp>
#include <anima/desktop/vulkan_renderer.hpp>
#ifdef ANIMA_HAS_ASSETS
#include <anima/assets/preview.hpp>
#endif

#include <SDL3/SDL.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
struct SDLSession {
    SDLSession() {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
            const std::string error = SDL_GetError();
            SDL_Quit();
            throw std::runtime_error("SDL_Init: " + error);
        }
    }
    ~SDLSession() { SDL_Quit(); }
};
[[maybe_unused]] void sdl_check(bool success, const char *operation) {
    if (!success)
        throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}
} // namespace

int anima::viewer::run_viewer(ViewerOptions options, ViewerDriver *driver) {
    try {
#ifdef ANIMA_HAS_ASSETS
        anima::OrbitCamera camera;
        std::unique_ptr<anima::AssetPreview> preview;
        if (!options.manifest.empty()) {
            preview = std::make_unique<anima::AssetPreview>(options.manifest);
            if (!options.clip.empty())
                preview->select(options.clip, !options.paused);
            else if (!preview->manifest().clips.empty())
                preview->select(preview->manifest().clips.front().name, !options.paused);
            if (options.pose_time)
                preview->seek(options.pose_time);
            options.renderer.scenes = {preview->render_scene()};
            std::cout << "PREVIEW " << preview->status()
                      << "\nControls: 1/2/3 manifest clip, Space pause/play, R restart, B bind\n";
            camera.frame(options.renderer.scenes.front()->bounds().minimum,
                         options.renderer.scenes.front()->bounds().maximum);
        }
        if (!options.asset.empty()) {
            const auto asset = anima::load_asset(options.asset);
            anima::print_mesh_report(anima::make_mesh_snapshot(*asset, anima::sample_pose(*asset)));
            auto scene = std::make_shared<anima::Scene>();
            (void)scene->create(options.asset.filename().string(), anima::Mesh::compile(*asset));
            options.renderer.scenes = {std::move(scene)};
            camera.frame(options.renderer.scenes.front()->bounds().minimum,
                         options.renderer.scenes.front()->bounds().maximum);
        }
#else
        if (!options.asset.empty() || !options.manifest.empty())
            throw std::runtime_error("GLB viewer disabled; configure ANIMA_BUILD_ASSETS=ON");
#endif
        // Bounded diagnostics leave the foreground application focused.
        if (options.frames || options.renderer.fail_after != anima::RendererFailureStage::none)
            SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        SDLSession sdl;
        std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window{
            SDL_CreateWindow("Anima | Vulkan foundation", 960, 640,
                             SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY),
            SDL_DestroyWindow};
        if (!window)
            throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
#ifdef ANIMA_HAS_ASSETS
        if (preview)
            SDL_SetWindowTitle(window.get(), ("Anima | " + preview->manifest().asset_id).c_str());
        else if (!options.renderer.scenes.empty())
            SDL_SetWindowTitle(window.get(), "Anima | Static asset preview");
#endif
        anima::VulkanRenderer renderer{window.get(), options.renderer};
        anima::FixedStepClock simulation;
        using Clock = std::chrono::steady_clock;
        const auto started = Clock::now();
        auto previous = started;
        std::uint64_t frames = 0, ticks = 0, suspended_iterations = 0;
        unsigned pixel_events = 0, minimize_events = 0, restore_events = 0, camera_updates = 0, preview_events = 0;
        bool quit = false, timed_out = false;
        while (!quit && (!options.frames || frames < options.frames)) {
            const auto now = Clock::now();
            if (options.frames && now - started > std::chrono::seconds(options.timeout_seconds)) {
                timed_out = true;
                break;
            }
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
                    (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE))
                    quit = true;
                if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                    ++pixel_events;
                    renderer.request_resize();
                }
                if (event.type == SDL_EVENT_WINDOW_MINIMIZED) {
                    ++minimize_events;
                    simulation.reset();
                }
                if (event.type == SDL_EVENT_WINDOW_RESTORED) {
                    ++restore_events;
                    renderer.request_resize();
                    simulation.reset();
                }
                if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST)
                    simulation.reset();
#ifdef ANIMA_HAS_ASSETS
                if (preview && event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                    switch (event.key.key) {
                    case SDLK_1:
                    case SDLK_2:
                    case SDLK_3: {
                        const auto index = static_cast<std::size_t>(event.key.key - SDLK_1);
                        if (index < preview->manifest().clips.size())
                            preview->select(preview->manifest().clips[index].name);
                        break;
                    }
                    case SDLK_SPACE:
                        preview->toggle_play();
                        break;
                    case SDLK_R:
                        preview->restart();
                        break;
                    case SDLK_B:
                        preview->bind_pose();
                        break;
                    default:
                        break;
                    }
                }
                if (!options.renderer.scenes.empty()) {
                    if (event.type == SDL_EVENT_MOUSE_MOTION && (event.motion.state & SDL_BUTTON_LMASK)) {
                        camera.orbit(-event.motion.xrel * 0.006F, event.motion.yrel * 0.006F);
                        ++camera_updates;
                    }
                    if (event.type == SDL_EVENT_MOUSE_WHEEL) {
                        camera.zoom(event.wheel.y * (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0F : 1.0F));
                        ++camera_updates;
                    }
                    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F)
                        camera.frame(options.renderer.scenes.front()->bounds().minimum,
                                     options.renderer.scenes.front()->bounds().maximum);
                }
#endif
            }
            if (quit)
                break;
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - previous);
            previous = now;
            if (driver)
                driver->before_draw({window.get(), renderer, frames, now,
                                     std::chrono::duration<double>(elapsed).count(),
#ifdef ANIMA_HAS_ASSETS
                                     preview.get(), !options.renderer.scenes.empty() ? &camera : nullptr,
#else
                                     nullptr, nullptr,
#endif
                                     camera_updates});
#ifdef ANIMA_HAS_ASSETS
            if (preview)
                SDL_SetWindowTitle(window.get(),
                                   ("Anima | " + preview->status() + " | 1/2/3 clip, Space pause").c_str());
            if (!options.renderer.scenes.empty()) {
                const auto *keys = SDL_GetKeyboardState(nullptr);
                const float dt = std::min(std::chrono::duration<float>(elapsed).count(), 0.1F);
                camera.orbit((float(keys[SDL_SCANCODE_RIGHT]) - float(keys[SDL_SCANCODE_LEFT])) * dt,
                             (float(keys[SDL_SCANCODE_UP]) - float(keys[SDL_SCANCODE_DOWN])) * dt);
                int width = 0, height = 0;
                sdl_check(SDL_GetWindowSizeInPixels(window.get(), &width, &height), "Get camera pixel aspect");
                if (width > 0 && height > 0)
                    renderer.set_view(camera.matrix(float(width) / float(height)));
            }
#endif
            if (renderer.draw()) {
#ifdef ANIMA_HAS_ASSETS
                if (preview) {
                    const auto step = driver ? driver->playback_seconds(std::chrono::duration<double>(elapsed).count())
                                             : std::chrono::duration<double>(elapsed).count();
                    for (const auto &preview_event : preview->advance(step)) {
                        ++preview_events;
                        std::cout << "PREVIEW_EVENT " << preview_event.name
                                  << " clip=" << preview->playback().animation()->name << " time=" << preview_event.time
                                  << " frame=" << frames << " (local preview only)\n";
                    }
                }
#endif
                ++frames;
                ticks += simulation.advance(elapsed).steps;
            } else {
                if (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_MINIMIZED)
                    ++suspended_iterations;
                simulation.reset();
                SDL_Delay(10); // Keep pumping events and the deadline while no drawable is available.
            }
        }
        const auto stats = renderer.shutdown(); // Includes teardown validation, before the borrowed window dies.
        const ViewerResult result{frames,         ticks,          suspended_iterations, pixel_events, minimize_events,
                                  restore_events, camera_updates, preview_events,       timed_out,    stats};
        const bool passed = !timed_out && (!options.frames || frames == options.frames) &&
                            (options.renderer.capture.empty() || stats.captured) && !stats.validation_warnings &&
                            !stats.validation_errors && (!driver || driver->passed(result));
        std::cout << "RESULT {\"passed\":" << (passed ? "true" : "false") << ",\"frames\":" << stats.presented_frames
                  << ",\"preview_events\":" << preview_events << ",\"captures\":" << stats.capture_count
                  << ",\"camera_updates\":" << camera_updates << ",\"simulation_ticks\":" << ticks
                  << ",\"swapchains\":" << stats.swapchain_generations << ",\"pixel_events\":" << pixel_events
                  << ",\"minimize_events\":" << minimize_events << ",\"restore_events\":" << restore_events
                  << ",\"suspended_iterations\":" << suspended_iterations
                  << ",\"captured\":" << (stats.captured ? "true" : "false")
                  << ",\"validation_warnings\":" << stats.validation_warnings
                  << ",\"validation_errors\":" << stats.validation_errors
                  << ",\"timed_out\":" << (timed_out ? "true" : "false") << "}\n";
        return passed ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << "Anima: " << error.what() << '\n';
        return 1;
    }
}
