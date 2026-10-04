#pragma once
#include "gpu_checks.hpp"
#include "rejection.hpp"
#include "resources.hpp"
#include <anima/mesh_placements.hpp>
#include <limits>
#include <numbers>

// Draws a field of copies through one object's placements and the same copies as separate objects, over a ground
// that receives their sun shadows, and requires identical frames. Every placement is an integer translation, a
// quarter turn about Y and a Y scale of 1 or 2, every other one mirrored in one case, and the objects are placed by
// integer translations, so the matrices that the CPU composes for separate objects and the GPU composes for placed
// copies are exact and equal. A visibility range draws the same frames on both, and at a view distance scale of 2 half
// that range draws them again.
namespace placements_test {
// A square pyramid with flat faces: a 1 m base centered on the origin and its apex 1.5 m up.
inline std::shared_ptr<const anima::Mesh> pyramid() {
    anima::Asset asset;
    asset.nodes.resize(1);
    asset.materials.push_back({"stone", {.85F, .62F, .3F}, -1});
    anima::SourcePrimitive primitive;
    primitive.material = 0;
    const anima::Vec3 apex{0, 1.5F, 0};
    const std::array<anima::Vec3, 4> base{{{-.5F, 0, -.5F}, {-.5F, 0, .5F}, {.5F, 0, .5F}, {.5F, 0, -.5F}}};
    const auto triangle = [&](anima::Vec3 a, anima::Vec3 b, anima::Vec3 c) {
        const auto normal = anima::normalized(anima::cross(b - a, c - a));
        for (const auto corner : {a, b, c}) {
            anima::SourceVertex vertex;
            vertex.position = corner;
            vertex.normal = normal;
            vertex.color = {1, 1, 1};
            primitive.vertices.push_back(vertex);
        }
    };
    for (std::size_t i = 0; i < base.size(); ++i)
        triangle(base[i], base[(i + 1) % base.size()], apex);
    triangle(base[0], base[3], base[2]);
    triangle(base[0], base[2], base[1]);
    asset.primitives.push_back(std::move(primitive));
    return anima::Mesh::compile(asset);
}
// A gray square of ground from -15 to 65 m in X and Z.
inline std::shared_ptr<const anima::Mesh> ground() {
    anima::Asset asset;
    asset.nodes.resize(1);
    asset.materials.push_back({"ground", {.45F, .5F, .42F}, -1});
    anima::SourcePrimitive primitive;
    primitive.material = 0;
    for (const auto corner : {anima::Vec3{-15, 0, -15}, anima::Vec3{-15, 0, 65}, anima::Vec3{65, 0, 65},
                              anima::Vec3{-15, 0, -15}, anima::Vec3{65, 0, 65}, anima::Vec3{65, 0, -15}}) {
        anima::SourceVertex vertex;
        vertex.position = corner;
        vertex.normal = {0, 1, 0};
        vertex.color = {1, 1, 1};
        primitive.vertices.push_back(vertex);
    }
    asset.primitives.push_back(std::move(primitive));
    return anima::Mesh::compile(asset);
}
inline anima::Mat4 translation(anima::Vec3 t) {
    auto m = anima::identity();
    anima::set_translation(m, t);
    return m;
}
// Placements on a @p side by @p side grid 3 m apart: quarter turns about Y and Y scales of 1 or 2 that vary along
// the grid, with entries of 0, 1, -1 and 2 only.
inline std::vector<anima::Mat4> field(int side = 16) {
    std::vector<anima::Mat4> result;
    for (int z = 0; z < side; ++z)
        for (int x = 0; x < side; ++x) {
            const int turn = (x + 2 * z) % 4;
            const float c = turn == 0 ? 1.F : turn == 2 ? -1.F : 0.F, s = turn == 1 ? 1.F : turn == 3 ? -1.F : 0.F;
            const float height = (x * 7 + z * 3) % 2 ? 2.F : 1.F;
            // Columns: X axis (c, 0, -s), Y axis scaled by height, Z axis (s, 0, c), then the translation.
            result.push_back({c, 0, -s, 0, 0, height, 0, 0, s, 0, c, 0, 3.F * float(x), 0, 3.F * float(z), 1});
        }
    return result;
}

inline int run(int argc, char **argv) {
    using resource_test::require;
    require(argc == 3, "Usage: consumer --placements OUTPUT");
    const std::filesystem::path output = argv[2];
    static constexpr int window_size = 512;
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    gpu_check::Video video;
    const auto window = gpu_check::window("Anima placement verification", window_size, window_size);
    anima::RendererOptions options;
    options.validation = true;
    options.profile = true;
    anima::VulkanRenderer renderer(window.get(), options);
    // A low sun in -X casts shadows four times as long as each copy is tall toward +X.
    anima::Environment environment;
    environment.sun.direction = anima::normalized({-4, 1, 0});
    // Four cascades over the 150 m that holds the field from every view below.
    environment.shadow_cascades.enabled = true;
    environment.shadow_cascades.distance = 150;
    renderer.set_environment(environment);

    const auto shape = pyramid(), floor = ground();
    const auto placed_copies = field();
    const auto placements = anima::MeshPlacements::create(shape, placed_copies);
    // Separate objects, one placed object, and the ground alone.
    auto separate = std::make_shared<anima::Scene>(), placed = std::make_shared<anima::Scene>(),
         bare = std::make_shared<anima::Scene>();
    for (const auto &scene : {separate, placed, bare})
        (void)scene->create({}, floor);
    std::vector<anima::GameObject> copies;
    const auto place_separately = [&](const anima::Mat4 &world, const std::vector<anima::Mat4> &copy_placements) {
        for (std::size_t i = 0; i < copy_placements.size(); ++i)
            copies[i].set_world_matrix(world * copy_placements[i]);
    };
    for (std::size_t i = 0; i < placed_copies.size(); ++i)
        copies.push_back(separate->create({}, shape));
    auto field_object = placed->create("field", shape);
    field_object.renderer().set_placements(placements);
    const auto world = translation({2, 0, -3});
    place_separately(world, placed_copies);
    field_object.set_world_matrix(world);

    gpu_check::Captures captures(output);
    const auto began = std::chrono::steady_clock::now();
    const auto present = [&] {
        for (;;) {
            require(std::chrono::steady_clock::now() - began < gpu_check::watchdog, "Placement watchdog");
            SDL_Event event{};
            while (SDL_PollEvent(&event))
                require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                        "Placement check interrupted");
            if (renderer.draw())
                return;
            SDL_Delay(5);
        }
    };
    auto draw = [&](const std::shared_ptr<anima::Scene> &scene, const std::string &name) {
        renderer.set_scenes({scene});
        renderer.request_capture();
        present();
        captures.add(name, gpu_check::take(renderer));
        return renderer.resource_stats();
    };
    const auto view = [&](anima::Vec3 eye, anima::Vec3 target) {
        renderer.set_view(anima::perspective(std::numbers::pi_v<float> / 4, 1, .1F, 300) * anima::look_at(eye, target));
    };
    // Draws the separate and the placed copies under the current view and requires the same frame.
    const auto compare = [&](const std::string &name) {
        const auto apart = draw(separate, name + "-separate");
        const auto together = draw(placed, name + "-placed");
        captures.require_same(name + "-separate", name + "-placed", "Placed copies differ from separate objects");
        std::cout << "CASE {\"name\":\"" << name << "\",\"separate_draw_calls\":" << apart.draw_calls
                  << ",\"placed_draw_calls\":" << together.draw_calls << ",\"drawn_copies\":" << together.drawn_copies
                  << ",\"culled_clusters\":" << together.culled_clusters
                  << ",\"placed_shadow_draw_calls\":" << together.shadow_draw_calls << "}\n";
        return std::pair{apart, together};
    };
    // Draws @p scene alone, then clears the selection and draws again, and requires the second frame to count no
    // main-view draw, copy, level, culled cluster or range-culled object. Returns the first frame's statistics.
    const auto draw_then_clear = [&](const std::shared_ptr<anima::Scene> &scene) {
        renderer.set_scenes({scene});
        present();
        const auto drawn = renderer.resource_stats();
        renderer.set_scenes({});
        present();
        const auto cleared = renderer.resource_stats();
        require(!cleared.draw_calls && !cleared.submitted_indices && !cleared.drawn_copies && !cleared.lod_draws &&
                    !cleared.culled_clusters && !cleared.range_culled,
                "Clearing the selection kept the previous frame's main-view counters");
        return drawn;
    };

    // The whole field, every copy drawn by one call per run of clusters.
    view({24, 55, 85}, {24, 0, 20});
    const auto [whole_apart, whole] = compare("whole");
    require(whole_apart.draw_calls == placed_copies.size() + 1 && whole_apart.drawn_copies == placed_copies.size() + 1,
            "Separate objects did not draw every copy");
    require(whole.drawn_copies == placed_copies.size() + 1 && !whole.culled_clusters,
            "Placements did not draw every copy in view");
    require(whole.draw_calls >= 2 && whole.draw_calls <= placements->clusters().size() + 1,
            "Placements took more than one call per cluster run");
    require(whole.resident_placement_bytes >= placed_copies.size() * 48, "Placements are not resident");
    captures.require_foreground("whole-placed", "The field did not render");

    // A corner of the field: the clusters out of view are culled, and the copies in view still match.
    view({7, 10, 11}, {7, 0, 2});
    const auto [corner_apart, corner] = compare("corner");
    require(corner.culled_clusters > 0 && corner.drawn_copies < whole.drawn_copies,
            "Clusters out of view were not culled");
    // A cluster's bounds hold its copies' bounds, so cluster culling keeps every copy that object culling keeps.
    require(corner.drawn_copies >= corner_apart.drawn_copies, "Cluster culling dropped a copy in view");
    // Clearing the selection after a frame that culls clusters leaves none of its counters behind.
    const auto corner_cleared = draw_then_clear(placed);
    require(corner_cleared.culled_clusters > 0 && corner_cleared.drawn_copies > 0,
            "The corner culled no cluster before the selection was cleared");

    // Beside the field, where no copy is in view but their shadows fall: the shadow pass draws placements that the
    // view culls.
    view({53, 6, 22}, {53, 0, 21.9F});
    const auto [shade_apart, shade] = compare("shadows");
    // The view culls the whole object, so it draws only the ground.
    require(shade.drawn_copies == 1 && shade.culled_instances == 1, "The view drew copies it cannot see");
    require(shade.shadow_draw_calls > 0, "Placements cast no shadows");
    (void)draw(bare, "shadows-bare");
    captures.require_changed("shadows-bare", "shadows-placed", .02, "Placed copies cast no visible shadow");

    // Moving the object moves every copy and its shadow.
    const auto moved = translation({-4, 0, 5});
    place_separately(moved, placed_copies);
    field_object.set_world_matrix(moved);
    view({24, 55, 85}, {24, 0, 20});
    (void)compare("moved");
    captures.require_changed("whole-placed", "moved-placed", .01, "Moving the object did not move its copies");

    // One visibility range on every separate object and on the placed copies draws the same frame: from this view the
    // far rows lie beyond its end and are hidden, the rows within its 15 m margin dissolve by the same dither, and the
    // rest draw whole. Separate objects outside the range are culled whole on the CPU.
    const anima::VisibilityRange range{0, 95, 0, 15};
    for (const auto &copy : copies)
        copy.renderer().set_visibility_range(range);
    field_object.renderer().set_visibility_range(range);
    const auto [ranged_apart, ranged] = compare("ranged");
    require(ranged_apart.range_culled > 0, "No separate object fell outside the visibility range");
    captures.require_changed("moved-placed", "ranged-placed", .002, "The visibility range hid no copy");
    std::cout << "RANGE {\"separate_range_culled\":" << ranged_apart.range_culled
              << ",\"placed_range_culled\":" << ranged.range_culled << "}\n";
    // Nor after a frame that culls objects by their visibility range.
    require(draw_then_clear(separate).range_culled > 0,
            "No separate object fell outside the visibility range before the selection was cleared");

    // At a view distance scale of 2, half that range draws the same frames: culling and the shaders' dither both
    // scale it, exactly at a power of two, so the copies from its end out to twice it still draw and the same rows
    // dissolve. Back at 1 the halved range hides more of the field.
    constexpr float doubled = 2;
    const anima::VisibilityRange halved{range.begin / doubled, range.end / doubled, range.begin_margin / doubled,
                                        range.end_margin / doubled};
    for (const auto &copy : copies)
        copy.renderer().set_visibility_range(halved);
    field_object.renderer().set_visibility_range(halved);
    require(renderer.view_distance_scale() == 1, "The view distance scale did not start at 1");
    renderer.set_view_distance_scale(doubled);
    for (const float invalid :
         {0.F, -1.F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        rejection::rejects<std::invalid_argument>([&] { renderer.set_view_distance_scale(invalid); },
                                                  "View distance scale must be finite and positive");
    require(renderer.view_distance_scale() == doubled, "A rejected view distance scale replaced the scale");
    const auto [scaled_apart, scaled] = compare("scaled");
    captures.require_same("ranged-placed", "scaled-placed", "Twice the view distance did not draw twice the range");
    require(scaled_apart.range_culled == ranged_apart.range_culled && scaled.range_culled == ranged.range_culled,
            "Twice the view distance culled other objects or clusters than twice the range");
    renderer.set_view_distance_scale(1);
    (void)draw(placed, "halved-placed");
    captures.require_changed("ranged-placed", "halved-placed", .002, "The view distance scale did not scale the range");
    for (const auto &copy : copies)
        copy.renderer().set_visibility_range({});
    field_object.renderer().set_visibility_range({});

    // Every other copy mirrored: its placement negates the X axis, so its determinant is negative and its triangles'
    // winding reverses, and placed copies must shade the same outward faces as separate mirrored objects.
    auto mirrored_copies = placed_copies;
    for (std::size_t i = 0; i < mirrored_copies.size(); i += 2)
        for (std::size_t row = 0; row < 3; ++row)
            mirrored_copies[i][row] = -mirrored_copies[i][row];
    place_separately(moved, mirrored_copies);
    field_object.renderer().set_placements(anima::MeshPlacements::create(shape, mirrored_copies));
    const auto [mirrored_apart, mirrored] = compare("mirrored");
    // As in the corner, cluster culling keeps every mirrored copy that object culling keeps.
    require(mirrored.drawn_copies >= mirrored_apart.drawn_copies, "Mirrored placements did not draw every copy");

    // Without placements the object draws its one copy at its own place.
    field_object.renderer().set_placements(nullptr);
    const auto single = draw(placed, "single");
    require(single.drawn_copies == 2 && single.draw_calls == 2, "Clearing placements did not leave one copy");

    // 10,000 copies as separate objects and through placements, reporting medians of 60 frames after 10 more; the
    // timings are for comparison on one machine, not a pass condition.
    const auto many = field(100);
    auto separate_many = std::make_shared<anima::Scene>(), placed_many = std::make_shared<anima::Scene>();
    for (const auto &placement : many)
        separate_many->create({}, shape).set_world_matrix(placement);
    placed_many->create("field", shape).renderer().set_placements(anima::MeshPlacements::create(shape, many));
    anima::OrbitCamera camera;
    const auto extent = separate_many->bounds();
    camera.frame(extent.minimum, extent.maximum);
    renderer.set_view(camera.matrix(1));
    for (const auto &[scene, name] : {std::pair{separate_many, "separate"}, std::pair{placed_many, "placed"}}) {
        renderer.set_scenes({scene});
        std::vector<double> upload, record, gpu;
        for (int frame = 0; frame < 70; ++frame) {
            present();
            const auto profile = renderer.frame_profile();
            if (frame < 10)
                continue;
            upload.push_back(profile.upload_ms);
            record.push_back(profile.record_submit_ms);
            if (profile.gpu_available)
                gpu.push_back(profile.gpu_ms);
        }
        const auto median = [](std::vector<double> values) {
            if (values.empty())
                return -1.0;
            std::nth_element(values.begin(), values.begin() + std::ptrdiff_t(values.size() / 2), values.end());
            return values[values.size() / 2];
        };
        const auto stats = renderer.resource_stats();
        require(stats.drawn_copies == many.size(), "The measured view did not draw every copy");
        std::cout << "BENCH {\"copies\":" << many.size() << ",\"drawn_as\":\"" << name
                  << "\",\"draw_calls\":" << stats.draw_calls << ",\"shadow_draw_calls\":" << stats.shadow_draw_calls
                  << ",\"upload_ms\":" << median(upload) << ",\"record_submit_ms\":" << median(record)
                  << ",\"gpu_ms\":" << median(gpu) << "}\n";
    }

    const auto stats = renderer.shutdown();
    require(!stats.validation_errors && !stats.validation_warnings, "Placement validation failed");
    std::cout << "PASS placements: " << placed_copies.size()
              << " copies through one object match separate objects with shadows and visibility ranges, culled by "
                 "cluster and scaled by the view distance scale; "
                 "validation_warnings=0 validation_errors=0\n";
    return 0;
}
} // namespace placements_test
