#pragma once
// RendererOptions::frames_in_flight through the public API. A renderer that keeps two frames in flight draws, frame
// for frame, the images that one with a single frame in flight draws, while the inputs that draw() turns into
// per-frame data change between frames: a skinned palette and a material factor, the custom materials' frame block,
// whose time drives an effect, and the view and the environment block, whose sun rises every frame so that each frame
// redraws the atmosphere's sky view table (FrameProfile::gpu_atmosphere_ms), which the frame before it, still in flight
// with two, may be sampling. Within that sequence the frames in flight outlive the release of a mesh with its custom
// material and of a mesh with its placements, the replacement of the shadow maps, a rebuild of every atmosphere table,
// after which the view and the sun hold still for two frames, the first of which samples the tables that the frame in
// flight before it wrote and writes none, and an upload. The gpu label's synchronization validation, which tracks
// shader accesses through descriptors, checks the barriers between each frame and the frame before it, unless the
// host waited for that frame first, as a capture and an upload do. Where the device writes timestamps, the frames'
// atmosphere times show that the moving frames redraw the sky view table and the held ones do not. Each frame slot
// must have a pose buffer of its own (ResourceStats::pose_buffer_bytes).
//
// Beyond the pose buffers' size, nothing here sees a host-written block that both slots share, such as one environment
// or custom frame block, whose writes race the frame in flight: synchronization validation does not track host
// accesses, and each captured frame is read back inside the draw() that submits it, which waits for that frame, so the
// frame that such a write would corrupt, the one still in flight while the next draw() writes, is never captured.
#include "custom_materials.hpp"
#include "gpu_checks.hpp"
#include "resources.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <anima/desktop/vulkan_renderer.hpp>
#include <anima/mesh_placements.hpp>
#include <anima/scene.hpp>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace frames_in_flight_test {
inline void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
// Frames that each renderer draws, and those at which the scene loses two meshes, the shadow maps change size, the
// atmosphere's medium changes and a mesh uploads. A draw() that uploads waits for every frame in flight, so the upload
// comes after the rebuild, which then overlaps the frame before it.
constexpr int frame_count = 40, release_frame = 10, shadow_frame = 20, rebuild_frame = 30, upload_frame = 32;
// Frames after the rebuild during which the view and the sun keep the rebuild frame's.
constexpr int held_frames = 2;
// The frames read back: one before the first event, the frame after each event, and the last. Reading a frame back
// waits for it, so the frame after a captured one overlaps no earlier frame.
constexpr std::array<int, 6> captured_frames{
    5, release_frame + 1, shadow_frame + 1, rebuild_frame + 1, upload_frame + 1, frame_count - 1};
constexpr int window_width = 640, window_height = 480;
// The share of a sky view redraw's atmosphere time that a frame which dispatches nothing stays under. A skip leaves
// none of the redraw's time and a redraw all of it, so half leaves room for noise both ways.
constexpr double skipped_share = .5;

inline anima::Mat4 translation(anima::Vec3 offset) {
    auto matrix = anima::identity();
    matrix[12] = offset.x;
    matrix[13] = offset.y;
    matrix[14] = offset.z;
    return matrix;
}
// A lit quad, which receives the sun's shadows, facing +Z.
inline std::shared_ptr<const anima::Mesh> panel(const blending_test::Color &color, anima::Vec3 center, float half) {
    return blending_test::facing(blending_test::opaque(color, true), center, half, half);
}

// The scene of the sequence and the objects that change in it.
class Sequence {
  public:
    Sequence() {
        (void)scene_->add(
            blending_test::horizontal(blending_test::opaque({.6, .6, .6}, true), {0, 0, -3}, ground_half, ground_half));
        skinned_ = scene_->add(anima::Mesh::compile(*skinned_asset_));
        // Only the scene holds these meshes, their placements and the effect, so removing the objects releases them.
        effect_ = custom_material_test::add(
            *scene_, custom_material_test::surface({1.5F, .8F, -2.5F}, .4F, .4F),
            custom_material_test::effect_material("effect", anima::CustomBlend::opaque, {{.9, .3, .2}, 1, .6, .4, 2}));
        placed_ = scene_->add(panel({.2, .7, .3}, {}, .15F));
        std::vector<anima::Mat4> copies;
        for (const float x : {-1.5F, -1.F, -.5F})
            copies.push_back(translation({x, .9F, -2}));
        scene_->set_placements(placed_, anima::MeshPlacements::create(scene_->instance(placed_).asset, copies));
        // Water reads the opaque depth and color that each frame copies.
        (void)custom_material_test::add(*scene_, custom_material_test::surface({0, .3F, -2}, 1, .3F),
                                        custom_material_test::water_material({{.05, .2, .3}, .8}));
    }
    [[nodiscard]] std::shared_ptr<const anima::Scene> scene() const { return scene_; }
    /// Sets every input of frame @p frame: the view through a window of @p aspect, the environment, the shader time,
    /// and the skinned object's pose and factor, and applies the frame's event.
    void prepare(anima::VulkanRenderer &renderer, int frame, float aspect) {
        const float t = float(frame);
        // The view and the sun move with every frame but the held ones.
        const float moved = float(frame <= rebuild_frame ? frame : std::max(rebuild_frame, frame - held_frames));
        constexpr float orbit_rate = .02F, sun_rate = .05F, pose_rate = .3F, factor_rate = .2F, time_step = .1F;
        // The sun's height before normalization, which raises its normalized direction's Y by 0.0024 to 0.0032 a frame.
        constexpr float sun_height = .6F, sun_rise = .005F;
        const anima::Vec3 target{0, .5F, -3};
        const float orbit = orbit_rate * moved;
        const anima::Vec3 eye{target.x + 4 * std::sin(orbit), 1.5F, target.z + 4 * std::cos(orbit)};
        renderer.set_view(anima::perspective(std::numbers::pi_v<float> / 4, aspect, .1F, 100) *
                          anima::look_at(eye, target));
        anima::Environment environment;
        environment.sun.direction = {std::cos(sun_rate * moved), sun_height + sun_rise * moved,
                                     std::sin(sun_rate * moved)};
        environment.sun.irradiance = {3, 3, 3};
        environment.shadow_cascades.enabled = true;
        environment.shadow_cascades.count = 2;
        environment.shadow_cascades.resolution = frame < shadow_frame ? 512 : 1024;
        environment.detail_shadow.enabled = frame >= shadow_frame;
        environment.detail_shadow.resolution = 512;
        environment.detail_shadow.extent = 5;
        environment.atmosphere.enabled = true;
        environment.atmosphere.rayleigh_scale_height = frame < rebuild_frame ? 8'000.F : 9'000.F;
        environment.fog.density = .02F;
        environment.fog.falloff = .5F;
        renderer.set_environment(environment);
        renderer.set_time(time_step * t);
        auto pose = anima::sample_pose(*skinned_asset_);
        pose.world[1][13] += .2F * std::sin(pose_rate * t);
        scene_->set_pose(skinned_, pose, translation({-1, 0, -3}));
        scene_->set_material_factor(skinned_, 0, {.5F + .4F * std::sin(factor_rate * t), .6F, .4F});
        if (frame == release_frame) {
            scene_->remove(effect_);
            scene_->remove(placed_);
        }
        if (frame == upload_frame)
            (void)scene_->add(panel({.8, .7, .2}, {1.2F, .5F, -3.5F}, .3F));
    }

  private:
    static constexpr float ground_half = 6;
    std::shared_ptr<anima::Scene> scene_ = std::make_shared<anima::Scene>();
    std::shared_ptr<const anima::Asset> skinned_asset_ = resource_test::fixture();
    anima::Scene::Id skinned_{}, effect_{}, placed_{};
};

// What one renderer drew.
struct Drawn {
    // The captured frames, in order.
    std::vector<gpu_check::Image> images;
    // ResourceStats::pose_buffer_bytes after the last frame.
    std::uint64_t pose_buffer_bytes{};
    // FrameProfile::gpu_atmosphere_ms of each frame of the sequence, where a later call read it.
    std::vector<std::optional<double>> atmosphere_ms;
};

/// Draws the sequence in @p window through a renderer made with @p options, which selects its scene.
inline Drawn render(SDL_Window *window, anima::RendererOptions options) {
    Sequence sequence;
    options.scenes = {sequence.scene()};
    anima::VulkanRenderer renderer(window, options);
    int width = 0, height = 0;
    require(SDL_GetWindowSizeInPixels(window, &width, &height) && width > 0 && height > 0,
            "Frames in flight window has no drawable size");
    const float aspect = float(width) / float(height);
    Drawn drawn;
    drawn.atmosphere_ms.resize(frame_count);
    // The frame that each submission drew. A draw() that submits its frame times its recording and submission, which
    // one that returns earlier leaves at 0.
    std::vector<int> submitted;
    const auto started = std::chrono::steady_clock::now();
    for (int frame = 0; frame < frame_count; ++frame) {
        sequence.prepare(renderer, frame, aspect);
        const bool capture = std::find(captured_frames.begin(), captured_frames.end(), frame) != captured_frames.end();
        if (capture)
            renderer.request_capture();
        // A draw() that returns false presented nothing, so the frame's inputs stay for the next call.
        for (;;) {
            require(std::chrono::steady_clock::now() - started < gpu_check::watchdog, "Frames in flight watchdog");
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                        "Frames in flight check interrupted");
                if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                    renderer.request_resize();
            }
            const bool presented = renderer.draw();
            const auto profile = renderer.frame_profile();
            // The GPU fields time the frame submitted frames_in_flight submissions before the one that the call
            // submits.
            if (profile.gpu_available) {
                require(submitted.size() >= options.frames_in_flight, "GPU fields timed a frame never submitted");
                drawn.atmosphere_ms[std::size_t(submitted[submitted.size() - options.frames_in_flight])] =
                    profile.gpu_atmosphere_ms;
            }
            if (profile.record_submit_ms > 0)
                submitted.push_back(frame);
            if (presented)
                break;
            SDL_Delay(5);
        }
        if (capture)
            drawn.images.push_back(gpu_check::take(renderer));
    }
    drawn.pose_buffer_bytes = renderer.resource_stats().pose_buffer_bytes;
    const auto stats = renderer.shutdown();
    require(stats.presented_frames == std::uint64_t(frame_count) && !stats.validation_errors &&
                !stats.validation_warnings,
            "The frames in flight sequence presented " + std::to_string(stats.presented_frames) +
                " frames with validation warnings or errors");
    return drawn;
}

// The premise of the barriers that the sequence checks, from @p drawn's atmosphere times: the median frame that moves
// the sun, other than the first frame and the rebuild, which build every table, takes more than 1 / skipped_share times
// the faster of the held frames, as a frame that redraws the sky view table does over one that dispatches nothing. A
// median rather than each frame's time, which timing noise can carry either way, and the faster held frame for the
// same reason. Returns that median and that held frame's time, or nothing where the device writes no timestamps.
inline std::optional<std::array<double, 2>> check_redraws(const Drawn &drawn, std::uint32_t frames) {
    std::vector<double> moving;
    std::optional<double> held;
    for (int frame = 1; frame < frame_count; ++frame) {
        const auto &atmosphere = drawn.atmosphere_ms[std::size_t(frame)];
        if (!atmosphere || frame == rebuild_frame)
            continue;
        if (frame > rebuild_frame && frame <= rebuild_frame + held_frames)
            held = std::min(held.value_or(*atmosphere), *atmosphere);
        else
            moving.push_back(*atmosphere);
    }
    if (!held || moving.empty())
        return std::nullopt;
    const auto middle = moving.begin() + std::ptrdiff_t(moving.size() / 2);
    std::nth_element(moving.begin(), middle, moving.end());
    require(*middle > *held / skipped_share,
            "With " + std::to_string(frames) + " frames in flight, the frames that move the sun took a median " +
                std::to_string(*middle) + " ms of atmosphere dispatches, not more than twice the " +
                std::to_string(*held) + " ms of a held frame, so they do not all redraw the sky view table");
    return std::array{*middle, *held};
}

inline int run(int argc, char **argv) {
    require(argc == 3, "Usage: consumer --frames-in-flight OUTPUT");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    gpu_check::Video video;
    const auto window = gpu_check::window("Anima frames in flight verification", window_width, window_height);
    gpu_check::Captures images(argv[2]);
    const auto name = [](std::uint32_t frames, int frame) {
        return std::to_string(frames) + "-in-flight-frame-" + std::to_string(frame);
    };
    std::array<std::uint64_t, 2> pose_bytes{};
    std::array<std::optional<std::array<double, 2>>, 2> redraws;
    for (const std::uint32_t frames : {1U, 2U}) {
        anima::RendererOptions options;
        options.validation = true;
        options.profile = true;
        options.frames_in_flight = frames;
        auto drawn = render(window.get(), options);
        for (std::size_t i = 0; i < drawn.images.size(); ++i)
            images.add(name(frames, captured_frames[i]), std::move(drawn.images[i]));
        pose_bytes[frames - 1] = drawn.pose_buffer_bytes;
        redraws[frames - 1] = check_redraws(drawn, frames);
    }
    // A slot's pose buffer grows by doubling until it holds the palettes written into it: those of each frame that the
    // slot draws and, in the first slot, those of construction's selection, which is prepared without culling. A single
    // slot's buffer holds them all, so neither of two slots' buffers exceeds it and one of them matches it, while the
    // other adds its own bytes; one buffer that both slots shared would report the bytes of a single slot.
    const auto [one_slot, two_slots] = pose_bytes;
    require(one_slot && two_slots > one_slot && two_slots <= 2 * one_slot,
            "Two frames in flight allocated " + std::to_string(two_slots) +
                " bytes of pose buffers, not more than the " + std::to_string(one_slot) +
                " of one and at most twice them");
    // Each event and the inputs that change every frame change the image, so equal images compare drawn frames.
    constexpr double least_change = .01;
    for (std::size_t i = 1; i < captured_frames.size(); ++i)
        images.require_changed(name(1, captured_frames[i - 1]), name(1, captured_frames[i]), least_change,
                               "The sequence did not change the image");
    for (const int frame : captured_frames)
        images.require_same(name(1, frame), name(2, frame),
                            "Two frames in flight drew another image than one frame in flight");
    std::cout << "PASS frames in flight: two frames in flight draw the " << captured_frames.size()
              << " captured frames of a " << frame_count
              << "-frame sequence exactly as one does, while a palette, a material factor and shader time change "
                 "every frame, the view and the sun every frame but "
              << held_frames
              << ", so that each such frame redraws the sky view table, two meshes, their placements and a custom "
                 "material are released, the shadow maps are replaced, the atmosphere's tables rebuilt and a mesh "
                 "uploaded, with clean validation, in "
              << two_slots << " bytes of pose buffers against " << one_slot << " with one frame in flight";
    for (const std::uint32_t frames : {1U, 2U}) {
        std::cout << "; with " << frames << " in flight, ";
        if (const auto &redraw = redraws[frames - 1])
            std::cout << "the moving frames took a median " << (*redraw)[0] << " ms of atmosphere dispatches and a "
                      << "held frame " << (*redraw)[1] << " ms";
        else
            std::cout << "no GPU timestamps, so no atmosphere times";
    }
    std::cout << '\n';
    return 0;
}
} // namespace frames_in_flight_test
