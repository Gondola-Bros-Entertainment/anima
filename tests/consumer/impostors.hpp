#pragma once
#include "gpu_checks.hpp"
#include "resources.hpp"
#include <anima/impostor.hpp>
#include <anima/mesh_placements.hpp>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Impostors: a tree of masked leaf cards around an opaque trunk, baked with bake_impostor() and drawn as one quad per
// copy. From several directions at the distance where they hand over, the impostor matches the tree within a stated
// parity, in the view and in the shadow it casts; across the handover the two keep complementary pixels, so none is
// left uncovered; and 10,000 placed trees are measured as meshes and as impostors.
namespace impostor_test {
// A small deterministic generator, so that every run builds the same tree.
class Random {
  public:
    float next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return float(state_ >> 8) / float(1U << 24);
    }

  private:
    std::uint32_t state_ = 2463534242U;
};
// A leaf of 32 by 32 texels: green with some variation, opaque within an ellipse.
inline std::shared_ptr<const anima::Image> leaf_image() {
    auto image = std::make_shared<anima::Image>();
    image->width = image->height = 32;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const float u = (float(x) + .5F) / 16 - 1, v = (float(y) + .5F) / 16 - 1;
            const bool inside = u * u / .55F + v * v < 1;
            const auto shade = std::uint8_t(150 + (x * 7 + y * 13) % 40);
            image->rgba.insert(image->rgba.end(), {std::uint8_t(shade / 3), shade, std::uint8_t(shade / 4),
                                                   std::uint8_t(inside ? 255 : 0)});
        }
    return image;
}
// A tree about 9 metres tall: a trunk 5 metres high and 0.35 thick, and a crown of 500 double-sided masked cards.
inline anima::Asset tree_asset() {
    anima::Asset asset;
    asset.nodes.resize(1);
    anima::Material bark;
    bark.name = "bark";
    bark.factor = {.45F, .36F, .28F};
    bark.roughness = .9F;
    bark.double_sided = false;
    anima::Material leaves;
    leaves.name = "leaves";
    leaves.texture = 0;
    leaves.roughness = .7F;
    leaves.alpha_mode = anima::AlphaMode::mask;
    asset.materials = {bark, leaves};
    asset.textures.push_back({leaf_image(), {}, anima::TextureEncoding::srgb});
    anima::SourcePrimitive trunk;
    trunk.material = 0;
    constexpr int segments = 16;
    const auto at = [](int segment, float height) {
        const float phi = 2 * std::numbers::pi_v<float> * float(segment % segments) / segments;
        anima::SourceVertex vertex;
        vertex.normal = {std::cos(phi), 0, std::sin(phi)};
        vertex.position = {.35F * vertex.normal.x, height, .35F * vertex.normal.z};
        return vertex;
    };
    for (int segment = 0; segment < segments; ++segment) {
        const auto a = at(segment, 0), b = at(segment + 1, 0), c = at(segment + 1, 5.5F), d = at(segment, 5.5F);
        for (const auto &v : {a, c, b, a, d, c})
            trunk.vertices.push_back(v);
    }
    anima::SourcePrimitive crown;
    crown.material = 1;
    Random random;
    for (int card = 0; card < 500; ++card) {
        // A point in the crown's ellipsoid, and two perpendicular axes of a random plane through it.
        anima::Vec3 center;
        do
            center = {random.next() * 2 - 1, random.next() * 2 - 1, random.next() * 2 - 1};
        while (anima::dot(center, center) > 1);
        center = {center.x * 3.5F, 6.5F + center.y * 2.4F, center.z * 3.5F};
        const auto normal =
            anima::normalized({random.next() * 2 - 1, random.next() * 2 - 1 + .6F, random.next() * 2 - 1});
        const auto side = anima::normalized(anima::cross(normal, {.3F, 1, .2F}));
        const auto up = anima::cross(normal, side);
        const auto corner = [&](float s, float t) {
            anima::SourceVertex vertex;
            vertex.position = center + side * (s * .45F) + up * (t * .45F);
            vertex.normal = normal;
            vertex.uv = {(s + 1) / 2, (t + 1) / 2};
            return vertex;
        };
        const auto a = corner(-1, -1), b = corner(1, -1), c = corner(1, 1), d = corner(-1, 1);
        for (const auto &v : {a, b, c, a, c, d})
            crown.vertices.push_back(v);
    }
    asset.primitives = {std::move(trunk), std::move(crown)};
    return asset;
}
inline std::shared_ptr<const anima::Mesh> ground() {
    anima::Asset asset;
    asset.nodes.resize(1);
    asset.materials.push_back({"ground", {.5F, .5F, .46F}, -1});
    anima::SourcePrimitive primitive;
    primitive.material = 0;
    for (const auto corner : {anima::Vec3{-40, 0, -40}, anima::Vec3{-40, 0, 40}, anima::Vec3{40, 0, 40},
                              anima::Vec3{-40, 0, -40}, anima::Vec3{40, 0, 40}, anima::Vec3{40, 0, -40}}) {
        anima::SourceVertex vertex;
        vertex.position = corner;
        vertex.normal = {0, 1, 0};
        primitive.vertices.push_back(vertex);
    }
    asset.primitives.push_back(std::move(primitive));
    return anima::Mesh::compile(asset);
}
// How two captures of one subject over the same background differ: the share of the pixels that either covers which
// only one covers, and the mean channel difference over the pixels that both cover.
struct Comparison {
    double silhouette{}, color{};
};
inline Comparison compare(const gpu_check::Image &a, const gpu_check::Image &b) {
    const auto background = gpu_check::pixel(a, 0, 0);
    std::size_t either = 0, one = 0, both = 0;
    double difference = 0;
    for (std::size_t y = 0; y < a.height; ++y)
        for (std::size_t x = 0; x < a.width; ++x) {
            const auto pa = gpu_check::pixel(a, x, y), pb = gpu_check::pixel(b, x, y);
            const bool in_a = gpu_check::difference(pa, background) > 0,
                       in_b = gpu_check::difference(pb, background) > 0;
            either += in_a || in_b;
            one += in_a != in_b;
            if (in_a && in_b) {
                ++both;
                for (std::size_t c = 0; c < 3; ++c)
                    difference += std::abs(pa[c] - pb[c]);
            }
        }
    return {either ? double(one) / double(either) : 0, both ? difference / double(3 * both) : 0};
}

// The pixels of @p ground that @p mesh's shadow darkens by more than 12 levels in each channel on average, and the
// share of the pixels that either's shadow darkens which only one does.
inline std::pair<std::size_t, double> shadows(const gpu_check::Image &ground, const gpu_check::Image &mesh,
                                              const gpu_check::Image &copy) {
    std::size_t either = 0, one = 0, shadowed = 0;
    for (std::size_t y = 0; y < ground.height; ++y)
        for (std::size_t x = 0; x < ground.width; ++x) {
            const auto lit = gpu_check::pixel(ground, x, y);
            const auto dark = [&](const gpu_check::Image &image) {
                const auto p = gpu_check::pixel(image, x, y);
                return lit[0] + lit[1] + lit[2] - p[0] - p[1] - p[2] > 36;
            };
            const bool a = dark(mesh), b = dark(copy);
            either += a || b;
            one += a != b;
            shadowed += a;
        }
    return {shadowed, either ? double(one) / double(either) : 1};
}
// The pixels that both @p mesh and @p copy cover, and how many of them @p handover leaves as background.
inline std::pair<std::size_t, std::size_t> uncovered(const gpu_check::Image &mesh, const gpu_check::Image &copy,
                                                     const gpu_check::Image &handover) {
    const auto background = gpu_check::pixel(mesh, 0, 0);
    std::size_t covered = 0, holes = 0;
    for (std::size_t y = 0; y < mesh.height; ++y)
        for (std::size_t x = 0; x < mesh.width; ++x)
            if (gpu_check::pixel(mesh, x, y) != background && gpu_check::pixel(copy, x, y) != background) {
                ++covered;
                holes += gpu_check::pixel(handover, x, y) == background;
            }
    return {covered, holes};
}
// The stated parity of an impostor with its mesh at the handover distance: the share of the pixels that either covers
// which only one covers, the mean channel difference where both cover, and the share of the pixels that either's
// shadow darkens which only one does.
constexpr double silhouette_parity = .06, color_parity = 9, shadow_parity = .08;

inline int run(int argc, char **argv) {
    using resource_test::require;
    require(argc == 3, "Usage: consumer --impostors OUTPUT");
    static constexpr int window_size = 512;
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    gpu_check::Video video;
    const auto window = gpu_check::window("Anima impostor verification", window_size, window_size);
    anima::RendererOptions options;
    options.validation = true;
    options.profile = true;
    anima::VulkanRenderer renderer(window.get(), options);

    const auto tree = anima::Mesh::compile(tree_asset());
    const auto atlas = anima::bake_impostor(*tree);
    const auto impostor = anima::Mesh::compile_impostor(atlas);
    const auto &center = atlas.frames.center;

    gpu_check::Captures captures(argv[2]);
    const auto began = std::chrono::steady_clock::now();
    const auto draw = [&](const std::string &name) {
        renderer.request_capture();
        for (;;) {
            require(std::chrono::steady_clock::now() - began < gpu_check::watchdog, "Impostor watchdog");
            SDL_Event event{};
            while (SDL_PollEvent(&event))
                require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                        "Impostor check interrupted");
            if (renderer.draw())
                break;
            SDL_Delay(5);
        }
        captures.add(name, gpu_check::take(renderer));
        return renderer.resource_stats();
    };
    const auto view = [&](anima::Vec3 eye, anima::Vec3 target, float far = 300) {
        renderer.set_view(anima::operator*(anima::perspective(1, .1F, far), anima::look_at(eye, target)));
    };
    // Parity from the horizon round to above, at the distance where the tree is about 100 pixels tall, as tall as
    // the atlas's frames, in perspective and orthographic views and under a placement that turns the tree and scales
    // it unevenly.
    const auto compare_views = [&](const std::string &name, const std::shared_ptr<anima::Scene> &mesh,
                                   const std::shared_ptr<anima::Scene> &copy) {
        renderer.set_scenes({mesh});
        (void)draw(name + "-mesh");
        renderer.set_scenes({copy});
        (void)draw(name + "-impostor");
        const auto result = compare(captures[name + "-mesh"], captures[name + "-impostor"]);
        std::cout << "IMPOSTOR {\"view\":\"" << name << "\",\"silhouette\":" << result.silhouette
                  << ",\"color\":" << result.color << "}\n";
        captures.require(result.silhouette < silhouette_parity && result.color < color_parity,
                         name + ": the impostor's silhouette differs from the tree's in " +
                             std::to_string(result.silhouette) + " of their pixels and its color by " +
                             std::to_string(result.color) + " levels",
                         {name + "-mesh", name + "-impostor"});
        captures.discard({name + "-mesh", name + "-impostor"});
    };
    const auto alone = [](std::shared_ptr<const anima::Mesh> mesh, const anima::Mat4 &placement = anima::identity()) {
        auto scene = std::make_shared<anima::Scene>();
        scene->create("subject", mesh)
            .renderer()
            .set_placements(anima::MeshPlacements::create(mesh, std::span<const anima::Mat4>(&placement, 1)));
        return scene;
    };
    for (const auto &[azimuth, elevation] : std::array<std::pair<float, float>, 6>{
             {{0.F, 5.F}, {70.F, 10.F}, {145.F, 0.F}, {220.F, 20.F}, {290.F, 35.F}, {30.F, 70.F}}}) {
        const float a = azimuth * std::numbers::pi_v<float> / 180, e = elevation * std::numbers::pi_v<float> / 180;
        view(center + anima::Vec3{std::cos(a) * std::cos(e), std::sin(e), std::sin(a) * std::cos(e)} * 60, center);
        compare_views("view-" + std::to_string(int(azimuth)) + "-" + std::to_string(int(elevation)), alone(tree),
                      alone(impostor));
    }
    renderer.set_view(anima::operator*(anima::orthographic(1, 48, .1F, 300),
                                       anima::look_at(center + anima::Vec3{-50, 12, 30}, center)));
    compare_views("orthographic", alone(tree), alone(impostor));
    anima::Transform turned;
    turned.rotation = {0, std::sin(.32F), 0, std::cos(.32F)};
    turned.scale = {1.3F, .8F, 1.3F};
    const auto placement = anima::matrix(turned);
    const auto placed_center = anima::point(placement, center);
    view(placed_center + anima::Vec3{40, 12, 44}, placed_center);
    compare_views("placed", alone(tree, placement), alone(impostor, placement));
    // In height fog lit by the sun, which the view's height fog pipelines apply at each one's own pixels, the impostor
    // keeps the tree's parity.
    {
        anima::Environment hazy;
        hazy.fog.color = {.45F, .5F, .55F};
        hazy.fog.density = .02F;
        hazy.fog.height = center.y;
        hazy.fog.falloff = .3F;
        hazy.fog.sun_scattering = {.2F, .2F, .2F};
        renderer.set_environment(hazy);
        view(center + anima::Vec3{60, 8, 0}, center);
        compare_views("height-fog", alone(tree), alone(impostor));
        renderer.set_environment({});
    }

    // The shadow each casts on the ground from a low sun, seen from above where the tree itself is out of view: the
    // pixels that it darkens by more than 12 levels against the bare ground.
    anima::Environment environment;
    environment.sun.direction = {-.6F, .45F, .8F};
    // One cascade over the orthographic view's 59 m of depth, with texels of about 3 cm.
    environment.shadow_cascades.enabled = true;
    environment.shadow_cascades.count = 1;
    environment.shadow_cascades.distance = 60;
    renderer.set_environment(environment);
    {
        // Looking straight down at the ground 15 metres along the shadow, with the frame's up along it.
        const anima::Vec3 along{.6F, 0, -.8F}, back{0, 1, 0}, right = anima::cross(along, back),
                                                              eye = along * 15 + anima::Vec3{0, 30, 0};
        const anima::Mat4 camera{right.x, right.y, right.z, 0, along.x, along.y, along.z, 0,
                                 back.x,  back.y,  back.z,  0, eye.x,   eye.y,   eye.z,   1};
        renderer.set_view(anima::operator*(anima::orthographic(1, 20, 1, 60), anima::inverse(camera)));
    }
    const auto with_ground = [&](std::shared_ptr<const anima::Mesh> mesh) {
        auto scene = alone(std::move(mesh));
        (void)scene->add(ground());
        return scene;
    };
    auto bare = std::make_shared<anima::Scene>();
    (void)bare->add(ground());
    renderer.set_scenes({bare});
    (void)draw("ground");
    renderer.set_scenes({with_ground(tree)});
    (void)draw("shadow-mesh");
    renderer.set_scenes({with_ground(impostor)});
    (void)draw("shadow-impostor");
    {
        const auto [shadowed, share] =
            shadows(captures["ground"], captures["shadow-mesh"], captures["shadow-impostor"]);
        std::cout << "IMPOSTOR {\"view\":\"shadow\",\"shadowed\":" << shadowed << ",\"mismatch\":" << share << "}\n";
        captures.require(shadowed > 2000, "The tree cast too small a shadow to compare", {"ground", "shadow-mesh"});
        captures.require(share < shadow_parity,
                         "The impostor's shadow differs from the tree's in " + std::to_string(share) +
                             " of their pixels",
                         {"ground", "shadow-mesh", "shadow-impostor"});
        captures.discard({"ground", "shadow-mesh", "shadow-impostor"});
    }
    renderer.set_environment({});

    // The handover: the tree dissolves over 60 to 70 metres while the impostor appears over the same distances, both
    // measured to the same center, so at 65 metres each keeps the pixels the other leaves and none that either alone
    // covers is left uncovered.
    view(center + anima::Vec3{65, 0, 0}, center);
    renderer.set_scenes({alone(tree)});
    (void)draw("handover-mesh");
    renderer.set_scenes({alone(impostor)});
    (void)draw("handover-impostor");
    {
        auto scene = std::make_shared<anima::Scene>();
        scene->create("tree", tree).renderer().set_visibility_range({0, 70, 0, 10});
        scene->create("impostor", impostor)
            .renderer()
            .set_visibility_range({60, std::numeric_limits<float>::infinity(), 10, 0});
        renderer.set_scenes({scene});
    }
    (void)draw("handover");
    {
        const auto [covered, holes] =
            uncovered(captures["handover-mesh"], captures["handover-impostor"], captures["handover"]);
        std::cout << "IMPOSTOR {\"view\":\"handover\",\"covered\":" << covered << ",\"holes\":" << holes << "}\n";
        captures.require(covered > 1000 && holes == 0,
                         "The handover left " + std::to_string(holes) + " of " + std::to_string(covered) +
                             " pixels that both cover uncovered",
                         {"handover-mesh", "handover-impostor", "handover"});
        captures.discard({"handover-mesh", "handover-impostor", "handover"});
    }

    // 10,000 trees, 10 metres apart, each turned at random, drawn as meshes and as impostors from above one corner.
    std::vector<anima::Mat4> forest;
    Random random;
    for (int z = 0; z < 100; ++z)
        for (int x = 0; x < 100; ++x) {
            const float yaw = random.next() * std::numbers::pi_v<float>;
            anima::Transform copy;
            copy.translation = {10.F * float(x), 0, 10.F * float(z)};
            copy.rotation = {0, std::sin(yaw), 0, std::cos(yaw)};
            forest.push_back(anima::matrix(copy));
        }
    view({-30, 40, -30}, {300, 0, 300}, 1500);
    const auto measure = [&](const std::string &name, std::shared_ptr<const anima::Mesh> mesh) {
        auto scene = std::make_shared<anima::Scene>();
        scene->create(name, mesh).renderer().set_placements(anima::MeshPlacements::create(mesh, forest));
        renderer.set_scenes({scene});
        anima::ResourceStats stats;
        double gpu = 0;
        int timed = 0;
        for (int frame = 0; frame < 6; ++frame) {
            stats = draw(name);
            const auto profile = renderer.frame_profile();
            // The first frames upload and warm caches.
            if (frame >= 2 && profile.gpu_available) {
                gpu += profile.gpu_ms;
                ++timed;
            }
        }
        std::cout << "IMPOSTOR {\"forest\":\"" << name << "\",\"trees\":" << forest.size()
                  << ",\"drawn_copies\":" << stats.drawn_copies << ",\"submitted_indices\":" << stats.submitted_indices
                  << ",\"gpu_ms\":" << (timed ? gpu / timed : -1) << "}\n";
        return stats;
    };
    const auto meshes = measure("forest-meshes", tree);
    const auto impostors = measure("forest-impostors", impostor);
    captures.require_foreground("forest-impostors", "The impostor forest did not render");
    // The tree draws its trunk and crown per copy; the impostor, one quad, and its cube of bounds keeps a few more
    // clusters in view.
    require(impostors.submitted_indices * 100 < meshes.submitted_indices,
            "The impostor forest did not draw a hundredth of the meshes' indices");

    const auto stats = renderer.shutdown();
    require(!stats.validation_errors && !stats.validation_warnings, "Impostor validation failed");
    std::cout << "PASS impostors: a tree's impostor matches it within the stated parity from six directions, in "
                 "orthographic and placed views, in height fog and in its shadow, hands over without leaving a pixel "
                 "uncovered, and draws 10,000 copies with under a hundredth of the indices; validation_warnings=0 "
                 "validation_errors=0\n";
    return 0;
}
} // namespace impostor_test
