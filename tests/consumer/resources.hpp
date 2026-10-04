#pragma once
#include "gpu_checks.hpp"
#include "reference.hpp"
#include "rejection.hpp"
#include <SDL3/SDL.h>
#include <anima/assets/mesh_preparation.hpp>
#include <anima/desktop/vulkan_renderer.hpp>
#include <anima/scene.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>

namespace resource_test {
inline void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
using rejection::rejects;
// Failures injected into set_scenes() and prepare_mesh() report their stage, no initialization and wording that
// fits both calls.
template <class F> void injected(anima::RendererFailureStage stage, F action) {
    const auto expected =
        "Injected resource preparation failure after " + std::string(anima::renderer_failure_name(stage));
    try {
        action();
    } catch (const anima::InjectedRendererFailure &error) {
        if (error.stage() != stage || error.initialization() || error.what() != expected)
            throw std::runtime_error("Injected failure misreported itself: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("Expected an injected resource failure");
}
inline std::shared_ptr<const anima::Asset> fixture() {
    auto asset = std::make_shared<anima::Asset>();
    asset->nodes.resize(2);
    asset->skins.push_back({{0, 1}, {anima::identity(), anima::identity()}});
    asset->materials.push_back({"test surface", {.5F, .7F, .9F}, 0});
    asset->textures.push_back({std::make_shared<anima::Image>(anima::Image{
                                   2, 2, {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255}}),
                               {}});
    anima::SourcePrimitive primitive;
    primitive.skin = 0;
    primitive.material = 0;
    for (auto p : {anima::Vec3{-.5F, 0, 0}, anima::Vec3{.5F, 0, 0}, anima::Vec3{.5F, 1, 0}, anima::Vec3{-.5F, 0, 0},
                   anima::Vec3{.5F, 1, 0}, anima::Vec3{-.5F, 1, 0}}) {
        anima::SourceVertex v;
        v.position = p;
        v.normal = anima::normalized({.2F, .3F, 1});
        v.color = {.7F, .8F, .9F};
        v.uv = {p.x + .5F, p.y};
        v.joints = {0, 1, 0, 0};
        v.weights = {.3F, .7F, 0, 0};
        primitive.vertices.push_back(v);
    }
    asset->primitives.push_back(primitive);
    primitive.skin = anima::no_index;
    primitive.node = 1;
    for (auto &v : primitive.vertices) {
        v.position.x += 1.2F;
        v.color = {.8F, .5F, .3F};
    }
    asset->primitives.push_back(primitive);
    return asset;
}
inline int run(int argc, char **argv) {
    require(argc >= 3, "Usage: consumer --resources OUTPUT [--asset GLB] [--hide NODE-FRAGMENT] [--fatal STAGE | "
                       "--prepare-fatal STAGE]");
    const std::filesystem::path output = argv[2];
    std::filesystem::path path;
    std::string hide, fatal;
    bool prepare_fatal = false;
    for (int i = 3; i < argc; ++i) {
        const std::string argument = argv[i];
        require(i + 1 < argc, "Missing resource consumer option value");
        if (argument == "--asset")
            path = argv[++i];
        else if (argument == "--hide")
            hide = argv[++i];
        else if (argument == "--fatal")
            fatal = argv[++i];
        else if (argument == "--prepare-fatal") {
            fatal = argv[++i];
            prepare_fatal = true;
        } else
            throw std::invalid_argument("Unknown resource consumer option");
    }
    require(fatal.empty() || fatal == "upload-timeout" || fatal == "device-lost", "Unknown resource fatal injection");
    const auto asset = path.empty() ? fixture() : anima::load_asset(path);
    const auto compiled = anima::Mesh::compile(*asset);
    auto source = std::make_shared<anima::Scene>();
    const auto a = source->create({}, compiled), b = source->create({}, compiled);
    std::array reference{anima::make_mesh_snapshot(*asset, anima::sample_pose(*asset)),
                         anima::make_mesh_snapshot(*asset, anima::sample_pose(*asset))};
    for (std::size_t i = 0; i < asset->primitives.size(); ++i)
        if (!hide.empty() && asset->nodes[asset->primitives[i].node].name.find(hide) != std::string::npos) {
            a.renderer().set_primitive_visible(i, false);
            b.renderer().set_primitive_visible(i, false);
            reference[0].primitives[i].visible = reference[1].primitives[i].visible = false;
        }
    const auto pose_at = [&](double time) {
        if (!asset->animations.empty()) {
            const auto found = std::find_if(asset->animations.begin(), asset->animations.end(),
                                            [](const auto &clip) { return clip.name == "Idle"; });
            return anima::sample_pose(*asset, found == asset->animations.end() ? &asset->animations.front() : &*found,
                                      time);
        }
        auto pose = anima::sample_pose(*asset);
        anima::Transform t;
        t.scale = {.7F, 1.2F, 1.6F};
        t.rotation = {0, 0, float(std::sin(time * .5)), float(std::cos(time * .5))};
        pose.world[1] = anima::matrix(t);
        return pose;
    };
    const auto pose_a = pose_at(.2), pose_b = pose_at(.7);
    auto left = anima::identity(), right = anima::identity();
    left[12] = -1.2F;
    right[12] = 1.2F;
    a.renderer().set_pose(pose_a, left);
    b.renderer().set_pose(pose_b, right);
    anima::pose_mesh_snapshot(*asset, pose_a, reference[0], 0, left);
    anima::pose_mesh_snapshot(*asset, pose_b, reference[1], 0, right);
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    gpu_check::Video video;
    const auto window =
        gpu_check::window("Anima shared GPU resources", 960, 720, SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE);
    anima::RendererOptions settings;
    settings.validation = true;
    settings.profile = true;
    anima::VulkanRenderer renderer(window.get(), settings);
    anima::OrbitCamera camera;
    camera.frame(source->bounds().minimum, source->bounds().maximum);
    int width{}, height{};
    SDL_GetWindowSizeInPixels(window.get(), &width, &height);
    renderer.set_view(camera.matrix(float(width) / height));
    const auto started = std::chrono::steady_clock::now();
    auto frame = [&] {
        for (;;) {
            require(std::chrono::steady_clock::now() - started < gpu_check::watchdog, "Resource consumer watchdog");
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                        "Resource consumer interrupted");
                if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                    renderer.request_resize();
            }
            if (renderer.draw())
                break;
            SDL_Delay(5);
        }
    };
    // Images are compared as soon as both sides exist and released once no later comparison needs them. GPU
    // skinning must match the CPU-posed reference within gpu_check::Captures::require_parity's tolerance, and
    // every rollback must leave the accepted image exactly as it was.
    gpu_check::Captures images(output);
    auto capture = [&](const std::string &name) {
        renderer.request_capture();
        frame();
        images.add(name, gpu_check::take(renderer));
    };
    renderer.set_scenes({reference_test::scene(reference)});
    capture("reference");
    const std::array pending{compiled, compiled};
    const anima::MeshPreparation prepared(compiled);
    renderer.prepare_mesh(prepared);
    renderer.prepare_meshes(pending);
    const auto prepared_uploads = renderer.resource_stats().mesh_uploads;
    require(prepared_uploads == reference.size() + 1 && renderer.resource_stats().cached_assets == reference.size() + 1,
            "Resource preparation duplicated a shared upload");
    capture("preloaded-reference");
    images.require_same("preloaded-reference", "reference", "Preparation changed the selected scene");
    images.discard({"preloaded-reference"});
    {
        const std::array invalid{anima::Mesh::compile(*asset), std::shared_ptr<const anima::Mesh>{}};
        rejects<std::invalid_argument>([&] { renderer.prepare_meshes(invalid); }, "Cannot prepare a null mesh");
        require(renderer.resource_stats().mesh_uploads == prepared_uploads,
                "Invalid preparation partially uploaded its inputs");
    }
    renderer.set_scenes({source});
    capture("gpu");
    const auto baseline = renderer.resource_stats();
    require(baseline.mesh_uploads == prepared_uploads && baseline.cached_assets == 1 && baseline.instances == 2,
            "Two instances did not share one GPU resource");
    require(baseline.pose_uploaded_bytes == 2 * compiled->palette_size() * sizeof(anima::Mat4),
            "Unexpected dynamic pose upload");
    require(renderer.frame_profile().uploaded_bytes == baseline.pose_uploaded_bytes,
            "Frame uploaded geometry instead of poses");
    if (!fatal.empty()) {
        auto candidate = std::make_shared<anima::Scene>();
        (void)candidate->create({}, anima::Mesh::compile(*asset));
        // The timeout reports the failed wait; the lost device is reported once the upload is retired.
        const bool timeout = fatal == "upload-timeout";
        constexpr std::string_view timed_out = "Injected upload timeout failed (VkResult 2)";
        if (prepare_fatal) {
            const std::array candidate_meshes{candidate->instance(candidate->instances().front()).mesh};
            const anima::MeshPreparation candidate_preparation(candidate_meshes.front());
            rejects<anima::RendererFatalError>(
                [&] { renderer.prepare_mesh(candidate_preparation, {anima::parse_renderer_failure_stage(fatal)}); },
                timeout ? timed_out : "Device lost while retiring resource preparation");
        } else
            rejects<anima::RendererFatalError>(
                [&] { renderer.set_scenes({candidate}, {anima::parse_renderer_failure_stage(fatal)}); },
                timeout ? timed_out : "Device lost while retiring resource upload");
        rejects<anima::RendererFatalError>([&] { (void)renderer.draw(); },
                                           "Renderer has a fatal failure; only shutdown is legal");
        const auto stats = renderer.shutdown();
        require(!stats.validation_errors && !stats.validation_warnings, "Fatal resource cleanup validation failed");
        std::cout << "PASS resource fatal classification " << fatal << '\n';
        return 0;
    }
    constexpr double least_change = .001;
    images.require_foreground("gpu", "The GPU-skinned instances are not visible");
    images.require_parity("reference", "gpu");
    images.discard({"reference"});
    const auto visible = source->instance(a.id()).primitive_visible;
    for (std::size_t i = 0; i < visible.size(); ++i) {
        a.renderer().set_primitive_visible(i, false);
        reference[0].primitives[i].visible = false;
    }
    capture("gpu-hidden");
    renderer.set_scenes({reference_test::scene(reference)});
    capture("reference-hidden");
    renderer.set_scenes({source});
    a.renderer().set_visible(false);
    capture("gpu-hidden-instance");
    require(renderer.resource_stats().instances == 1, "Hidden instance still uploaded a pose");
    images.require_parity("reference-hidden", "gpu-hidden");
    images.require_same("gpu-hidden-instance", "gpu-hidden", "Hiding the instance differs from hiding its primitives");
    images.require_changed("gpu", "gpu-hidden", least_change, "Hiding the primitives had no visible effect");
    images.discard({"reference-hidden", "gpu-hidden", "gpu-hidden-instance"});
    a.renderer().set_visible(true);
    for (std::size_t i = 0; i < visible.size(); ++i) {
        a.renderer().set_primitive_visible(i, visible[i]);
        reference[0].primitives[i].visible = visible[i];
    }
    frame();
    const auto churn_uploads = renderer.resource_stats().mesh_uploads;
    for (unsigned i = 0; i < 16; ++i) {
        auto joined = source->create({}, compiled);
        joined.renderer().set_pose(pose_at(.1 * i));
        frame();
        joined.destroy();
        frame();
        require(renderer.resource_stats().mesh_uploads == churn_uploads &&
                    renderer.resource_stats().resident_geometry_bytes == baseline.resident_geometry_bytes,
                "Join/leave duplicated or reuploaded a shared mesh");
    }
    capture("after-churn");
    images.require_same("after-churn", "gpu", "Instances joining and leaving changed the accepted image");
    images.discard({"after-churn"});
    for (const auto *name : {"vertex", "index", "texture", "texture-upload", "descriptors", "ready"}) {
        const auto stage = anima::parse_renderer_failure_stage(name);
        {
            const std::array candidate{anima::Mesh::compile(*asset)};
            const anima::MeshPreparation candidate_preparation(candidate.front());
            injected(stage, [&] { renderer.prepare_mesh(candidate_preparation, {stage}); });
        }
        const auto preload = std::string("preload-rollback-") + name;
        capture(preload);
        images.require_same(preload, "gpu", "A failed preparation changed the accepted image");
        {
            auto candidate = std::make_shared<anima::Scene>();
            (void)candidate->create({}, anima::Mesh::compile(*asset));
            injected(stage, [&] { renderer.set_scenes({candidate}, {stage}); });
        }
        const auto rollback = std::string("rollback-") + name;
        capture(rollback);
        images.require_same(rollback, "gpu", "A failed selection changed the accepted image");
        images.discard({preload, rollback});
    }
    {
        auto candidate = std::make_shared<anima::Scene>();
        for (unsigned i = 0; i < 33; ++i)
            (void)candidate->create({}, compiled);
        injected(anima::RendererFailureStage::palette,
                 [&] { renderer.set_scenes({candidate}, {anima::RendererFailureStage::palette}); });
        capture("rollback-palette");
        images.require_same("rollback-palette", "gpu", "A failed palette upload changed the accepted image");
        images.discard({"rollback-palette"});
    }
    a.renderer().set_pose(pose_at(.6), left);
    auto colored = *asset;
    colored.materials[0].factor = {.9F, .2F, .3F};
    anima::pose_mesh_snapshot(colored, pose_at(.6), reference[0], 0, left);
    a.renderer().set_material_factor(0, {.9F, .2F, .3F});
    capture("gpu-updated");
    renderer.set_scenes({reference_test::scene(reference)});
    capture("reference-updated");
    images.require_parity("reference-updated", "gpu-updated");
    renderer.set_scenes({source});
    capture("gpu-restored-scene");
    images.require_same("gpu-restored-scene", "gpu-updated", "Selecting the scene again changed its image");
    images.require_changed("gpu", "gpu-updated", least_change, "The pose and material update had no visible effect");
    images.discard({"reference-updated", "gpu-updated", "gpu-restored-scene"});
    const auto restored = renderer.resource_stats();
    require(restored.cached_assets == 1, "Unused failed candidate resources were retained");
    {
        const std::array temporary{anima::Mesh::compile(*asset)};
        renderer.prepare_meshes(temporary);
        require(renderer.resource_stats().cached_assets == 2, "Prepared resource was not retained");
    }
    frame();
    require(renderer.resource_stats().cached_assets == 1,
            "Unused prepared resource did not retire after its frame fence");
    {
        const auto temporary = anima::Mesh::compile(*asset);
        auto joined = source->create({}, temporary);
        frame();
        require(renderer.resource_stats().cached_assets == 2, "Distinct resource was not uploaded");
        joined.destroy();
    }
    frame();
    require(renderer.resource_stats().cached_assets == 1 &&
                renderer.resource_stats().resident_geometry_bytes == baseline.resident_geometry_bytes,
            "Last-owner departure did not retire GPU geometry after its frame fence");
    if (path.empty()) {
        const auto samplers = renderer.resource_stats().resident_material_samplers;
        std::vector<anima::GameObject> joined;
        // Distinct assets previously allocated more than the M2's 1,024-sampler
        // limit even though every image used the same two sampling policies.
        for (unsigned i = 0; i < 600; ++i)
            joined.push_back(source->create({}, anima::Mesh::compile(*asset)));
        frame();
        require(renderer.resource_stats().cached_assets == 601 &&
                    renderer.resource_stats().resident_material_samplers == samplers,
                "Distinct images did not share identical material samplers");
        for (auto &object : joined)
            object.destroy();
        frame();
        require(renderer.resource_stats().cached_assets == 1, "Sampler sharing retained departed assets");
        for (unsigned policy = 0; policy < 7; ++policy) {
            auto variant = *asset;
            auto &texture = variant.textures.front();
            switch (policy) {
            case 0:
                texture.sampler.mag = anima::Filter::nearest;
                break;
            case 1:
                texture.sampler.min = anima::Filter::nearest;
                break;
            case 2:
                texture.sampler.mip = anima::Filter::nearest;
                break;
            case 3:
                texture.sampler.u = anima::Wrap::clamp;
                break;
            case 4:
                texture.sampler.v = anima::Wrap::mirror;
                break;
            case 5:
                texture.sampler.mipmapped = false;
                break;
            case 6:
                texture.image = std::make_shared<anima::Image>(anima::Image{4, 4, std::vector<std::uint8_t>(64, 255)});
                break;
            }
            auto object = source->create({}, anima::Mesh::compile(variant));
            frame();
            require(renderer.resource_stats().resident_material_samplers == samplers + 1,
                    "Different filtering, wrapping or LOD limits shared a sampler");
            object.destroy();
            // The draw() that releases the mesh destroys it, and its sampler, only once no frame in flight can draw
            // it; with two frames in flight, the next draw() does.
            frame();
            frame();
            require(renderer.resource_stats().resident_material_samplers == samplers,
                    "Last-owner departure retained an unused sampler");
        }
        // Preparing a cached mesh waits for no frame in flight, so it neither uploads nor destroys anything. With two
        // frames in flight, the draw() that releases a mesh leaves the frame before it, which may draw the mesh, in
        // flight; the mesh keeps its sampler through the call, until the next draw() has waited for that frame.
        {
            auto variant = *asset;
            variant.textures.front().sampler.mag = anima::Filter::nearest;
            auto object = source->create({}, anima::Mesh::compile(variant));
            frame();
            object.destroy();
            frame();
            const auto before_preparation = renderer.resource_stats();
            require(before_preparation.resident_material_samplers == samplers + 1,
                    "A released mesh's sampler did not outlast the frame in flight that may draw it");
            const std::array cached{compiled};
            renderer.prepare_meshes(cached);
            const auto after_preparation = renderer.resource_stats();
            require(after_preparation.mesh_uploads == before_preparation.mesh_uploads &&
                        after_preparation.cached_assets == before_preparation.cached_assets,
                    "Preparing a cached mesh uploaded it again");
            require(after_preparation.resident_material_samplers == before_preparation.resident_material_samplers,
                    "Preparing a cached mesh waited for the frames in flight");
            frame();
            require(renderer.resource_stats().resident_material_samplers == samplers,
                    "The draw() after preparing a cached mesh retained a released sampler");
        }
        std::cout << "PASS 600 distinct assets share " << samplers
                  << " material samplers; seven policy variants retire correctly; preparing a cached mesh waits for "
                     "no frame in flight\n";
    }
    renderer.set_scenes({});
    capture("empty");
    images.require_clear("empty", "An empty selection drew geometry");
    images.discard({"empty"});
    renderer.set_scenes({source});
    frame();
    const auto resources = renderer.resource_stats();
    unsigned clip_captures = 0;
    for (const auto &clip : asset->animations) {
        a.renderer().clear_material_factor(0);
        const auto pa = anima::sample_pose(*asset, &clip, .2), pb = anima::sample_pose(*asset, &clip, .7);
        a.renderer().set_pose(pa, left);
        b.renderer().set_pose(pb, right);
        anima::pose_mesh_snapshot(*asset, pa, reference[0], 0, left);
        anima::pose_mesh_snapshot(*asset, pb, reference[1], 0, right);
        const auto suffix = std::to_string(clip_captures++);
        capture("gpu-clip-" + suffix);
        renderer.set_scenes({reference_test::scene(reference)});
        capture("reference-clip-" + suffix);
        images.require_parity("reference-clip-" + suffix, "gpu-clip-" + suffix);
        images.discard({"reference-clip-" + suffix, "gpu-clip-" + suffix});
        renderer.set_scenes({source});
        frame();
    }
    require(SDL_SetWindowSize(window.get(), 800, 600), "Resource resize failed");
    require(SDL_SyncWindow(window.get()), "Resource resize did not settle");
    renderer.request_resize();
    capture("gpu-resized");
    renderer.set_scenes({reference_test::scene(reference)});
    capture("reference-resized");
    images.require_parity("reference-resized", "gpu-resized");
    images.require(!gpu_check::same_size(images["gpu-resized"], images["gpu"]),
                   "The resized window kept its capture size", {"gpu-resized", "gpu"});
    renderer.set_scenes({source});
    frame();
    const auto stats = renderer.shutdown();
    require(!stats.validation_errors && !stats.validation_warnings && stats.scene_generations == 11 + 2 * clip_captures,
            "Resource renderer validation failed or membership rebuilt the scene");
    std::cout << "RESULT {\"indexed_vertices\":" << compiled->vertices().size()
              << ",\"indices\":" << compiled->indices().size()
              << ",\"pair_pose_bytes\":" << baseline.pose_uploaded_bytes
              << ",\"geometry_bytes\":" << compiled->vertices().size_bytes() + compiled->indices().size_bytes()
              << ",\"resident_geometry_bytes\":" << baseline.resident_geometry_bytes
              << ",\"resident_texture_bytes\":" << baseline.resident_texture_bytes
              << ",\"cached_assets\":" << resources.cached_assets << ",\"draw_calls\":" << resources.draw_calls
              << ",\"resident_material_samplers\":" << resources.resident_material_samplers
              << ",\"clip_captures\":" << clip_captures << ",\"validation_warnings\":0,\"validation_errors\":0}\n";
    return 0;
}
} // namespace resource_test
