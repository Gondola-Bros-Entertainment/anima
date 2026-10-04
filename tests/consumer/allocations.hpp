#pragma once
#include "gpu_checks.hpp"
#include "resources.hpp"

// Caches and draws more meshes than a renderer that gave each buffer and image its own Vulkan allocation could
// hold under a maxMemoryAllocationCount of 4096, the least that Vulkan allows, with clean validation.
namespace allocation_test {
inline int run(int argc, char **argv) {
    using resource_test::require;
    require(argc == 3, "Usage: consumer --allocations OUTPUT");
    const std::filesystem::path output = argv[2];
    // Each cached copy of the fixture holds at least five buffers and images: its vertex, index and material
    // uniform buffers, the white image that every material plan includes, and the fixture's texture.
    constexpr std::size_t meshes = 1000, least_allocation_limit = 4096, columns = 40;
    static_assert(meshes * 5 > least_allocation_limit);
    const auto asset = resource_test::fixture();
    auto scene = std::make_shared<anima::Scene>();
    for (std::size_t i = 0; i < meshes; ++i) {
        // The renderer caches resources per Mesh object, so each instance gets its own.
        const auto mesh = anima::Mesh::compile(*asset);
        const auto id = scene->add(mesh);
        auto world = anima::identity();
        world[12] = 1.5F * float(i % columns);
        world[13] = 1.5F * float(i / columns);
        scene->set_pose(id, mesh->rest_pose(), world);
    }
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    gpu_check::Video video;
    const auto window = gpu_check::window("Anima allocation verification", 640, 480, SDL_WINDOW_HIGH_PIXEL_DENSITY);
    anima::RendererOptions options;
    options.validation = true;
    options.log = gpu_check::log;
    anima::VulkanRenderer renderer(window.get(), options);
    anima::OrbitCamera camera;
    const auto bounds = scene->bounds();
    camera.frame(bounds.minimum, bounds.maximum);
    int width{}, height{};
    SDL_GetWindowSizeInPixels(window.get(), &width, &height);
    renderer.set_view(camera.matrix(float(width) / float(height)));
    renderer.set_scenes({scene});
    const auto started = std::chrono::steady_clock::now();
    const auto frame = [&] {
        for (;;) {
            require(std::chrono::steady_clock::now() - started < gpu_check::watchdog, "Allocation watchdog");
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                        "Allocation test interrupted");
                if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                    renderer.request_resize();
            }
            if (renderer.draw())
                return;
            SDL_Delay(5);
        }
    };
    gpu_check::Captures captures(output);
    renderer.request_capture();
    frame();
    captures.add("meshes", gpu_check::take(renderer));
    require(renderer.resource_stats().cached_assets == meshes, "The renderer did not cache every mesh");
    captures.require_foreground("meshes", "The cached meshes are not visible");
    const auto stats = renderer.shutdown();
    require(!stats.validation_errors && !stats.validation_warnings, "Allocation check validation failed");
    std::cout << "PASS allocations: " << meshes << " meshes, each with its own buffers and images, cached and drawn "
              << "with clean validation\n";
    return 0;
}
} // namespace allocation_test
