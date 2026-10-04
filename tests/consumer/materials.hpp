#pragma once
// Material checks: texture sampling (sRGB decoding, filters, wrap modes, minification), metallic-roughness
// shading, glTF surface maps and single-sided culling. Each fixture is a GLB built in memory and imported with
// anima::load_asset, then rendered as the viewer shows a static asset: framed by an OrbitCamera, lit by the default
// Environment.
#include "gltf_fixture.hpp"
#include "gpu_checks.hpp"
#include "rejection.hpp"
#include <anima/assets/asset.hpp>
#include <anima/mesh_placements.hpp>
#include <anima/scene.hpp>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <string_view>
#include <utility>

namespace material_test {
inline void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
// glTF sampler and wrap enumerants.
constexpr int nearest = 9728, linear = 9729, linear_mipmap_linear = 9987;
constexpr int repeat = 10497, clamp_to_edge = 33071, mirrored_repeat = 33648;
// A quad from -1 to 1 in X and Y as two triangles, counterclockwise seen from +Z.
constexpr std::array<std::array<float, 2>, 6> quad_corners{{{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}}};

inline double srgb_to_linear(double byte) {
    const auto s = byte / 255;
    return s <= .04045 ? s / 12.92 : std::pow((s + .055) / 1.055, 2.4);
}
inline double linear_to_srgb(double value) {
    return value <= .0031308 ? 12.92 * value : 1.055 * std::pow(value, 1 / 2.4) - .055;
}
// The default OrbitCamera around a quad from -1 to 1 in X and Y: yaw 0.3, pitch 0.12, three radii of sqrt(2) from
// the origin, with a 45-degree vertical field of view.
constexpr double orbit_yaw = .3, orbit_pitch = .12;
inline double orbit_distance() { return 3 * std::numbers::sqrt2; }
// The pixel showing quad point (u - 1, v - 1, 0), truncated toward zero as the removed Python checks did.
inline gpu_check::Rgb sample(const gpu_check::Image &image, double u, double v) {
    const double sy = std::sin(orbit_yaw), cy = std::cos(orbit_yaw), sp = std::sin(orbit_pitch),
                 cp = std::cos(orbit_pitch);
    const double cotangent = 1 + std::numbers::sqrt2; // 1 / tan(22.5 degrees)
    const double x = u - 1, y = v - 1, aspect = double(image.width) / image.height;
    const auto depth = orbit_distance() - sy * cp * x - sp * y;
    const auto px = std::trunc((1 + cotangent / aspect * cy * x / depth) * image.width / 2);
    const auto py = std::trunc((1 - cotangent * (-sy * sp * x + cp * y) / depth) * image.height / 2);
    return gpu_check::pixel(image, std::size_t(px), std::size_t(py));
}
inline std::array<double, 3> unit(const std::array<double, 3> &v) {
    const auto length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return {v[0] / length, v[1] / length, v[2] / length};
}
inline std::array<double, 3> default_light() { return unit({-.6, .9, .8}); }
inline std::array<double, 3> default_view() {
    return {std::sin(orbit_yaw) * std::cos(orbit_pitch), std::sin(orbit_pitch),
            std::cos(orbit_yaw) * std::cos(orbit_pitch)};
}

// A scene of one object: the mesh that @p glb compiles to, placed at @p world.
inline std::shared_ptr<const anima::Scene> imported_scene(const std::vector<std::byte> &glb,
                                                          const anima::Mat4 &world = anima::identity()) {
    const auto asset = anima::load_asset(std::span<const std::byte>(glb));
    auto scene = std::make_shared<anima::Scene>();
    scene->create({}, anima::Mesh::compile(*asset)).set_world_matrix(world);
    return scene;
}

// Renders fixtures one at a time in one window and keeps each first frame by name.
class Harness {
  public:
    /// A camera at #position looking at #target.
    struct Eye {
        anima::Vec3 position, target;
    };
    /// Renders with RendererOptions::max_anisotropy @p anisotropy.
    explicit Harness(const std::filesystem::path &output, float anisotropy = anima::RendererOptions{}.max_anisotropy)
        : window_(gpu_check::window("Anima material verification", 960, 640, SDL_WINDOW_HIGH_PIXEL_DENSITY)),
          renderer_(window_.get(), options(anisotropy)), images(output) {
        renderer_.set_environment(anima::Environment{});
    }
    /// Imports @p glb, places it at @p world, views it from @p eye or else frames it with an OrbitCamera, and reads
    /// back its first frame as @p name.
    void render(const std::string &name, const std::vector<std::byte> &glb,
                const anima::Mat4 &world = anima::identity(), const std::optional<Eye> &eye = std::nullopt) {
        render(name, imported_scene(glb, world), eye);
    }
    /// Views @p scene from @p eye or else frames it with an OrbitCamera, reads back its first frame as @p name, and
    /// keeps the frame's counters in #resources.
    void render(const std::string &name, const std::shared_ptr<const anima::Scene> &scene,
                const std::optional<Eye> &eye = std::nullopt) {
        select(scene, eye);
        capture(name);
    }
    /// Selects @p scene alone, which waits for every frame in flight, and views it from @p eye or else frames it with
    /// an OrbitCamera.
    void select(const std::shared_ptr<const anima::Scene> &scene, const std::optional<Eye> &eye = std::nullopt) {
        renderer_.set_scenes({scene});
        int width = 0, height = 0;
        require(SDL_GetWindowSizeInPixels(window_.get(), &width, &height) && width > 0 && height > 0,
                "Material window has no drawable size");
        const auto aspect = float(width) / float(height);
        if (eye) {
            constexpr float near_plane = .05F, far_plane = 200;
            renderer_.set_view(anima::perspective(std::numbers::pi_v<float> / 4, aspect, near_plane, far_plane) *
                               anima::look_at(eye->position, eye->target));
        } else {
            anima::OrbitCamera camera;
            camera.frame(scene->bounds().minimum, scene->bounds().maximum);
            renderer_.set_view(camera.matrix(aspect));
        }
    }
    /// Reads back the next frame that draw() presents as @p name, and keeps the frame's counters in #resources.
    void capture(const std::string &name) {
        renderer_.request_capture();
        present();
        images.add(name, gpu_check::take(renderer_));
        resources = renderer_.resource_stats();
    }
    /// Draws until draw() presents a frame, which it leaves in flight unless a capture waits for it.
    void present() {
        const auto started = std::chrono::steady_clock::now();
        for (;;) {
            require(std::chrono::steady_clock::now() - started < gpu_check::watchdog, "Material watchdog");
            SDL_Event event{};
            while (SDL_PollEvent(&event))
                require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                        "Material test interrupted");
            if (renderer_.draw())
                break;
            SDL_Delay(5);
        }
    }
    /// Shuts the renderer down and requires clean validation.
    void finish() {
        const auto stats = renderer_.shutdown();
        require(!stats.validation_errors && !stats.validation_warnings, "Material GPU validation failed");
    }
    /// VulkanRenderer::max_anisotropy.
    [[nodiscard]] float max_anisotropy() const noexcept { return renderer_.max_anisotropy(); }
    /// The renderer, for settings that the harness does not wrap.
    [[nodiscard]] anima::VulkanRenderer &renderer() noexcept { return renderer_; }

  private:
    static anima::RendererOptions options(float anisotropy) {
        anima::RendererOptions settings;
        settings.validation = true;
        settings.log = gpu_check::log;
        settings.max_anisotropy = anisotropy;
        return settings;
    }
    gpu_check::Video video_;
    gpu_check::Window window_;
    anima::VulkanRenderer renderer_;

  public:
    gpu_check::Captures images;
    /// VulkanRenderer::resource_stats() after the latest render().
    anima::ResourceStats resources{};
};

// A quad textured with a gray checker of 0 and 128, 2x2 texels, or 64x64 when mipmapped, repeated @p uv_extent
// times across it, as gpu_material_smoke.py generated it.
inline std::vector<std::byte> checker_quad(int wrap, int mag, int min, bool mipmapped, double uv_extent) {
    const unsigned size = mipmapped ? 64 : 2;
    std::vector<std::uint8_t> pixels;
    for (unsigned y = 0; y < size; ++y)
        for (unsigned x = 0; x < size; ++x)
            pixels.insert(pixels.end(), 3, std::uint8_t((x + y) % 2 ? 128 : 0));
    std::vector<float> vertices;
    for (const auto &[x, y] : quad_corners)
        vertices.insert(vertices.end(),
                        {x, y, 0, 0, 0, 1, float((x + 1) * uv_extent / 2), float((y + 1) * uv_extent / 2)});
    gltf_fixture::Builder builder;
    const auto first = builder.interleaved(vertices, 8, {{"VEC3", 0}, {"VEC3", 3}, {"VEC2", 6}});
    const auto image = builder.png(size, size, 3, pixels);
    return builder.glb(R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],"textures":[{"source":)" +
                       std::to_string(image) + R"(,"sampler":0}],"samplers":[{"wrapS":)" + std::to_string(wrap) +
                       R"(,"wrapT":)" + std::to_string(wrap) + R"(,"magFilter":)" + std::to_string(mag) +
                       R"(,"minFilter":)" + std::to_string(min) +
                       R"(}],"materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0},)"
                       R"("metallicFactor":0,"roughnessFactor":1}}],"meshes":[{"primitives":[{"attributes":{)"
                       R"("POSITION":)" +
                       std::to_string(first) + R"(,"NORMAL":)" + std::to_string(first + 1) + R"(,"TEXCOORD_0":)" +
                       std::to_string(first + 2) + R"(},"material":0}]}])");
}
// The closed form of a quad point's color: a rough dielectric, whose GGX distribution is 1/pi and whose
// correlated Smith visibility is 1 / (2 * (N.L + N.V)), with @p fraction of the checker's linear gray as albedo,
// lit by the default Environment. Its fill light faces away from the quad's +Z normal.
inline int expected_gray(double fraction, double u, double v) {
    const auto gray = srgb_to_linear(128);
    const auto eye = default_view();
    const auto view =
        unit({eye[0] * orbit_distance() - (u - 1), eye[1] * orbit_distance() - (v - 1), eye[2] * orbit_distance()});
    const auto light = default_light();
    const auto half = unit({view[0] + light[0], view[1] + light[1], view[2] + light[2]});
    const auto fresnel = .04 + .96 * std::pow(1 - (view[0] * half[0] + view[1] * half[1] + view[2] * half[2]), 5);
    const auto albedo = gray * fraction;
    constexpr double ambient = .4, specular_ambient = .08, reflectance = .04, sun_over_pi = .5;
    const auto value = ambient * albedo + specular_ambient * reflectance +
                       sun_over_pi * light[2] * ((1 - fresnel) * albedo + fresnel / (2 * (light[2] + view[2])));
    return int(std::nearbyint(255 * linear_to_srgb(value)));
}
// Checks sampling in one renderer with the default RendererOptions::max_anisotropy. The orbit camera sees each quad
// nearly face on, where a pixel's footprint is barely stretched and anisotropy has little to sharpen.
inline void check_filters(const std::filesystem::path &output) {
    Harness harness(output);
    auto &images = harness.images;
    // Per-pixel sRGB decoding before filtering, nearest and linear filters, the three wrap modes and trilinear
    // minification, each against the closed form within 4 levels per channel.
    using Point = std::pair<double, double>;
    const std::vector<Point> points{{.25, .25}, {.75, .25}, {1.25, .25}, {1.75, .25}, {.25, .75}};
    struct Case {
        const char *name;
        int wrap;
        bool filtered, mipmapped;
        std::vector<Point> probes;
        std::vector<double> fractions;
    };
    const std::vector<Case> cases{{"nearest-repeat", repeat, false, false, points, {0, 1, 0, 1, 1}},
                                  {"nearest-clamp", clamp_to_edge, false, false, points, {0, 1, 1, 1, 1}},
                                  {"nearest-mirror", mirrored_repeat, false, false, points, {0, 1, 1, 0, 1}},
                                  {"linear-clamp", clamp_to_edge, true, false, {{.5, .25}, {.5, .5}}, {.5, .5}},
                                  {"mip-minification", repeat, true, true, points, {.5, .5, .5, .5, .5}}};
    constexpr int tolerance = 4;
    for (const auto &c : cases) {
        const int filter = c.filtered ? linear : nearest;
        harness.render(c.name, checker_quad(c.wrap, filter, c.mipmapped ? linear_mipmap_linear : filter, c.mipmapped,
                                            c.mipmapped ? 64 : 2));
        for (std::size_t i = 0; i < c.probes.size(); ++i) {
            const auto [u, v] = c.probes[i];
            const auto actual = sample(images[c.name], u, v);
            const auto target = expected_gray(c.fractions[i], u, v);
            images.require(gpu_check::difference(actual, {target, target, target}) <= tolerance,
                           std::string(c.name) + " shows " + gpu_check::text(actual) + " at UV " + std::to_string(u) +
                               ", " + std::to_string(v) + " instead of gray " + std::to_string(target),
                           {c.name});
        }
        images.discard({c.name});
    }
    // UVs repeating 1024 times force minification of mip level zero alone: a mixed sampler must reproduce its
    // minification filter's reference whatever its magnification filter, and the references must differ.
    constexpr double dense = 1024, least_change = .01;
    harness.render("min-reference-nearest", checker_quad(repeat, nearest, nearest, false, dense));
    harness.render("min-reference-linear", checker_quad(repeat, linear, linear, false, dense));
    harness.render("min-linear-mag-nearest", checker_quad(repeat, nearest, linear, false, dense));
    harness.render("min-nearest-mag-linear", checker_quad(repeat, linear, nearest, false, dense));
    images.require_same("min-linear-mag-nearest", "min-reference-linear", "Minification used the wrong filter");
    images.require_same("min-nearest-mag-linear", "min-reference-nearest", "Minification used the wrong filter");
    images.require_changed("min-reference-nearest", "min-reference-linear", least_change,
                           "The filter references do not discriminate minification");
    harness.finish();
}

// A floor from -50 to 50 in X and Z, facing +Y, whose 64x64 texture alternates gray 0 and 255 every 4 texel
// columns and repeats 64 times across it: stripes about 10 cm wide that run along Z. @p mag and @p min are glTF
// filters.
inline std::vector<std::byte> striped_floor(int mag, int min) {
    constexpr unsigned size = 64, stripe = 4;
    constexpr float half = 50, repeats = 64;
    std::vector<std::uint8_t> pixels;
    for (unsigned y = 0; y < size; ++y)
        for (unsigned x = 0; x < size; ++x)
            pixels.insert(pixels.end(), 3, std::uint8_t(x / stripe % 2 ? 255 : 0));
    std::vector<float> vertices;
    // Quad Y becomes -Z, which keeps the winding counterclockwise seen from +Y.
    for (const auto &[x, y] : quad_corners)
        vertices.insert(vertices.end(),
                        {x * half, 0, -y * half, 0, 1, 0, (x + 1) * repeats / 2, (y + 1) * repeats / 2});
    gltf_fixture::Builder builder;
    const auto first = builder.interleaved(vertices, 8, {{"VEC3", 0}, {"VEC3", 3}, {"VEC2", 6}});
    const auto image = builder.png(size, size, 3, pixels);
    return builder.glb(R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],"textures":[{"source":)" +
                       std::to_string(image) + R"(,"sampler":0}],"samplers":[{"magFilter":)" + std::to_string(mag) +
                       R"(,"minFilter":)" + std::to_string(min) +
                       R"(}],"materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0},)"
                       R"("metallicFactor":0,"roughnessFactor":1}}],"meshes":[{"primitives":[{"attributes":{)"
                       R"("POSITION":)" +
                       std::to_string(first) + R"(,"NORMAL":)" + std::to_string(first + 1) + R"(,"TEXCOORD_0":)" +
                       std::to_string(first + 2) + R"(},"material":0}]}])");
}
// The row of @p image that shows the floor @p distance ahead of an eye @p height above it that looks at the floor
// @p aim ahead, through a 45-degree vertical field of view.
inline std::size_t floor_row(const gpu_check::Image &image, double height, double aim, double distance) {
    const double cotangent = 1 + std::numbers::sqrt2; // 1 / tan(22.5 degrees)
    const auto below = std::atan(height / distance) - std::atan(height / aim);
    return std::size_t((1 + cotangent * std::tan(below)) * image.height / 2);
}
// The mean absolute deviation of green from each row's mean, over rows @p first to @p last and the middle half of
// the columns: how much of the stripes' contrast those rows keep.
inline double stripe_contrast(const gpu_check::Image &image, std::size_t first, std::size_t last) {
    double deviation = 0;
    std::size_t count = 0;
    for (auto y = first; y <= last; ++y) {
        double mean = 0;
        for (auto x = image.width / 4; x < image.width * 3 / 4; ++x)
            mean += gpu_check::pixel(image, x, y)[1];
        mean /= double(image.width * 3 / 4 - image.width / 4);
        for (auto x = image.width / 4; x < image.width * 3 / 4; ++x, ++count)
            deviation += std::abs(gpu_check::pixel(image, x, y)[1] - mean);
    }
    return deviation / double(count);
}
// What RendererOptions::max_anisotropy and VulkanRenderer::set_max_anisotropy() report for a degree that is not finite
// or is below 1.
constexpr std::string_view invalid_anisotropy = "Maximum anisotropy must be finite and at least 1";
// Draws the striped floor from 1 m above it with the default RendererOptions::max_anisotropy, with 64, above the 16
// that devices commonly allow, and with 1, and returns the degree that the first renderer used. Between 12 and 30 m
// ahead, a pixel's footprint on the floor is 12 to 30 times longer than it is wide, so isotropic filtering blurs the
// stripes to gray, and anisotropic filtering, which Vulkan requires to reach 16 wherever it is supported, keeps more of
// them. Nearest and unmipmapped floors must not change. The first renderer then lowers its anisotropy to 1 with
// set_max_anisotropy() and must draw the floors that it has cached as the renderer created with 1 draws them, and raise
// it again and draw its first frame again, with as many samplers throughout.
inline float check_anisotropy(const std::filesystem::path &output) {
    constexpr double height = 1, aim = 20, farthest = 30, closest = 12, least_gain = 2;
    const Harness::Eye eye{{0, float(height), float(aim)}, {0, 0, 0}};
    const std::array<std::pair<std::string, std::vector<std::byte>>, 3> floors{
        {{"floor-trilinear", striped_floor(linear, linear_mipmap_linear)},
         {"floor-nearest", striped_floor(nearest, nearest)},
         {"floor-unmipmapped", striped_floor(linear, linear)}}};
    std::vector<std::pair<std::string, gpu_check::Image>> anisotropic, lowered;
    float degree{};
    {
        Harness harness(output);
        degree = harness.max_anisotropy();
        // Each floor keeps its scene, so its mesh stays cached and the changes below replace its samplers rather than
        // upload it again.
        std::vector<std::shared_ptr<const anima::Scene>> floor_scenes;
        for (const auto &[name, glb] : floors) {
            floor_scenes.push_back(imported_scene(glb));
            harness.render(name, floor_scenes.back(), eye);
            anisotropic.emplace_back(name + "-anisotropic", harness.images[name]);
        }
        const auto samplers = harness.resources.resident_material_samplers;
        auto &renderer = harness.renderer();
        constexpr auto infinity = std::numeric_limits<float>::infinity();
        for (const auto value : {0.F, .5F, -infinity, infinity, std::numeric_limits<float>::quiet_NaN()})
            rejection::rejects<std::invalid_argument>([&] { renderer.set_max_anisotropy(value); }, invalid_anisotropy);
        require(harness.max_anisotropy() == degree, "A rejected set_max_anisotropy() changed the anisotropy");
        // Each change is applied by a draw() while the frame before it is still in flight, so validation checks that
        // the descriptors it rewrites wait for that frame.
        const auto change = [&](float samples, std::size_t index, const std::string &name) {
            harness.select(floor_scenes[index], eye);
            harness.present();
            renderer.set_max_anisotropy(samples);
            harness.capture(name);
            require(harness.resources.resident_material_samplers == samplers,
                    "set_max_anisotropy(" + std::to_string(samples) + ") left " +
                        std::to_string(harness.resources.resident_material_samplers) + " material samplers, not " +
                        std::to_string(samplers));
        };
        change(1, 0, "floor-trilinear-lowered");
        require(harness.max_anisotropy() == 1, "set_max_anisotropy(1) did not filter isotropically");
        for (std::size_t index = 1; index < floors.size(); ++index) {
            harness.render(floors[index].first + "-lowered", floor_scenes[index], eye);
            require(harness.resources.resident_material_samplers == samplers,
                    "A cached floor drawn at anisotropy 1 changed the material samplers");
        }
        for (const auto &[name, glb] : floors)
            lowered.emplace_back(name + "-lowered", harness.images[name + "-lowered"]);
        change(anima::RendererOptions{}.max_anisotropy, 0, "floor-trilinear-raised");
        require(harness.max_anisotropy() == degree, "Raising the anisotropy again gave " +
                                                        std::to_string(harness.max_anisotropy()) + ", not " +
                                                        std::to_string(degree));
        harness.images.require_same("floor-trilinear", "floor-trilinear-raised",
                                    "Raising the anisotropy again filtered unlike the renderer's first frame");
        harness.finish();
    }
    require(degree == 1 || degree == anima::RendererOptions{}.max_anisotropy,
            "The default anisotropy is " + std::to_string(degree) + ", not 1 or the default option");
    // A request above the device's maxSamplerAnisotropy gets that limit, which Vulkan requires to be at least 16 where
    // the device filters anisotropically, or 1 on a device that does not. Validation, which rejects a sampler beyond
    // the limit, checks that the draw uses it, and where the limit is the default option's 16 the floor is the
    // default's.
    {
        constexpr float above = 64;
        Harness harness(output, above);
        const auto limit = harness.max_anisotropy();
        std::cout << "Anisotropy " << limit << " for a request of " << above << '\n';
        require(degree == 1 ? limit == 1 : limit >= degree && limit <= above,
                "A request of " + std::to_string(above) + " gave anisotropy " + std::to_string(limit) +
                    ", not the device's limit, with the default option giving " + std::to_string(degree));
        harness.render("floor-trilinear-above-limit", floors[0].second, anima::identity(), eye);
        if (limit == degree) {
            harness.images.add(anisotropic[0].first, anisotropic[0].second);
            harness.images.require_same(anisotropic[0].first, "floor-trilinear-above-limit",
                                        "A request above the device's limit filtered unlike the limit itself");
        }
        harness.finish();
    }
    Harness harness(output, 1);
    require(harness.max_anisotropy() == 1, "RendererOptions::max_anisotropy 1 did not filter isotropically");
    auto &images = harness.images;
    for (auto &[name, image] : anisotropic)
        images.add(name, std::move(image));
    for (auto &[name, image] : lowered)
        images.add(name, std::move(image));
    for (const auto &[name, glb] : floors) {
        harness.render(name + "-isotropic", glb, anima::identity(), eye);
        images.require_same(name + "-lowered", name + "-isotropic",
                            "set_max_anisotropy(1) filtered a cached floor unlike a renderer created with 1");
    }
    images.require_same("floor-nearest-anisotropic", "floor-nearest-isotropic",
                        "Anisotropy changed a nearest-filtered texture");
    images.require_same("floor-unmipmapped-anisotropic", "floor-unmipmapped-isotropic",
                        "Anisotropy changed an unmipmapped texture");
    if (degree == 1)
        images.require_same("floor-trilinear-anisotropic", "floor-trilinear-isotropic",
                            "A device without anisotropic filtering changed a trilinear texture");
    else {
        const auto &sharp = images["floor-trilinear-anisotropic"];
        const auto first = floor_row(sharp, height, aim, farthest), last = floor_row(sharp, height, aim, closest);
        const auto kept = stripe_contrast(sharp, first, last),
                   isotropic = stripe_contrast(images["floor-trilinear-isotropic"], first, last);
        std::cout << "Stripe contrast in rows " << first << " to " << last << ": " << kept << " anisotropic, "
                  << isotropic << " isotropic\n";
        images.require(kept > least_gain * isotropic,
                       "Anisotropic filtering kept stripe contrast " + std::to_string(kept) + ", not more than " +
                           std::to_string(least_gain) + " times the isotropic " + std::to_string(isotropic),
                       {"floor-trilinear-anisotropic", "floor-trilinear-isotropic"});
    }
    harness.finish();
    return degree;
}
// Rejects a RendererOptions::max_anisotropy that is not finite or is below 1, after an unknown failure stage and
// before a missing window, so without a display or GPU.
inline void reject_invalid_anisotropy() {
    const auto construct = [](float anisotropy, anima::RendererFailureStage stage) {
        anima::RendererOptions options;
        options.max_anisotropy = anisotropy;
        options.fail_after = stage;
        anima::VulkanRenderer renderer(nullptr, options);
    };
    constexpr auto no_failure = anima::RendererFailureStage::none;
    constexpr auto infinity = std::numeric_limits<float>::infinity();
    for (const auto value : {0.F, .5F, -1.F, -infinity, infinity, std::numeric_limits<float>::quiet_NaN()})
        rejection::rejects<std::invalid_argument>([&] { construct(value, no_failure); }, invalid_anisotropy);
    for (const auto value : {1.F, 16.F, 1000.F})
        rejection::rejects<std::invalid_argument>([&] { construct(value, no_failure); },
                                                  "Renderer requires an SDL window");
    // The failure stage is checked first.
    rejection::rejects<std::invalid_argument>([&] { construct(0, anima::RendererFailureStage::vertex); },
                                              "Unknown initialization failure stage");
}
inline int run_sampling(int argc, char **argv) {
    require(argc == 3, "Usage: consumer --material-sampling OUTPUT");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    check_filters(argv[2]);
    const auto degree = check_anisotropy(argv[2]);
    std::cout << "PASS material sampling: per-pixel sRGB, nearest and linear filters, wrapping, mip minification, "
                 "independent minification and magnification filters, and anisotropy "
              << degree << " that sharpens only linear, mipmapped textures\n";
    return 0;
}

struct Shape {
    std::vector<float> vertices; // Position and normal, six floats per vertex.
    double metallic, roughness;
    std::array<double, 3> color;
};
inline std::vector<std::byte> shapes_glb(const std::vector<Shape> &shapes) {
    gltf_fixture::Builder builder;
    std::string primitives, materials;
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        const auto &shape = shapes[i];
        const auto first = builder.interleaved(shape.vertices, 6, {{"VEC3", 0}, {"VEC3", 3}});
        const auto separator = std::string(i ? "," : "");
        primitives += separator + R"({"attributes":{"POSITION":)" + std::to_string(first) + R"(,"NORMAL":)" +
                      std::to_string(first + 1) + R"(},"material":)" + std::to_string(i) + '}';
        materials += separator + R"({"name":"metal=)" + gltf_fixture::number(shape.metallic) +
                     " rough=" + gltf_fixture::number(shape.roughness) +
                     R"(","pbrMetallicRoughness":{"baseColorFactor":[)" + gltf_fixture::number(shape.color[0]) + ',' +
                     gltf_fixture::number(shape.color[1]) + ',' + gltf_fixture::number(shape.color[2]) +
                     R"(,1],"metallicFactor":)" + gltf_fixture::number(shape.metallic) + R"(,"roughnessFactor":)" +
                     gltf_fixture::number(shape.roughness) + "}}";
    }
    return builder.glb(R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],"meshes":[{"primitives":[)" +
                       primitives + R"(]}],"materials":[)" + materials + ']');
}
// A unit sphere of 40 rings and 64 segments with outward winding and no degenerate pole triangles.
inline std::vector<float> sphere(std::array<float, 3> center) {
    constexpr int rings = 40, segments = 64;
    const auto vertex = [&](int i, int j) {
        const auto theta = std::numbers::pi * i / rings, phi = 2 * std::numbers::pi * j / segments;
        const std::array<float, 3> n{float(std::sin(theta) * std::cos(phi)), float(std::cos(theta)),
                                     float(std::sin(theta) * std::sin(phi))};
        return std::array<float, 6>{center[0] + n[0], center[1] + n[1], center[2] + n[2], n[0], n[1], n[2]};
    };
    std::vector<float> result;
    const auto triangle = [&](std::array<float, 6> a, std::array<float, 6> b, std::array<float, 6> c) {
        for (const auto &corner : {a, b, c})
            result.insert(result.end(), corner.begin(), corner.end());
    };
    for (int i = 0; i < rings; ++i)
        for (int j = 0; j < segments; ++j) {
            if (i > 0)
                triangle(vertex(i, j), vertex(i, j + 1), vertex(i + 1, j));
            if (i < rings - 1)
                triangle(vertex(i, j + 1), vertex(i + 1, j + 1), vertex(i + 1, j));
        }
    return result;
}
inline int run_pbr(int argc, char **argv) {
    require(argc == 3, "Usage: consumer --pbr OUTPUT");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    Harness harness(argv[2]);
    auto &images = harness.images;
    // The quad's shading normal lies halfway between the default light and the camera, which shows the highlight;
    // changing only roughness must change it.
    const auto light = default_light(), view = default_view();
    const auto normal = unit({light[0] + view[0], light[1] + view[1], light[2] + view[2]});
    std::vector<float> quad;
    for (const auto &[x, y] : quad_corners)
        quad.insert(quad.end(), {x, y, 0, float(normal[0]), float(normal[1]), float(normal[2])});
    constexpr std::array<double, 3> black{0, 0, 0}, copper{.7, .25, .06};
    std::map<std::string, gpu_check::Rgb> centers;
    for (const auto &[name, metallic, roughness, color] :
         {std::tuple{"black-rough", 0., 1., black}, std::tuple{"black-smooth", 0., .25, black},
          std::tuple{"copper-dielectric", 0., 1., copper}, std::tuple{"copper-metal", 1., 1., copper},
          std::tuple{"zero-roughness", 1., 0., copper}}) {
        harness.render(name, shapes_glb({{quad, metallic, roughness, color}}));
        centers[name] = sample(images[name], 1, 1);
    }
    const auto &rough = centers["black-rough"], &smooth = centers["black-smooth"];
    const auto &dielectric = centers["copper-dielectric"], &metal = centers["copper-metal"];
    constexpr int highlight_gain = 60, neutral_spread = 1, lost_diffuse = 20;
    images.require(std::min({smooth[0], smooth[1], smooth[2]}) >
                       std::max({rough[0], rough[1], rough[2]}) + highlight_gain,
                   "Lowering the roughness did not raise the highlight: " + gpu_check::text(smooth) + " against " +
                       gpu_check::text(rough),
                   {"black-smooth", "black-rough"});
    images.require(std::min({rough[0], rough[1], rough[2]}) > 0,
                   "A black dielectric lost its specular reflection: " + gpu_check::text(rough), {"black-rough"});
    images.require(std::max({smooth[0], smooth[1], smooth[2]}) - std::min({smooth[0], smooth[1], smooth[2]}) <=
                       neutral_spread,
                   "The dielectric highlight is tinted: " + gpu_check::text(smooth), {"black-smooth"});
    images.require(dielectric[0] > metal[0] + lost_diffuse,
                   "The metal kept its diffuse reflection: " + gpu_check::text(metal) + " against " +
                       gpu_check::text(dielectric),
                   {"copper-dielectric", "copper-metal"});
    images.require(metal[0] > metal[1] && metal[1] > metal[2],
                   "The metal reflection is not tinted by its color: " + gpu_check::text(metal), {"copper-metal"});
    // A study of dielectric (top) and metal (bottom) spheres at roughness 0.2, 0.5 and 0.9 renders with clean
    // validation; no pixel of it is compared.
    std::vector<Shape> study;
    for (const auto &[metallic, y] : {std::pair{0., 1.3F}, std::pair{1., -1.3F}})
        for (const auto &[x, roughness] : {std::pair{-2.5F, .2}, std::pair{0.F, .5}, std::pair{2.5F, .9}})
            study.push_back({sphere({x, y, 0}), metallic, roughness, copper});
    harness.render("material-study", shapes_glb(study));
    harness.finish();
    std::cout << "PASS PBR: roughness response, neutral dielectric specular, tinted metal, zero roughness and a "
                 "material study\n";
    return 0;
}

// A quad with one 1x1 RGBA texel, as gpu_surface_smoke.py generated it. The reference variant bakes the texel
// into constant factors or the vertex normal instead of mapping it; @p authored adds a TANGENT attribute, and
// without it the renderer derives the tangent frame from screen derivatives.
inline std::vector<std::byte> surface_quad(std::string_view kind, bool reference, bool authored) {
    std::array<std::uint8_t, 4> texel{};
    if (kind == "normal" || kind == "shared")
        texel = {218, 128, 218, 255};
    else if (kind == "surface")
        texel = {40, 96, 192, 255};
    else
        texel = {128, 180, 80, 255};
    std::array<double, 3> normal{0, 0, 1};
    if (reference && (kind == "normal" || kind == "shared"))
        normal = unit({texel[0] / 255. * 2 - 1, texel[1] / 255. * 2 - 1, texel[2] / 255. * 2 - 1});
    std::array<double, 3> base{.4, .5, .3}, emissive{.35, .25, .45};
    double metallic = .2, roughness = .7;
    std::string maps;
    if ((kind == "normal" || kind == "shared") && !reference)
        maps += R"(,"normalTexture":{"index":0})";
    std::string pbr_maps;
    if (kind == "surface") {
        if (reference) {
            metallic *= texel[2] / 255.;
            roughness *= texel[1] / 255.;
        } else
            pbr_maps += R"(,"metallicRoughnessTexture":{"index":0})";
    }
    if (kind == "emissive") {
        if (reference)
            for (std::size_t c = 0; c < 3; ++c)
                emissive[c] *= srgb_to_linear(texel[c]);
        else
            maps += R"(,"emissiveTexture":{"index":0})";
        maps += R"(,"emissiveFactor":[)" + gltf_fixture::number(emissive[0]) + ',' + gltf_fixture::number(emissive[1]) +
                ',' + gltf_fixture::number(emissive[2]) + ']';
    }
    if (kind == "shared" || kind == "unlit") {
        if (reference)
            for (std::size_t c = 0; c < 3; ++c)
                base[c] *= srgb_to_linear(texel[c]);
        else
            pbr_maps += R"(,"baseColorTexture":{"index":0})";
    }
    if (kind == "unlit")
        maps += R"(,"extensions":{"KHR_materials_unlit":{}})";
    std::vector<float> vertices;
    for (const auto &[x, y] : quad_corners)
        vertices.insert(vertices.end(), {x, y, 0, float(normal[0]), float(normal[1]), float(normal[2]), (x + 1) / 2,
                                         (y + 1) / 2, 1, 0, 0, 1});
    gltf_fixture::Builder builder;
    const auto first = builder.interleaved(vertices, 12, {{"VEC3", 0}, {"VEC3", 3}, {"VEC2", 6}, {"VEC4", 8}});
    const auto image = builder.png(1, 1, 4, {texel.begin(), texel.end()});
    std::string attributes = R"("POSITION":)" + std::to_string(first) + R"(,"NORMAL":)" + std::to_string(first + 1) +
                             R"(,"TEXCOORD_0":)" + std::to_string(first + 2);
    if (authored)
        attributes += R"(,"TANGENT":)" + std::to_string(first + 3);
    std::string members = R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],"textures":[{"source":)" +
                          std::to_string(image) + R"(}],"materials":[{"pbrMetallicRoughness":{"baseColorFactor":[)" +
                          gltf_fixture::number(base[0]) + ',' + gltf_fixture::number(base[1]) + ',' +
                          gltf_fixture::number(base[2]) + R"(,1],"metallicFactor":)" + gltf_fixture::number(metallic) +
                          R"(,"roughnessFactor":)" + gltf_fixture::number(roughness) + pbr_maps + '}' + maps +
                          R"(}],"meshes":[{"primitives":[{"attributes":{)" + attributes + R"(},"material":0}]}])";
    if (kind == "unlit")
        members += R"(,"extensionsUsed":["KHR_materials_unlit"],"extensionsRequired":["KHR_materials_unlit"])";
    return builder.glb(members);
}
inline int run_surface_maps(int argc, char **argv) {
    require(argc == 3, "Usage: consumer --surface-maps OUTPUT");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    Harness harness(argv[2]);
    auto &images = harness.images;
    // Each map must render as its constant or normal equivalent: authored and derivative normal frames, linear
    // green and blue surface maps, sRGB emission, one texture read both as sRGB color and as a linear normal map,
    // and the required unlit extension.
    constexpr double largest_mean = .15;
    constexpr int largest_center_error = 2;
    for (const auto *kind : {"normal", "surface", "emissive", "shared", "unlit"}) {
        const std::string prefix = kind;
        harness.render(prefix + "-reference", surface_quad(kind, true, true));
        harness.render(prefix + "-mapped", surface_quad(kind, false, true));
        harness.render(prefix + "-derivatives", surface_quad(kind, false, false));
        const auto &reference = images[prefix + "-reference"];
        for (const auto *variant : {"-mapped", "-derivatives"}) {
            const auto name = prefix + variant;
            const auto &actual = images[name];
            const auto parity = gpu_check::parity(reference, actual);
            images.require(parity.mean < largest_mean,
                           name + " differs from its reference by a mean of " + std::to_string(parity.mean) + " levels",
                           {prefix + "-reference", name});
            const auto expected = sample(reference, 1, 1), center = sample(actual, 1, 1);
            images.require(gpu_check::difference(expected, center) <= largest_center_error,
                           name + " shows " + gpu_check::text(center) + " at its center instead of " +
                               gpu_check::text(expected),
                           {prefix + "-reference", name});
        }
        images.discard({prefix + "-reference", prefix + "-mapped", prefix + "-derivatives"});
    }
    harness.finish();
    std::cout << "PASS surface maps: authored and derivative normal frames, linear surface maps, sRGB emission, "
                 "mixed texture encodings and the required unlit extension\n";
    return 0;
}

// An untextured quad from -1 to 1 in X and Y. @p back reverses each triangle's corners and points the normal to -Z,
// so the camera, on the +Z side, sees the quad's back; @p hidden masks every fragment out, leaving the background.
inline std::vector<std::byte> sided_quad(bool back, bool double_sided, bool hidden = false) {
    std::vector<float> vertices;
    for (std::size_t i = 0; i < quad_corners.size(); ++i) {
        const auto &[x, y] = quad_corners[back ? i / 3 * 3 + 2 - i % 3 : i];
        vertices.insert(vertices.end(), {x, y, 0, 0, 0, back ? -1.F : 1.F});
    }
    gltf_fixture::Builder builder;
    const auto first = builder.interleaved(vertices, 6, {{"VEC3", 0}, {"VEC3", 3}});
    return builder.glb(R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],"materials":[{)"
                       R"("pbrMetallicRoughness":{"baseColorFactor":[0.8,0.6,0.4,)" +
                       std::string(hidden ? "0" : "1") + R"(],"metallicFactor":0,"roughnessFactor":0.6})" +
                       (hidden ? R"(,"alphaMode":"MASK")" : "") + R"(,"doubleSided":)" +
                       (double_sided ? "true" : "false") + R"(}],"meshes":[{"primitives":[{"attributes":{"POSITION":)" +
                       std::to_string(first) + R"(,"NORMAL":)" + std::to_string(first + 1) + R"(},"material":0}]}])");
}
// sided_quad() as an Asset, narrowed onto its left half, from -1 to 0 in X, which keeps its winding.
inline anima::Asset sided_half(bool back) {
    auto asset = *anima::load_asset(std::span<const std::byte>(sided_quad(back, false)));
    for (auto &vertex : asset.primitives.at(0).vertices)
        vertex.position.x = (vertex.position.x - 1) / 2;
    return asset;
}
// sided_quad() whose node's rest matrix mirrors X, which reverses the winding of the copies that place it, as a
// mirrored world matrix or placement does.
inline anima::Asset mirrored_node_quad(bool back) {
    auto asset = *anima::load_asset(std::span<const std::byte>(sided_quad(back, false)));
    asset.nodes.at(0).rest.scale = {-1, 1, 1};
    return asset;
}
// sided_half() twice in one skinned primitive, bound whole to a joint at rest and to one that mirrors X, which carries
// the second half onto the right with its winding reversed, so that together they cover sided_quad().
inline anima::Asset skinned_halves(bool back) {
    auto asset = sided_half(back);
    auto &vertices = asset.primitives.at(0).vertices;
    const auto half = vertices;
    vertices.insert(vertices.end(), half.begin(), half.end());
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        vertices[i].joints = {i < half.size() ? 0U : 1U, 0, 0, 0};
        vertices[i].weights = {1, 0, 0, 0};
    }
    const auto joint = asset.nodes.size();
    asset.nodes.resize(joint + 2);
    asset.nodes[joint + 1].rest.scale = {-1, 1, 1};
    asset.skins.push_back({{joint, joint + 1}, {anima::identity(), anima::identity()}});
    asset.primitives.at(0).skin = 0;
    return asset;
}
inline int run_sidedness(int argc, char **argv) {
    require(argc == 3, "Usage: consumer --sidedness OUTPUT");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    Harness harness(argv[2]);
    auto &images = harness.images;
    // Renders a fixture as @p name, requiring one draw call, @p discarding of them through the pipeline whose fragment
    // shader may discard. The others cull single-sided faces in the rasterizer instead.
    const auto render = [&](const std::string &name, std::uint64_t discarding, const auto &...fixture) {
        harness.render(name, fixture...);
        const auto &drawn = harness.resources;
        images.require(drawn.draw_calls == 1 && drawn.discarding_draw_calls == discarding,
                       name + " recorded " + std::to_string(drawn.draw_calls) + " draw calls, " +
                           std::to_string(drawn.discarding_draw_calls) +
                           " through the pipeline that may discard, instead of 1 and " + std::to_string(discarding),
                       {name});
    };
    // A scene of one object that draws @p mesh at @p world, at @p placements unless they are empty, within @p range.
    const auto object = [](const std::shared_ptr<const anima::Mesh> &mesh, const std::vector<anima::Mat4> &placements,
                           const anima::VisibilityRange &range, const anima::Mat4 &world = anima::identity()) {
        auto scene = std::make_shared<anima::Scene>();
        auto created = scene->create({}, mesh);
        created.set_world_matrix(world);
        auto renderer = created.renderer();
        if (!placements.empty()) {
            const auto copies = anima::MeshPlacements::create(mesh, placements);
            require(copies->clusters().size() == 1, "The sidedness placements do not form one cluster");
            renderer.set_placements(copies);
        }
        renderer.set_visibility_range(range);
        return std::shared_ptr<const anima::Scene>(std::move(scene));
    };
    // Mirroring X winds the quad's outward faces clockwise; its normal still points to +Z.
    auto mirrored = anima::identity();
    mirrored[0] = -1;
    // A masked material discards; the rest cull a single-sided back face in the rasterizer, or under a mirrored
    // transform a front face, as the shader would discard it.
    render("empty", 1, sided_quad(false, false, true));
    render("front", 0, sided_quad(false, false));
    render("back", 0, sided_quad(true, false));
    render("double-sided-back", 0, sided_quad(true, true));
    render("mirrored-front", 0, sided_quad(false, false), mirrored);
    render("mirrored-back", 0, sided_quad(true, false), mirrored);
    // A cluster whose placements all mirror culls front faces, and one whose placements differ in orientation, here
    // half of the quad in place and half mirrored onto the other half, discards in the shader.
    const auto quad = anima::Mesh::compile(*anima::load_asset(std::span<const std::byte>(sided_quad(false, false))));
    const std::vector<anima::Mat4> mixed{anima::identity(), mirrored};
    render("placed-mirrored-front", 0, object(quad, {mirrored}, {}));
    render("placed-mixed-front", 1, object(anima::Mesh::compile(sided_half(false)), mixed, {}));
    render("placed-mixed-back", 1, object(anima::Mesh::compile(sided_half(true)), mixed, {}));
    // A copy's orientation composes the object's world matrix and its node's rest matrix with its placement's, so a
    // mirrored world or node culls front faces, and a mirrored world undone by a mirrored placement culls back faces.
    const auto back_quad =
        anima::Mesh::compile(*anima::load_asset(std::span<const std::byte>(sided_quad(true, false))));
    render("placed-world-mirrored-front", 0, object(quad, {anima::identity()}, {}, mirrored));
    render("placed-world-mirrored-back", 0, object(back_quad, {anima::identity()}, {}, mirrored));
    render("placed-world-and-placement-mirrored-front", 0, object(quad, {mirrored}, {}, mirrored));
    render("placed-world-and-placement-mirrored-back", 0, object(back_quad, {mirrored}, {}, mirrored));
    render("placed-node-mirrored-front", 0,
           object(anima::Mesh::compile(mirrored_node_quad(false)), {anima::identity()}, {}));
    render("placed-node-mirrored-back", 0,
           object(anima::Mesh::compile(mirrored_node_quad(true)), {anima::identity()}, {}));
    // Each skinned triangle's first vertex orients it, here one half in place and one mirrored, so a single-sided
    // skinned draw discards in the shader.
    render("skinned-front", 1, object(anima::Mesh::compile(skinned_halves(false)), {}, {}));
    render("skinned-back", 1, object(anima::Mesh::compile(skinned_halves(true)), {}, {}));
    // The orbit camera lies 3 sqrt(2), about 4.24, from the quad's center: inside the end margin of a range that ends
    // at 6 after a 4 wide margin, where the object, or a copy placed at it, dissolves, and outside the margin of one
    // that ends at 100.
    constexpr anima::VisibilityRange fading{0, 6, 0, 4}, whole{0, 100, 0, 4};
    render("ranged-front", 1, object(quad, {}, fading));
    render("placed-ranged-front", 1, object(quad, {anima::identity()}, fading));
    render("ranged-whole-front", 0, object(quad, {}, whole));
    constexpr int least_surface_difference = 32;
    const auto background = sample(images["empty"], 1, 1), surface = sample(images["front"], 1, 1);
    images.require(gpu_check::difference(background, surface) >= least_surface_difference,
                   "The front of a single-sided quad shows " + gpu_check::text(surface) +
                       " at its center, too close to the background " + gpu_check::text(background),
                   {"empty", "front"});
    // A single-sided back draws nothing, a double-sided one is lit as its front, and mirrored transforms, placements
    // and nodes keep their outward sides, as does a skinned draw whose joints differ in orientation.
    images.require_parity("empty", "back");
    images.require_parity("front", "double-sided-back");
    images.require_parity("front", "mirrored-front");
    images.require_parity("empty", "mirrored-back");
    for (const auto *name : {"placed-mirrored-front", "placed-mixed-front", "placed-world-mirrored-front",
                             "placed-world-and-placement-mirrored-front", "placed-node-mirrored-front", "skinned-front",
                             "ranged-whole-front"})
        images.require_parity("front", name);
    for (const auto *name : {"placed-mixed-back", "placed-world-mirrored-back",
                             "placed-world-and-placement-mirrored-back", "placed-node-mirrored-back", "skinned-back"})
        images.require_parity("empty", name);
    // In the margin the quad dissolves, the same pixels as an object and as a placed copy.
    constexpr double least_dissolved = .01;
    images.require_changed("front", "ranged-front", least_dissolved, "The quad in its range's margin kept every pixel");
    images.require_changed("empty", "ranged-front", least_dissolved, "The quad in its range's margin drew nothing");
    images.require_parity("ranged-front", "placed-ranged-front");
    harness.finish();
    std::cout << "PASS sidedness: single-sided backs culled, double-sided backs lit as their fronts, mirrored "
                 "transforms, placements, nodes and joints keeping their outward sides, through pipelines that "
                 "discard only where the rasterizer cannot cull\n";
    return 0;
}
} // namespace material_test
