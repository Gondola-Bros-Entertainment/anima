#pragma once
#include "resources.hpp"

namespace culling_test {
inline int run(int argc, char **argv) {
    using resource_test::require;
    require(argc == 3 || argc == 5, "Usage: consumer --culling OUTPUT [--asset GLB]");
    const std::filesystem::path output = argv[2];
    if (argc == 5)
        require(std::string_view(argv[3]) == "--asset", "Expected --asset");
    const auto asset = argc == 5 ? anima::load_asset(argv[4]) : resource_test::fixture();
    const auto compiled = anima::Mesh::compile(*asset);
    auto scene = std::make_shared<anima::Scene>();
    const auto a = scene->add(compiled);
    const auto bounds = scene->bounds();
    anima::OrbitCamera camera;
    camera.frame(bounds.minimum, bounds.maximum);
    const auto center = camera.target;
    const auto radius = camera.radius;
    auto remote_world = anima::identity();
    remote_world[12] = 30 * radius;
    const auto b = scene->add(compiled);
    scene->set_pose(b, compiled->rest_pose(), remote_world);
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    require(SDL_Init(SDL_INIT_VIDEO), "Culling consumer SDL initialization failed");
    struct Quit {
        ~Quit() { SDL_Quit(); }
    } quit;
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window{
        SDL_CreateWindow("Anima culling verification", 800, 600,
                         SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE),
        SDL_DestroyWindow};
    require(bool(window), "Culling consumer window failed");
    anima::RendererOptions options;
    options.validation = true;
    options.profile = true;
    anima::VulkanRenderer renderer(window.get(), options);
    int width{}, height{};
    SDL_GetWindowSizeInPixels(window.get(), &width, &height);
    auto view = camera.matrix(float(width) / height);
    renderer.set_view(view);
    renderer.set_scenes({scene});
    const auto original_palette_a = scene->instance(a).palette;
    const auto original_palette_b = scene->instance(b).palette;
    const auto started = std::chrono::steady_clock::now();
    std::filesystem::create_directories(output);
    auto frame = [&] {
        for (;;) {
            require(std::chrono::steady_clock::now() - started < std::chrono::seconds(90), "Culling watchdog");
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                        "Culling test interrupted");
                if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                    renderer.request_resize();
            }
            if (renderer.draw())
                return;
            SDL_Delay(5);
        }
    };
    auto compare = [&](const std::string &name) {
        renderer.set_frustum_culling(false);
        renderer.request_capture(output / (name + "-off.ppm"));
        frame();
        const auto unculled = renderer.resource_stats();
        require(unculled.draw_calls == unculled.candidate_draws && !unculled.culled_draws && !unculled.culled_instances,
                "Disabled culling omitted visible draws");
        renderer.set_frustum_culling(true);
        renderer.request_capture(output / (name + "-on.ppm"));
        frame();
        const auto culled = renderer.resource_stats();
        require(culled.draw_calls + culled.culled_draws == culled.candidate_draws &&
                    culled.instances + culled.culled_instances == culled.candidate_instances,
                "Culling counters do not account for candidates");
        require(culled.mesh_uploads == unculled.mesh_uploads &&
                    culled.resident_geometry_bytes == unculled.resident_geometry_bytes,
                "Culling changed geometry residency");
        std::cout << "CASE {\"name\":\"" << name << "\",\"candidate_draws\":" << culled.candidate_draws
                  << ",\"draw_calls\":" << culled.draw_calls << ",\"culled_draws\":" << culled.culled_draws
                  << ",\"culled_instances\":" << culled.culled_instances
                  << ",\"pose_bytes_off\":" << unculled.pose_uploaded_bytes
                  << ",\"pose_bytes_on\":" << culled.pose_uploaded_bytes
                  << ",\"indices_off\":" << unculled.submitted_indices << ",\"indices_on\":" << culled.submitted_indices
                  << "}\n";
        return culled;
    };
    const auto baseline = compare("visible");
    require(baseline.instances == 1 && baseline.culled_instances == 1 && baseline.draw_calls,
            "Off-camera instance was not culled");
    auto group = scene->create("activation group");
    scene->object(a).set_parent(group);
    group.set_active(false);
    const auto inactive = compare("hierarchy-inactive");
    require(inactive.candidate_instances == 1 && !inactive.draw_calls && scene->instance(a).visible &&
                !scene->instance(a).active,
            "Inactive hierarchy submitted rendering or overwrote visibility");
    group.set_active(true);
    const auto reactivated = compare("hierarchy-reactivated");
    require(reactivated.instances == 1 && reactivated.mesh_uploads == baseline.mesh_uploads,
            "Reactivation lost rendering or reuploaded shared mesh");
    require(scene->instance(a).palette == original_palette_a && scene->instance(b).palette == original_palette_b,
            "Culling changed the source poses");
    camera.target.x += 100 * radius;
    renderer.set_view(camera.matrix(float(width) / height));
    const auto empty = compare("all-outside");
    require(!empty.draw_calls && !empty.pose_uploaded_bytes && !empty.instances && empty.culled_instances == 2,
            "Empty view submitted geometry or palettes");
    // Change independent poses and material/visibility while neither actor is
    // drawn. Returning to view must immediately render the current scene state.
    auto pose = compiled->rest_pose();
    if (!asset->animations.empty())
        pose = anima::sample_pose(*asset, &asset->animations.back(), .7);
    else
        pose.world[1][13] = .35F;
    scene->set_pose(a, pose);
    scene->set_pose(b, pose, remote_world);
    if (!scene->instance(b).factors.empty())
        scene->set_material_factor(b, 0, {.8F, .3F, .2F});
    frame();
    camera.target = center;
    camera.target.x += 30 * radius;
    renderer.set_view(camera.matrix(float(width) / height));
    const auto returned = compare("returned");
    require(returned.instances == 1 && returned.mesh_uploads == baseline.mesh_uploads,
            "Returning instance reuploaded geometry or disappeared");
    camera.target = center;
    renderer.set_view(camera.matrix(float(width) / height));
    unsigned clips = 0;
    if (argc == 3) {
        auto orthographic = anima::identity();
        orthographic[12] = .5F;
        orthographic[14] = .5F;
        renderer.set_view(orthographic);
        const auto partial = compare("partial-instance");
        require(partial.instances == 1 && partial.draw_calls == 1 && partial.culled_draws == 3,
                "Primitive-level culling did not refine the instance broad phase");
    }
    for (const auto &clip : asset->animations) {
        scene->set_pose(a, anima::sample_pose(*asset, &clip, .2));
        compare("clip-" + std::to_string(clips++));
    }
    // Move the camera across object edges and clip through the object with the
    // near plane. Exact on/off images are checked by the Python driver.
    for (const auto offset : {anima::Vec3{-radius, 0, 0}, anima::Vec3{radius, 0, 0}, anima::Vec3{0, -radius, 0},
                              anima::Vec3{0, radius, 0}}) {
        camera.target = center + offset;
        renderer.set_view(camera.matrix(float(width) / height));
        compare("edge-" + std::to_string(clips++));
    }
    const auto eye = center + anima::Vec3{0, 0, 3 * radius};
    renderer.set_view(anima::operator*(anima::perspective(float(width) / height, 3 * radius, 50 * radius),
                                       anima::look_at(eye, center)));
    compare("near-plane");
    renderer.set_view(anima::operator*(anima::perspective(float(width) / height, .01F * radius, 3 * radius),
                                       anima::look_at(eye, center)));
    compare("far-plane");
    camera.target = center;
    require(SDL_SetWindowSize(window.get(), 600, 800), "Culling resize failed");
    renderer.request_resize();
    SDL_GetWindowSizeInPixels(window.get(), &width, &height);
    renderer.set_view(camera.matrix(float(width) / height));
    compare("resized");
    scene->set_visible(a, false);
    compare("explicit-hidden");
    scene->set_visible(a, true);
    for (std::size_t i = 0; i < scene->instance(a).primitive_visible.size(); ++i)
        scene->set_primitive_visible(a, i, false);
    const auto hidden = compare("primitives-hidden");
    require(!hidden.draw_calls && hidden.candidate_instances == 1, "Explicit hidden draws counted as candidates");
    const auto stats = renderer.shutdown();
    require(!stats.validation_errors && !stats.validation_warnings && stats.scene_generations == 2,
            "Culling validation failed or rebuilt the scene");
    std::cout
        << "PASS culling resource ownership, reentry and camera changes; validation_warnings=0 validation_errors=0\n";
    return 0;
}
} // namespace culling_test
