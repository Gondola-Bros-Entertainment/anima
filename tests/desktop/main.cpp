#include "../../apps/desktop/viewer_application.hpp"
#ifdef ANIMA_HAS_ASSETS
#include <anima/assets/preview.hpp>
#endif
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
using namespace anima::viewer;
namespace {
void sdl_check(bool success, const char *operation) {
    if (!success)
        throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}
class Verification final : public ViewerDriver {
  public:
    bool smoke_{};
    std::filesystem::path captures_;
    void before_draw(const ViewerFrame &frame) override {
        auto *window = frame.window;
        auto &renderer = frame.renderer;
        const auto frames = frame.frames;
        const auto now = frame.now;
        if (smoke_) {
            if (frames >= 20 && !resized) {
                sdl_check(SDL_SetWindowSize(window, 800, 500), "Resize smoke window");
                resized = true;
            }
            if (frames >= 40 && !minimized) {
                sdl_check(SDL_MinimizeWindow(window), "Minimize smoke window");
                minimized = waiting_restore = true;
                restore_at = now + std::chrono::milliseconds(700);
            }
            if (waiting_restore && now >= restore_at) {
                sdl_check(SDL_RestoreWindow(window), "Restore smoke window");
                waiting_restore = false;
                restored = true;
                renderer.request_resize();
            }
            if (frames >= 80 && !resized_again) {
                sdl_check(SDL_SetWindowSize(window, 1024, 576), "Second smoke resize");
                resized_again = true;
            }
        }
#ifdef ANIMA_HAS_ASSETS
        auto *preview = frame.preview;
        auto *camera = frame.camera;
        if (preview && !captures_.empty() && scripted_frame != frames) {
            scripted_frame = frames;
            if (frames == 140)
                preview->select("Walk");
            if (frames == 155 || frames == 165)
                preview->toggle_play();
            if (frames == 180)
                preview->select("Walk");
            if (frames == 250)
                preview->restart();
            if (frames == 315)
                preview->bind_pose();
            const std::map<std::uint64_t, std::string> captures{
                {100, "idle_start"},        {130, "idle_mid"},   {150, "walk_start"}, {155, "walk_paused"},
                {164, "walk_paused_again"}, {175, "walk_mid"},   {190, "walk_early"}, {214, "walk_swing"},
                {284, "walk_restart"},      {320, "bind_start"}, {340, "bind_later"}};
            if (const auto capture = captures.find(frames); capture != captures.end()) {
                renderer.request_capture(captures_ / (capture->second + ".ppm"));
                std::cout << "CAPTURE " << capture->second << " frame=" << frames << " " << preview->status() << '\n';
            } else if (frames >= 180 && frames <= 244 && (frames - 180) % 4 == 0) {
                std::ostringstream name;
                name << "stride_" << std::setw(3) << std::setfill('0') << (frames - 180) / 4 << ".ppm";
                renderer.request_capture(captures_ / name.str());
            }
        }
        if (camera) {
            const auto dt = static_cast<float>(std::min(frame.seconds, .1));
            auto &camera_updates = frame.camera_updates;
            if (smoke_ && captures_.empty() && frames >= 60) {
                camera->orbit(dt * 0.7F, 0);
                ++camera_updates;
            }
        }
#endif
    }
    double playback_seconds(double elapsed) const override { return captures_.empty() ? elapsed : 1.0 / 60.0; }
    bool passed(const ViewerResult &r) const override {
        return (!smoke_ ||
                (resized && minimized && restored && resized_again && r.pixel_events >= 2 && r.minimize_events > 0 &&
                 r.restore_events > 0 && r.suspended_iterations > 0 && r.renderer.swapchain_generations >= 3)) &&
               (captures_.empty() || r.renderer.capture_count >= 28);
    }

  private:
    bool resized{}, minimized{}, restored{}, resized_again{}, waiting_restore{};
    std::chrono::steady_clock::time_point restore_at{};
    [[maybe_unused]] std::uint64_t scripted_frame = std::numeric_limits<std::uint64_t>::max();
};
} // namespace
int main(int argc, char **argv) {
    try {
        ViewerOptions options;
        Verification checks;
        for (int i = 1; i < argc; ++i) {
            if (parse_viewer_argument(options, i, argc, argv))
                continue;
            const std::string_view argument = argv[i];
            const auto next = [&]() -> std::string_view {
                if (++i >= argc)
                    throw std::invalid_argument("Missing verification option value");
                return argv[i];
            };
            if (argument == "--smoke")
                checks.smoke_ = true;
            else if (argument == "--preview-smoke") {
                checks.captures_ = next();
                checks.smoke_ = true;
            } else if (argument == "--fail-after") {
                options.renderer.fail_after = anima::parse_renderer_failure_stage(next());
                using enum anima::RendererFailureStage;
                switch (options.renderer.fail_after) {
                case instance:
                case surface:
                case device:
                case resources:
                case swapchain:
                case texture:
                case texture_upload:
                    break;
                default:
                    throw std::invalid_argument("Unsupported initialization failure stage");
                }
            } else
                throw std::invalid_argument("Unknown verification option: " + std::string(argument));
        }
        if (options.help) {
            std::cout << viewer_usage() << "anima_check: --smoke | --preview-smoke DIR | --fail-after STAGE\n";
            return 0;
        }
        if (!checks.captures_.empty()) {
            if (options.manifest.empty())
                throw std::invalid_argument("Preview check requires a manifest");
            if (options.frames && options.frames < 360)
                throw std::invalid_argument("Preview check needs 360 frames");
            if (!options.frames)
                options.frames = 360;
        }
        if (checks.smoke_) {
            options.renderer.validation = true;
            if (options.frames && options.frames < 120)
                throw std::invalid_argument("Lifecycle check needs 120 frames");
            if (!options.frames)
                options.frames = 120;
        }
        validate_viewer_options(options);
        return run_viewer(std::move(options), &checks);
    } catch (const std::exception &error) {
        std::cerr << "Anima check: " << error.what() << '\n';
        return 1;
    }
}
