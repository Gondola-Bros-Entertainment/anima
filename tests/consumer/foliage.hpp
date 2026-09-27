#pragma once
#include "gpu_checks.hpp"
#include "reference.hpp"
#include "resources.hpp"

namespace foliage_test {
inline std::shared_ptr<const anima::Asset> fixture() {
    auto asset = std::make_shared<anima::Asset>();
    asset->nodes.resize(1);
    anima::Image image{256, 256, std::vector<std::uint8_t>(256 * 256 * 4)};
    constexpr unsigned opaque_per_block[]{0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 2, 2, 3, 4, 4};
    for (unsigned y = 0; y < 256; ++y)
        for (unsigned x = 0; x < 256; ++x) {
            const auto block = (y % 8 / 2) * 4 + x % 8 / 2;
            const bool visible = (y % 2) * 2 + x % 2 < opaque_per_block[block];
            const auto offset = (y * 256 + x) * 4;
            image.rgba[offset] = image.rgba[offset + 2] = 255;
            image.rgba[offset + 1] = image.rgba[offset + 3] = visible ? 255 : 0;
        }
    anima::Texture texture{std::make_shared<anima::Image>(std::move(image)), {}};
    texture.sampler.mag = texture.sampler.min = texture.sampler.mip = anima::Filter::nearest;
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
    static constexpr unsigned window_size = 512;
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    gpu_check::Video video;
    const auto window = gpu_check::window("Anima foliage verification", int(window_size), int(window_size));
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
    gpu_check::Captures captures(output);
    const auto began = std::chrono::steady_clock::now();
    auto capture = [&](const std::string &name) {
        renderer.request_capture();
        for (;;) {
            require(std::chrono::steady_clock::now() - began < gpu_check::watchdog, "Foliage watchdog");
            SDL_Event event{};
            while (SDL_PollEvent(&event))
                require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                        "Foliage test interrupted");
            if (renderer.draw())
                break;
            SDL_Delay(5);
        }
        captures.add(name, gpu_check::take(renderer));
    };
    for (const unsigned size : {256U, 128U}) {
        auto projection = anima::identity();
        projection[0] = projection[5] = float(size) / window_size;
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
    // The quad covers a centered square of the texture's size or half of it. The mask texture's coverage per 8x8
    // block survives minification: 22/64 of the full-size texels and 5/16 at half size pass both cutoffs.
    const auto square = [&](const std::string &name, unsigned size) {
        const auto &image = captures[name];
        captures.require(image.width == window_size && image.height == window_size,
                         name + " is " + std::to_string(image.width) + "x" + std::to_string(image.height) +
                             ", not the window's 512x512",
                         {name});
        std::vector<gpu_check::Rgb> pixels;
        const auto offset = (window_size - size) / 2;
        for (unsigned y = offset; y < offset + size; ++y)
            for (unsigned x = offset; x < offset + size; ++x)
                pixels.push_back(gpu_check::pixel(image, x, y));
        return pixels;
    };
    static constexpr int visible_level = 245;
    constexpr double coverage_tolerance = .001, brightest_opaque_green = 180;
    for (const auto &[size, expected] : {std::pair{256U, 22. / 64}, std::pair{128U, 5. / 16}}) {
        const auto prefix = std::to_string(size) + "-";
        const auto masked = square(prefix + "0", size);
        const auto visible = std::count_if(masked.begin(), masked.end(), [](const gpu_check::Rgb &p) {
            return std::min({p[0], p[1], p[2]}) > visible_level;
        });
        const auto fraction = double(visible) / double(masked.size());
        captures.require(std::abs(fraction - expected) < coverage_tolerance,
                         prefix + "0 keeps " + std::to_string(fraction) + " of its square visible instead of " +
                             std::to_string(expected),
                         {prefix + "0"});
        // Different cutoffs need separate mips, yet both recover this fixture's footprint; opaque and emissive
        // uses keep the ordinary RGB of the mask's transparent texels.
        captures.require_same(prefix + "0", prefix + "1", "The cutoff variants lost their common footprint");
        captures.require_same(prefix + "2", prefix + "3", "Mask processing leaked into emission");
        const auto opaque = square(prefix + "2", size);
        double green = 0;
        for (const auto &p : opaque)
            green += p[1];
        green /= double(opaque.size());
        captures.require(green < brightest_opaque_green,
                         prefix + "2 has a mean green of " + std::to_string(green) +
                             ": the transparent magenta RGB was removed from opaque use",
                         {prefix + "2"});
    }
    captures.require_same("resource-mask", "reference-mask", "The scene and its CPU reference differ");
    captures.require_same("resource-mask", "restored-mask", "Replacing the scene changed its image");
    std::cout << "PASS foliage: minified coverage, distinct cutoffs, ordinary RGB and emission isolation and scene "
                 "parity, with clean validation\n";
    return 0;
}
} // namespace foliage_test
