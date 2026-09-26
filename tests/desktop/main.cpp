#include "../../apps/desktop/viewer_application.hpp"
#include "../consumer/gpu_checks.hpp"
#ifdef ANIMA_HAS_ASSETS
#include <anima/assets/mesh_snapshot.hpp>
#include <anima/assets/preview.hpp>
#endif
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
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
    bool asset_{};
    // Reads the first frame back for check_first_frame().
    bool first_frame_{};
    std::optional<anima::CapturedImage> first_;
    std::filesystem::path captures_;
    void before_draw(const ViewerFrame &frame) override {
        auto *window = frame.window;
        auto &renderer = frame.renderer;
        const auto frames = frame.frames;
        const auto now = frame.now;
        if (first_frame_ && !first_) {
            if (!first_requested_) {
                renderer.request_capture();
                first_requested_ = true;
            } else
                first_ = renderer.take_capture();
        }
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
        constexpr unsigned asset_camera_updates = 60;
        bool passing = true;
        const auto expect = [&](bool condition, const char *failure) {
            if (!condition) {
                std::cerr << "Anima check: " << failure << '\n';
                passing = false;
            }
        };
        if (smoke_)
            expect(resized && minimized && restored && resized_again && r.pixel_events >= 2 && r.minimize_events > 0 &&
                       r.restore_events > 0 && r.suspended_iterations > 0 && r.renderer.swapchain_generations >= 3,
                   "The window did not resize, minimize and restore with the swapchain following it");
        if (smoke_ && asset_)
            expect(r.camera_updates >= asset_camera_updates, "The asset smoke orbited its camera too few times");
        if (!captures_.empty())
            expect(r.renderer.capture_count >= 28, "The preview check wrote too few captures");
        return passing;
    }

  private:
    bool resized{}, minimized{}, restored{}, resized_again{}, waiting_restore{};
    bool first_requested_{};
    std::chrono::steady_clock::time_point restore_at{};
    [[maybe_unused]] std::uint64_t scripted_frame = std::numeric_limits<std::uint64_t>::max();
};
// The viewer's first frame: nothing but the clear color with --empty, the asset with --asset, and otherwise
// the diagnostic triangle on a dark background.
void check_first_frame(const anima::CapturedImage &image, const ViewerOptions &options,
                       const std::filesystem::path &output) {
    gpu_check::Captures captures(output);
    captures.add("first-frame", image);
    if (options.empty) {
        captures.require_clear("first-frame", "The empty viewer drew geometry");
        return;
    }
    constexpr int darkest_background = 80, changed_level = 20;
    const auto background = gpu_check::pixel(image, 0, 0);
    std::vector<std::size_t> covered;
    std::set<std::uint32_t> colors; // Packed as 0xRRGGBB.
    for (std::size_t i = 0; i < std::size_t{image.width} * image.height; ++i) {
        const gpu_check::Rgb color{image.rgb[i * 3], image.rgb[i * 3 + 1], image.rgb[i * 3 + 2]};
        if (gpu_check::difference(color, background) > changed_level) {
            covered.push_back(i);
            colors.insert(std::uint32_t(color[0]) << 16 | std::uint32_t(color[1]) << 8 | std::uint32_t(color[2]));
        }
    }
    const auto coverage = double(covered.size()) / double(std::size_t{image.width} * image.height);
    if (!options.asset.empty()) {
        // The asset spans most of the view's height, with shading rather than one flat color.
        constexpr double least_asset_coverage = .015, most_asset_coverage = .4, least_height = .45;
        constexpr std::size_t least_colors = 100;
        captures.require(coverage > least_asset_coverage && coverage < most_asset_coverage,
                         "The asset covers " + std::to_string(coverage) + " of the first frame", {"first-frame"});
        const auto top = covered.front() / image.width, bottom = covered.back() / image.width;
        captures.require(double(bottom - top) > image.height * least_height,
                         "The asset spans only rows " + std::to_string(top) + " to " + std::to_string(bottom),
                         {"first-frame"});
        captures.require(colors.size() > least_colors,
                         "The asset shows only " + std::to_string(colors.size()) + " colors", {"first-frame"});
        return;
    }
    constexpr double least_coverage = .07, most_coverage = .4;
    const auto center = gpu_check::pixel(image, image.width / 2, image.height / 2);
    captures.require(std::max({background[0], background[1], background[2]}) < darkest_background &&
                         std::min({center[0], center[1], center[2]}) > darkest_background,
                     "The diagnostic triangle is missing: background " + gpu_check::text(background) + ", center " +
                         gpu_check::text(center),
                     {"first-frame"});
    captures.require(coverage > least_coverage && coverage < most_coverage,
                     "The diagnostic triangle covers " + std::to_string(coverage) + " of the first frame",
                     {"first-frame"});
}
} // namespace
int main(int argc, char **argv) {
    try {
        ViewerOptions options;
        Verification checks;
        std::filesystem::path output = ".";
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
            } else if (argument == "--output")
                output = next();
            else if (argument == "--fail-after") {
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
            std::cout << viewer_usage()
                      << "anima_check: --smoke | --preview-smoke DIR | --fail-after STAGE, with --output DIR for the\n"
                         "  images of a failed check. A run that draws its first frame compares it in memory.\n";
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
        checks.asset_ = !options.asset.empty();
        checks.first_frame_ =
            options.manifest.empty() && options.renderer.fail_after == anima::RendererFailureStage::none;
        if (checks.first_frame_) {
            if (!options.renderer.capture.empty())
                throw std::invalid_argument("anima_check compares its first frame in memory; omit --capture");
            // The check takes the first frame when the viewer prepares the second.
            if (options.frames == 1)
                throw std::invalid_argument("Checking the first frame needs at least 2 frames");
        }
        validate_viewer_options(options);
#ifdef ANIMA_HAS_ASSETS
        if (checks.asset_) {
            const auto asset = anima::load_asset(options.asset);
            const auto snapshot = anima::make_mesh_snapshot(*asset, anima::sample_pose(*asset));
            if (!snapshot.mesh_nodes || snapshot.primitives.empty() || snapshot.vertices.empty())
                throw std::runtime_error("The asset has no mesh nodes, primitives or triangles");
        }
#endif
        const auto status = run_viewer(options, &checks); // A copy: check_first_frame() reads the options.
        if (status != 0 || !checks.first_frame_)
            return status;
        if (!checks.first_)
            throw std::runtime_error("The first frame was not read back");
        check_first_frame(*checks.first_, options, output);
        return 0;
    } catch (const DisplayUnavailable &error) {
        return gpu_check::unavailable(error);
    } catch (const anima::RendererUnavailableError &error) {
        return gpu_check::unavailable(error);
    } catch (const std::exception &error) {
        std::cerr << "Anima check: " << error.what() << '\n';
        return 1;
    }
}
