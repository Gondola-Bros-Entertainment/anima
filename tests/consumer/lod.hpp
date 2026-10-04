#pragma once
#include "gpu_checks.hpp"
#include "rejection.hpp"
#include "resources.hpp"
#include <anima/mesh_placements.hpp>
#include <limits>
#include <numbers>

// Levels of detail: dense spheres, as one object and as a field of placed copies over a shadowed ground, drawn at the
// default 1 pixel threshold and with levels off. Far away the threshold draws simplified levels, in the view and the
// shadow passes, and the frames match within the renderer's pixel parity; up close it draws finer levels. A skinned
// sphere chooses its levels by its joints' scale, and a flat-shaded one simplifies within the same parity.
namespace lod_test {
inline anima::Asset sphere_asset() {
    anima::Asset asset;
    asset.nodes.resize(1);
    asset.materials.push_back({"sphere", {.7F, .55F, .4F}, -1});
    anima::SourcePrimitive primitive;
    primitive.material = 0;
    constexpr int rings = 48, segments = 96;
    // Vertices repeat exactly at the poles and where the last segment meets the first, so the sphere is closed, as an
    // exported mesh is; in float, sin(2 pi) is not 0.
    const auto corner = [&](int ring, int segment) {
        const float theta = std::numbers::pi_v<float> * float(ring) / rings,
                    phi = 2 * std::numbers::pi_v<float> * float(segment % segments) / segments;
        const anima::Vec3 direction = ring == 0       ? anima::Vec3{0, 1, 0}
                                      : ring == rings ? anima::Vec3{0, -1, 0}
                                                      : anima::Vec3{std::sin(theta) * std::cos(phi), std::cos(theta),
                                                                    std::sin(theta) * std::sin(phi)};
        anima::SourceVertex vertex;
        vertex.position = {direction.x * .5F, direction.y * .5F + .5F, direction.z * .5F};
        vertex.normal = direction;
        vertex.color = {1, 1, 1};
        return vertex;
    };
    for (int ring = 0; ring < rings; ++ring)
        for (int segment = 0; segment < segments; ++segment) {
            const auto a = corner(ring, segment), b = corner(ring + 1, segment), c = corner(ring + 1, segment + 1),
                       d = corner(ring, segment + 1);
            for (const auto &v : {a, c, b, a, d, c})
                primitive.vertices.push_back(v);
        }
    asset.primitives.push_back(std::move(primitive));
    return asset;
}
// The sphere skinned to one joint that scales it by 0.01, for an object whose world matrix scales it back by 100.
inline anima::Asset skinned_sphere_asset() {
    auto asset = sphere_asset();
    asset.nodes.resize(2);
    asset.nodes[1].rest.scale = {.01F, .01F, .01F};
    asset.skins.push_back({{1}, {anima::identity()}});
    auto &primitive = asset.primitives[0];
    primitive.skin = 0;
    for (auto &vertex : primitive.vertices) {
        vertex.joints = {0, 0, 0, 0};
        vertex.weights = {1, 0, 0, 0};
    }
    return asset;
}
// The sphere shaded flat: each triangle has its own outward normal, so its vertices weld with no neighbor's, as in a
// model with hard edges.
inline anima::Asset faceted_sphere_asset() {
    auto asset = sphere_asset();
    auto &vertices = asset.primitives[0].vertices;
    for (std::size_t t = 0; t < vertices.size(); t += 3) {
        auto normal = anima::normalized(anima::cross(vertices[t + 1].position - vertices[t].position,
                                                     vertices[t + 2].position - vertices[t].position));
        if (anima::dot(normal, vertices[t].position - anima::Vec3{0, .5F, 0}) < 0)
            normal = -normal;
        for (std::size_t k = 0; k < 3; ++k)
            vertices[t + k].normal = normal;
    }
    return asset;
}
inline std::shared_ptr<const anima::Mesh> ground() {
    anima::Asset asset;
    asset.nodes.resize(1);
    asset.materials.push_back({"ground", {.45F, .5F, .42F}, -1});
    anima::SourcePrimitive primitive;
    primitive.material = 0;
    for (const auto corner : {anima::Vec3{-10, 0, -10}, anima::Vec3{-10, 0, 70}, anima::Vec3{70, 0, 70},
                              anima::Vec3{-10, 0, -10}, anima::Vec3{70, 0, 70}, anima::Vec3{70, 0, -10}}) {
        anima::SourceVertex vertex;
        vertex.position = corner;
        vertex.normal = {0, 1, 0};
        vertex.color = {1, 1, 1};
        primitive.vertices.push_back(vertex);
    }
    asset.primitives.push_back(std::move(primitive));
    return anima::Mesh::compile(asset);
}

// Pixels where @p actual differs from @p reference by more than 16 levels farther than a pixel from any edge of
// @p reference, a pixel next to one that differs from it by more than 16 levels. A level of detail within the 1 pixel
// threshold moves silhouettes and shadow edges by up to a pixel and changes nothing else that much.
inline std::size_t changes_off_edges(const gpu_check::Image &reference, const gpu_check::Image &actual) {
    const auto w = std::ptrdiff_t(reference.width), h = std::ptrdiff_t(reference.height);
    const auto differs = [](const gpu_check::Rgb &a, const gpu_check::Rgb &b) {
        return std::max({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2])}) > 16;
    };
    const auto at = [](const gpu_check::Image &image, std::ptrdiff_t x, std::ptrdiff_t y) {
        return gpu_check::pixel(image, std::size_t(x), std::size_t(y));
    };
    std::vector<bool> edge(std::size_t(w * h));
    for (std::ptrdiff_t y = 0; y < h; ++y)
        for (std::ptrdiff_t x = 0; x < w; ++x)
            for (std::ptrdiff_t dy = -1; dy <= 1; ++dy)
                for (std::ptrdiff_t dx = -1; dx <= 1; ++dx)
                    if (x + dx >= 0 && x + dx < w && y + dy >= 0 && y + dy < h &&
                        differs(at(reference, x, y), at(reference, x + dx, y + dy)))
                        edge[std::size_t(y * w + x)] = true;
    std::size_t outside = 0;
    for (std::ptrdiff_t y = 0; y < h; ++y)
        for (std::ptrdiff_t x = 0; x < w; ++x) {
            if (!differs(at(reference, x, y), at(actual, x, y)))
                continue;
            bool near = false;
            for (std::ptrdiff_t dy = -1; dy <= 1 && !near; ++dy)
                for (std::ptrdiff_t dx = -1; dx <= 1 && !near; ++dx)
                    near = x + dx >= 0 && x + dx < w && y + dy >= 0 && y + dy < h &&
                           edge[std::size_t((y + dy) * w + x + dx)];
            outside += !near;
        }
    return outside;
}

inline int run(int argc, char **argv) {
    using resource_test::require;
    require(argc == 3, "Usage: consumer --lod OUTPUT");
    static constexpr int window_size = 512;
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    gpu_check::Video video;
    const auto window = gpu_check::window("Anima level of detail verification", window_size, window_size);
    anima::RendererOptions options;
    options.validation = true;
    anima::VulkanRenderer renderer(window.get(), options);
    for (const float invalid : {-1.F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        rejection::rejects<std::invalid_argument>([&] { renderer.set_lod_threshold(invalid); },
                                                  "LOD threshold must be finite and nonnegative");
    anima::Environment environment;
    // Four cascades over the 150 m that holds the field from every view below.
    environment.shadow_cascades.enabled = true;
    environment.shadow_cascades.distance = 150;
    renderer.set_environment(environment);

    const auto sphere = anima::Mesh::compile(sphere_asset(), {.lods = {.levels = 6}});
    require(sphere->draws()[0].levels.size() >= 3, "The sphere did not simplify into levels");
    std::vector<anima::Mat4> field;
    for (int z = 0; z < 20; ++z)
        for (int x = 0; x < 20; ++x) {
            auto m = anima::identity();
            anima::set_translation(m, {3.F * float(x), 0, 3.F * float(z)});
            field.push_back(m);
        }
    auto scene = std::make_shared<anima::Scene>();
    (void)scene->create({}, ground());
    scene->create("field", sphere).renderer().set_placements(anima::MeshPlacements::create(sphere, field));
    auto single = scene->create("single", sphere);
    single.set_world_matrix([] {
        auto m = anima::identity();
        anima::set_translation(m, {28.5F, 0, -4});
        return m;
    }());
    renderer.set_scenes({scene});

    gpu_check::Captures captures(argv[2]);
    const auto began = std::chrono::steady_clock::now();
    const auto draw = [&](const std::string &name, float threshold) {
        renderer.set_lod_threshold(threshold);
        renderer.request_capture();
        for (;;) {
            require(std::chrono::steady_clock::now() - began < gpu_check::watchdog, "LOD watchdog");
            SDL_Event event{};
            while (SDL_PollEvent(&event))
                require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                        "LOD check interrupted");
            if (renderer.draw())
                break;
            SDL_Delay(5);
        }
        captures.add(name, gpu_check::take(renderer));
        const auto stats = renderer.resource_stats();
        std::cout << "LOD {\"name\":\"" << name << "\",\"threshold\":" << threshold
                  << ",\"draw_calls\":" << stats.draw_calls << ",\"lod_draws\":" << stats.lod_draws
                  << ",\"submitted_indices\":" << stats.submitted_indices
                  << ",\"shadow_submitted_indices\":" << stats.shadow_submitted_indices << "}\n";
        return stats;
    };
    const auto view = [&](anima::Vec3 eye, anima::Vec3 target) {
        renderer.set_view(anima::perspective(std::numbers::pi_v<float> / 4, 1, .1F, 300) * anima::look_at(eye, target));
    };

    // Simplified levels may move silhouettes and shadow edges by up to a pixel and nothing more: every pixel that
    // changes by more than 16 levels lies within a pixel of an edge of the full frame, and the mean color barely moves.
    const auto require_levels_match = [&](const std::string &full, const std::string &levels) {
        const auto off_edges = changes_off_edges(captures[full], captures[levels]);
        const auto difference = gpu_check::parity(captures[full], captures[levels]);
        captures.require(off_edges == 0 && difference.mean < .5,
                         levels + " differs from " + full + " away from its edges in " + std::to_string(off_edges) +
                             " pixels, with a mean channel difference of " + std::to_string(difference.mean),
                         {full, levels});
    };

    // At a middle distance, where each sphere is 15 to 30 pixels across, simplified levels draw fewer indices in the
    // view and the shadow passes.
    view({28.5F, 14, 62}, {28.5F, 0, 30});
    const auto middle_full = draw("middle-full", 0);
    const auto middle = draw("middle-levels", 1);
    require(!middle_full.lod_draws && middle.lod_draws > 0, "Draws at a middle distance did not choose levels");
    require(middle.submitted_indices < middle_full.submitted_indices / 2 &&
                middle.shadow_submitted_indices < middle_full.shadow_submitted_indices,
            "Simplified levels did not halve the indices drawn, or the shadow passes drew the full draws");
    require_levels_match("middle-full", "middle-levels");
    captures.require_foreground("middle-levels", "The field did not render");
    // Clearing the selection after a frame that drew levels leaves none of its main-view counters behind.
    renderer.set_scenes({});
    const auto cleared = draw("middle-cleared", 1);
    require(!cleared.draw_calls && !cleared.submitted_indices && !cleared.drawn_copies && !cleared.lod_draws &&
                !cleared.culled_clusters && !cleared.range_culled,
            "Clearing the selection kept the previous frame's main-view counters");
    renderer.set_scenes({scene});

    // Far away, where each sphere covers about 6 pixels, the levels draw at least ten times fewer indices.
    view({28.5F, 45, 110}, {28.5F, 0, 28});
    const auto far_full = draw("far-full", 0);
    const auto far = draw("far-levels", 1);
    require(far.submitted_indices * 10 < far_full.submitted_indices &&
                far.shadow_submitted_indices * 10 < far_full.shadow_submitted_indices,
            "Far levels did not draw a tenth of the indices");
    require_levels_match("far-full", "far-levels");

    // Up close the single sphere draws a finer level than each far one does, still within a pixel of its full draw:
    // 1 pixel at 2 m allows about 3 mm, which a quarter of its triangles already meet.
    view({28.5F, 1, -1.5F}, {28.5F, .5F, -4});
    (void)draw("close-full", 0);
    const auto close = draw("close-levels", 1);
    require_levels_match("close-full", "close-levels");
    constexpr std::uint64_t ground_indices = 6;
    const std::uint64_t far_spheres = field.size() + 1;
    require(close.submitted_indices - ground_indices > 4 * (far.submitted_indices - ground_indices) / far_spheres,
            "The near sphere did not draw a finer level than the far ones");

    // One sphere at a time at the middle distance. A skinned copy that its world matrix scales by 100 and its joint
    // by 0.01 is the rigid sphere's size and chooses the same level, since the joint matrices place a skinned draw.
    // Each casts the level that it draws: every shadow pass submits the indices of its one view draw call. Shaded flat,
    // the sphere simplifies across its hard edges within the same parity.
    view({28.5F, 14, 62}, {28.5F, 0, 30});
    const auto at = [](float scale) {
        auto m = anima::identity();
        m[0] = m[5] = m[10] = scale;
        anima::set_translation(m, {28.5F, 0, 30});
        return m;
    };
    const auto only = [&](std::shared_ptr<const anima::Mesh> mesh, const anima::Mat4 &world) {
        auto subject = std::make_shared<anima::Scene>();
        subject->create("subject", std::move(mesh)).set_world_matrix(world);
        renderer.set_scenes({subject});
    };
    const auto casts_drawn_level = [](const anima::ResourceStats &stats) {
        return stats.draw_calls == 1 && stats.shadow_draw_calls > 0 &&
               stats.shadow_submitted_indices == stats.shadow_draw_calls * stats.submitted_indices;
    };
    only(sphere, at(1));
    const auto rigid = draw("rigid-levels", 1);
    require(rigid.lod_draws > 0, "The rigid sphere did not choose a level");
    require(casts_drawn_level(rigid), "The rigid sphere cast another level than it drew");
    only(anima::Mesh::compile(skinned_sphere_asset(), {.lods = {.levels = 6}}), at(100));
    (void)draw("skinned-full", 0);
    const auto skinned = draw("skinned-levels", 1);
    require(skinned.submitted_indices == rigid.submitted_indices &&
                skinned.shadow_submitted_indices == rigid.shadow_submitted_indices,
            "The skinned sphere chose another level than the rigid one");
    require(casts_drawn_level(skinned), "The skinned sphere cast another level than it drew");
    require_levels_match("skinned-full", "skinned-levels");
    only(anima::Mesh::compile(faceted_sphere_asset(), {.lods = {.levels = 6}}), at(1));
    const auto faceted_full = draw("faceted-full", 0);
    const auto faceted = draw("faceted-levels", 1);
    require(faceted.lod_draws > 0 && faceted.submitted_indices < faceted_full.submitted_indices / 2,
            "The faceted sphere did not simplify");
    require_levels_match("faceted-full", "faceted-levels");

    const auto stats = renderer.shutdown();
    require(!stats.validation_errors && !stats.validation_warnings, "LOD validation failed");
    std::cout << "PASS levels of detail: copies, objects, skinned and flat-shaded meshes draw simplified levels that "
                 "move only their edges, by at most a pixel, in the view and the shadow passes, finer nearer the eye; "
                 "validation_warnings=0 validation_errors=0\n";
    return 0;
}
} // namespace lod_test
