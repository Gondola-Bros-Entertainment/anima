#pragma once
#include "reference.hpp"
#include "resources.hpp"

namespace foliage_test {
inline std::shared_ptr<const anima::Asset> fixture() {
    auto asset = std::make_shared<anima::Asset>();
    asset->nodes.resize(1);
    anima::Texture texture{256, 256, std::vector<std::uint8_t>(256 * 256 * 4), {}};
    texture.sampler.mag = texture.sampler.min = texture.sampler.mip = anima::Filter::nearest;
    constexpr unsigned opaque_per_block[]{0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 2, 2, 3, 4, 4};
    for (unsigned y = 0; y < 256; ++y)
        for (unsigned x = 0; x < 256; ++x) {
            const auto block = (y % 8 / 2) * 4 + x % 8 / 2;
            const bool visible = (y % 2) * 2 + x % 2 < opaque_per_block[block];
            const auto offset = (y * 256 + x) * 4;
            texture.rgba[offset] = texture.rgba[offset + 2] = 255;
            texture.rgba[offset + 1] = texture.rgba[offset + 3] = visible ? 255 : 0;
        }
    asset->textures.push_back(std::move(texture));
    anima::Material mask;
    mask.texture = 0;
    mask.unlit = true;
    mask.alpha_mode = anima::AlphaMode::mask;
    mask.alpha_cutoff = .75F;
    auto other = mask;
    other.alpha_cutoff = .5F;
    auto opaque = mask;
    opaque.alpha_mode = anima::AlphaMode::opaque;
    auto emission = opaque;
    emission.unlit = false;
    emission.texture = -1;
    emission.factor = {};
    emission.emissive = {1, 1, 1};
    emission.emissive_texture = 0;
    asset->materials = {mask, other, opaque, emission};
    for (int material = 0; material < 4; ++material) {
        anima::SourcePrimitive primitive;
        primitive.material = material;
        for (const auto uv : {std::array<float, 2>{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}}) {
            anima::SourceVertex vertex;
            vertex.position = {uv[0] * 2 - 1, uv[1] * 2 - 1, 0};
            vertex.normal = {0, 0, 1};
            vertex.uv = uv;
            primitive.vertices.push_back(vertex);
        }
        asset->primitives.push_back(std::move(primitive));
    }
    return asset;
}
inline int run(int argc, char **argv) {
    using resource_test::require;
    require(argc == 3, "Usage: consumer --foliage OUTPUT");
    const std::filesystem::path output = argv[2];
    std::filesystem::create_directories(output);
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    require(SDL_Init(SDL_INIT_VIDEO), "Foliage SDL initialization failed");
    struct Quit {
        ~Quit() { SDL_Quit(); }
    } quit;
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window{
        SDL_CreateWindow("Anima foliage verification", 512, 512, SDL_WINDOW_VULKAN), SDL_DestroyWindow};
    require(bool(window), "Foliage window failed");
    anima::RendererOptions options;
    options.validation = true;
    anima::VulkanRenderer renderer(window.get(), options);
    anima::Environment environment;
    environment.sun.radiance = environment.fill.radiance = environment.ambient_sky = environment.ambient_ground =
        environment.ambient_specular = {};
    renderer.set_environment(environment);
    const auto asset = fixture();
    auto scene = std::make_shared<anima::Scene>();
    const auto id = scene->add(anima::Mesh::compile(*asset));
    renderer.set_scenes({scene});
    const auto began = std::chrono::steady_clock::now();
    auto capture = [&](const std::string &name) {
        renderer.request_capture(output / (name + ".ppm"));
        for (;;) {
            require(std::chrono::steady_clock::now() - began < std::chrono::seconds(45), "Foliage watchdog");
            SDL_Event event{};
            while (SDL_PollEvent(&event))
                require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                        "Foliage test interrupted");
            if (renderer.draw())
                break;
            SDL_Delay(5);
        }
    };
    for (const unsigned size : {256U, 128U}) {
        auto projection = anima::identity();
        projection[0] = projection[5] = float(size) / 512;
        projection[14] = .5F;
        renderer.set_view(projection);
        for (std::size_t material = 0; material < 4; ++material) {
            for (std::size_t p = 0; p < 4; ++p)
                scene->set_primitive_visible(id, p, p == material);
            capture(std::to_string(size) + "-" + std::to_string(material));
        }
    }
    for (std::size_t p = 0; p < 4; ++p)
        scene->set_primitive_visible(id, p, p == 0);
    capture("resource-mask");
    auto reference = std::make_shared<anima::MeshSnapshot>(scene->snapshot());
    renderer.set_scenes({reference_test::scene(*reference)});
    capture("reference-mask");
    renderer.set_scenes({scene});
    capture("restored-mask");
    const auto stats = renderer.shutdown();
    require(!stats.validation_errors && !stats.validation_warnings, "Foliage GPU validation failed");
    std::cout << "PASS foliage: mip coverage, independent material uses, reference parity and replacement\n";
    return 0;
}
} // namespace foliage_test
