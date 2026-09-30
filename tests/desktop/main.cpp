#include "../../apps/desktop/viewer_application.hpp"
#include "../consumer/gpu_checks.hpp"
#ifdef ANIMA_HAS_ASSETS
#include "../consumer/gltf_fixture.hpp"
#include <anima/assets/mesh_snapshot.hpp>
#include <anima/assets/preview.hpp>
#endif
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>
using namespace anima::viewer;
namespace {
void sdl_check(bool success, const char *operation) {
    if (!success)
        throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}
// Preview captures by name: the 11 named frames and 17 strides 4 frames apart from frame 180.
constexpr unsigned stride_captures = 17, stride_start = 180, stride_frames = 4;
constexpr std::uint32_t preview_captures = 11 + stride_captures;
std::string stride_name(unsigned index) {
    std::ostringstream name;
    name << "stride_" << std::setw(3) << std::setfill('0') << index;
    return name.str();
}
class Verification final : public ViewerDriver {
  public:
    bool smoke_{};
    bool asset_{};
    // Reads the first frame back for check_first_frame().
    bool first_frame_{};
    std::optional<anima::CapturedImage> first_;
    // Plays the preview script and reads its frames back for check_preview().
    bool preview_{};
    std::map<std::string, anima::CapturedImage> previews_;
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
        // A frame is presented only after a draw() that completed its capture request.
        if (pending_)
            if (auto image = renderer.take_capture()) {
                previews_.insert_or_assign(*pending_, std::move(*image));
                pending_.reset();
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
        if (preview && preview_ && scripted_frame != frames) {
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
                renderer.request_capture();
                pending_ = capture->second;
                std::cout << "CAPTURE " << capture->second << " frame=" << frames << " " << preview->status() << '\n';
            } else if (frames >= stride_start && frames < stride_start + stride_captures * stride_frames &&
                       (frames - stride_start) % stride_frames == 0) {
                renderer.request_capture();
                pending_ = stride_name(unsigned((frames - stride_start) / stride_frames));
            }
        }
        if (camera) {
            const auto dt = static_cast<float>(std::min(frame.seconds, .1));
            auto &camera_updates = frame.camera_updates;
            if (smoke_ && !preview_ && frames >= 60) {
                camera->orbit(dt * 0.7F, 0);
                ++camera_updates;
            }
        }
#endif
    }
    double playback_seconds(double elapsed) const override { return preview_ ? 1.0 / 60.0 : elapsed; }
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
        if (preview_)
            expect(r.renderer.capture_count == preview_captures && previews_.size() == preview_captures,
                   "The preview check did not read back its 28 frames");
        return passing;
    }

  private:
    bool resized{}, minimized{}, restored{}, resized_again{}, waiting_restore{};
    bool first_requested_{};
    std::optional<std::string> pending_;
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
#ifdef ANIMA_HAS_ASSETS
// A quad skinned to one joint, with two looping clips that move the joint along X and back: Idle by 0.15 over 2
// seconds, Walk by 0.9 over 4. Writes preview.glb and its manifest, preview.asset.json, into @p directory.
std::filesystem::path write_preview_fixture(const std::filesystem::path &directory) {
    gltf_fixture::Builder builder;
    const auto positions = builder.floats({-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, -1, 0, 1, 1, 0, -1, 1, 0}, "VEC3", true);
    std::vector<float> normals, weights;
    for (unsigned vertex = 0; vertex < 6; ++vertex) {
        normals.insert(normals.end(), {0, 0, 1});
        weights.insert(weights.end(), {1, 0, 0, 0});
    }
    const auto normal = builder.floats(normals, "VEC3");
    const auto joints = builder.shorts(std::vector<std::uint16_t>(6 * 4, 0), "VEC4");
    const auto weight = builder.floats(weights, "VEC4");
    const auto inverse_bind = builder.floats({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, "MAT4");
    std::string animations;
    for (const auto &[name, duration, travel] : {std::tuple{"Idle", 2.F, .15F}, std::tuple{"Walk", 4.F, .9F}}) {
        const auto times = builder.floats({0, duration / 2, duration}, "SCALAR", true);
        const auto translations = builder.floats({0, 0, 0, travel, 0, 0, 0, 0, 0}, "VEC3");
        animations += std::string(animations.empty() ? "" : ",") + R"({"name":")" + name +
                      R"(","samplers":[{"input":)" + std::to_string(times) + R"(,"output":)" +
                      std::to_string(translations) +
                      R"(}],"channels":[{"sampler":0,"target":{"node":1,"path":"translation"}}]})";
    }
    const auto glb = builder.glb(
        R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"children":[1,2]},{"name":"joint"},{"mesh":0,"skin":0}],)"
        R"("skins":[{"joints":[1],"inverseBindMatrices":)" +
        std::to_string(inverse_bind) + R"(}],"meshes":[{"primitives":[{"attributes":{"POSITION":)" +
        std::to_string(positions) + R"(,"NORMAL":)" + std::to_string(normal) + R"(,"JOINTS_0":)" +
        std::to_string(joints) + R"(,"WEIGHTS_0":)" + std::to_string(weight) +
        R"(},"material":0}]}],"materials":[{"pbrMetallicRoughness":{"baseColorFactor":[0.3,0.6,0.9,1],)"
        R"("metallicFactor":0}}],"animations":[)" +
        animations + "]");
    std::filesystem::create_directories(directory);
    std::ofstream model(directory / "preview.glb", std::ios::binary);
    model.write(reinterpret_cast<const char *>(glb.data()), static_cast<std::streamsize>(glb.size()));
    const auto manifest = directory / "preview.asset.json";
    std::ofstream(manifest) << R"({"schema_version":3,"units":"meters","asset_id":"test.preview",)"
                               R"("model":"preview.glb","skeleton":{"id":"test.rig","joint_count":1,"bind_signature":")"
                            << std::string(64, '0') << R"("},"clips":[{"name":"Idle","loop":true,"events":[]},)"
                            << R"({"name":"Walk","loop":true,"events":[]}]})" << '\n';
    model.close();
    if (!model || !std::filesystem::exists(manifest))
        throw std::runtime_error("Could not write the preview fixture");
    return manifest;
}
// Idle and Walk move the quad, a paused pose holds, a restart reproduces its pose, the stride frames differ and
// the bind pose holds.
void check_preview(std::map<std::string, anima::CapturedImage> images, const std::filesystem::path &output) {
    gpu_check::Captures captures(output);
    for (auto &[name, image] : images)
        captures.add(name, std::move(image));
    constexpr double idle_motion = .0001, walk_motion = .001;
    captures.require_changed("idle_start", "idle_mid", idle_motion, "Idle did not move the quad");
    captures.require_changed("walk_start", "walk_mid", walk_motion, "Walk did not move the quad");
    captures.require_changed("walk_early", "walk_swing", walk_motion, "Walk did not swing the quad");
    captures.require_same("walk_paused", "walk_paused_again", "The paused pose moved");
    captures.require_same("walk_swing", "walk_restart", "Restarting did not reproduce the pose");
    std::vector<std::string> strides;
    unsigned distinct = 0;
    for (unsigned i = 0; i < stride_captures; ++i) {
        strides.push_back(stride_name(i));
        bool repeated = false;
        for (unsigned earlier = 0; earlier < i && !repeated; ++earlier)
            repeated = gpu_check::same(captures[strides[earlier]], captures[strides[i]]);
        if (!repeated)
            ++distinct;
    }
    constexpr unsigned least_distinct_strides = 14;
    captures.require(distinct >= least_distinct_strides,
                     "Only " + std::to_string(distinct) + " of the 17 stride frames differ", strides);
    captures.require_same("bind_start", "bind_later", "The bind pose moved");
}
#endif
// Removes the directory of a generated fixture when the check ends.
struct RemovedDirectory {
    std::filesystem::path path;
    ~RemovedDirectory() {
        std::error_code ignored;
        if (!path.empty())
            std::filesystem::remove_all(path, ignored);
    }
};
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
            else if (argument == "--preview") {
                checks.preview_ = true;
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
                      << "anima_check: --smoke | --preview | --fail-after STAGE, with --output DIR for the images\n"
                         "  of a failed check. A run that draws its first frame compares it in memory; --preview\n"
                         "  plays a generated skinned quad unless --manifest names Idle and Walk clips.\n";
            return 0;
        }
        RemovedDirectory fixture;
        if (checks.preview_) {
#ifdef ANIMA_HAS_ASSETS
            if (!options.asset.empty())
                throw std::invalid_argument("The preview check takes --manifest, not --asset");
            if (options.manifest.empty()) {
                fixture.path = output / "preview-fixture";
                options.manifest = write_preview_fixture(fixture.path);
            }
#else
            throw std::invalid_argument("The preview check needs asset support");
#endif
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
        if (checks.first_frame_ || checks.preview_) {
            if (!options.renderer.capture.empty())
                throw std::invalid_argument("anima_check compares its frames in memory; omit --capture");
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
        const auto status = run_viewer(options, &checks); // A copy: the checks below read the options.
        if (status != 0)
            return status;
#ifdef ANIMA_HAS_ASSETS
        if (checks.preview_)
            check_preview(std::move(checks.previews_), output);
#endif
        if (checks.first_frame_) {
            if (!checks.first_)
                throw std::runtime_error("The first frame was not read back");
            check_first_frame(*checks.first_, options, output);
        }
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
