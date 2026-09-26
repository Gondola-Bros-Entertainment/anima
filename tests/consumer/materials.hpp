#pragma once
// Material checks: texture sampling (sRGB decoding, filters, wrap modes, minification), metallic-roughness
// shading and glTF surface maps. Each fixture is a GLB built in memory and imported with anima::load_asset, then
// rendered as the viewer shows a static asset: framed by an OrbitCamera, lit by the default Environment.
#include "gltf_fixture.hpp"
#include "gpu_checks.hpp"
#include <anima/assets/asset.hpp>
#include <anima/scene.hpp>
#include <cmath>
#include <numbers>

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

// Renders fixtures one at a time in one window and keeps each first frame by name.
class Harness {
  public:
    explicit Harness(const std::filesystem::path &output)
        : window_(gpu_check::window("Anima material verification", 960, 640, SDL_WINDOW_HIGH_PIXEL_DENSITY)),
          renderer_(window_.get(), options()), images(output) {
        renderer_.set_environment(anima::Environment{});
    }
    /// Imports @p glb, frames it and reads back its first frame as @p name.
    void render(const std::string &name, const std::vector<std::byte> &glb) {
        const auto asset = anima::load_asset(std::span<const std::byte>(glb));
        auto scene = std::make_shared<anima::Scene>();
        (void)scene->add(anima::Mesh::compile(*asset));
        renderer_.set_scenes({scene});
        anima::OrbitCamera camera;
        camera.frame(scene->bounds().minimum, scene->bounds().maximum);
        int width = 0, height = 0;
        require(SDL_GetWindowSizeInPixels(window_.get(), &width, &height) && width > 0 && height > 0,
                "Material window has no drawable size");
        renderer_.set_view(camera.matrix(float(width) / float(height)));
        renderer_.request_capture();
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
        images.add(name, gpu_check::take(renderer_));
    }
    /// Shuts the renderer down and requires clean validation.
    void finish() {
        const auto stats = renderer_.shutdown();
        require(!stats.validation_errors && !stats.validation_warnings, "Material GPU validation failed");
    }

  private:
    static anima::RendererOptions options() {
        anima::RendererOptions settings;
        settings.validation = true;
        return settings;
    }
    gpu_check::Video video_;
    gpu_check::Window window_;
    anima::VulkanRenderer renderer_;

  public:
    gpu_check::Captures images;
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
inline int run_sampling(int argc, char **argv) {
    require(argc == 3, "Usage: consumer --material-sampling OUTPUT");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    Harness harness(argv[2]);
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
    std::cout << "PASS material sampling: per-pixel sRGB, nearest and linear filters, wrapping, mip minification "
                 "and independent minification and magnification filters\n";
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
} // namespace material_test
