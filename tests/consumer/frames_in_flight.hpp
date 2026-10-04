#pragma once
// RendererOptions::frames_in_flight through the public API. A renderer that keeps two frames in flight draws, frame
// for frame, the images that one with a single frame in flight draws, while every input that draw() turns into
// per-frame data changes between frames: the view, a skinned palette and a material factor, the environment block,
// from which the atmosphere rebuilds its sky view table each frame, and the custom materials' frame block, whose time
// drives an effect. Within that sequence the frames in flight outlive the release of a mesh with its custom material
// and of a mesh with its placements, the replacement of the shadow maps, a rebuild of every atmosphere table and an
// upload, under the gpu label's synchronization validation.
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
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace frames_in_flight_test {
inline void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
// Frames that each renderer draws, and those at which the scene loses two meshes, the shadow maps change size and the
// atmosphere's medium changes while a mesh uploads.
constexpr int frame_count = 40, release_frame = 10, shadow_frame = 20, rebuild_frame = 30;
// The frames read back: one before the first event, the frame after each event, and the last.
constexpr std::array<int, 5> captured_frames{5, release_frame + 1, shadow_frame + 1, rebuild_frame + 1,
                                             frame_count - 1};
constexpr int window_width = 640, window_height = 480;

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
        constexpr float orbit_rate = .02F, sun_rate = .05F, pose_rate = .3F, factor_rate = .2F, time_step = .1F;
        const anima::Vec3 target{0, .5F, -3};
        const float orbit = orbit_rate * t;
        const anima::Vec3 eye{target.x + 4 * std::sin(orbit), 1.5F, target.z + 4 * std::cos(orbit)};
        using anima::operator*;
        renderer.set_view(anima::perspective(aspect, .1F, 100) * anima::look_at(eye, target));
        anima::Environment environment;
        environment.sun.direction = {std::cos(sun_rate * t), .7F, std::sin(sun_rate * t)};
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
        if (frame == rebuild_frame)
            (void)scene_->add(panel({.8, .7, .2}, {1.2F, .5F, -3.5F}, .3F));
    }

  private:
    static constexpr float ground_half = 6;
    std::shared_ptr<anima::Scene> scene_ = std::make_shared<anima::Scene>();
    std::shared_ptr<const anima::Asset> skinned_asset_ = resource_test::fixture();
    anima::Scene::Id skinned_{}, effect_{}, placed_{};
};

/// Draws the sequence in @p window through a renderer made with @p options, which selects its scene, and returns the
/// captured frames in order.
inline std::vector<gpu_check::Image> render(SDL_Window *window, anima::RendererOptions options) {
    Sequence sequence;
    options.scenes = {sequence.scene()};
    anima::VulkanRenderer renderer(window, options);
    int width = 0, height = 0;
    require(SDL_GetWindowSizeInPixels(window, &width, &height) && width > 0 && height > 0,
            "Frames in flight window has no drawable size");
    const float aspect = float(width) / float(height);
    std::vector<gpu_check::Image> images;
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
            if (renderer.draw())
                break;
            SDL_Delay(5);
        }
        if (capture)
            images.push_back(gpu_check::take(renderer));
    }
    const auto stats = renderer.shutdown();
    require(stats.presented_frames == std::uint64_t(frame_count) && !stats.validation_errors &&
                !stats.validation_warnings,
            "The frames in flight sequence presented " + std::to_string(stats.presented_frames) +
                " frames with validation warnings or errors");
    return images;
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
    for (const std::uint32_t frames : {1U, 2U}) {
        anima::RendererOptions options;
        options.validation = true;
        options.profile = true;
        options.frames_in_flight = frames;
        auto captured = render(window.get(), options);
        for (std::size_t i = 0; i < captured.size(); ++i)
            images.add(name(frames, captured_frames[i]), std::move(captured[i]));
    }
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
              << "-frame sequence exactly as one does, while the view, a palette, a material factor, the environment "
                 "and shader time change every frame, two meshes, their placements and a custom material are "
                 "released, the shadow maps are replaced and the atmosphere's tables rebuilt, with clean validation\n";
    return 0;
}
} // namespace frames_in_flight_test
