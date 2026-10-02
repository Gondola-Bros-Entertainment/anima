#pragma once
#include "environment.hpp"
#include "gpu_checks.hpp"
#include "materials.hpp"
#include "placements.hpp"
#include "resources.hpp"
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

// The sun's shadow cascades against what ShadowCascades states. The same plate casts an edge 4 m from the camera and
// 60 m away, and each edge's lit-to-shadowed transition spans the texels of the cascade that fit_shadow_cascades() says
// holds it, so the far edge widens by the ratio of their texels. Moving the camera by less than a texel leaves an edge
// where it lies on the ground, since cascades move by whole texels. A wall's long edge stays where its geometry puts it
// across every seam between cascades and within their blends, and a caster toward the sun beyond every cascade's sphere
// still shadows the ground in view. Last, the cost of one to four cascades over a field of 10,000 copies, reported for
// comparison on one machine rather than checked.
namespace cascades_test {
using gpu_check::Image;
// The camera's eye and target, 3 m above the ground looking along -Z, and a sun 31 degrees up in +X, so that shadows
// fall toward -X, one texel of a cascade spanning 1 / 0.514 of its width on the ground across their edges.
constexpr anima::Vec3 eye{0, 3, 0}, target{0, 0, -10}, sun{1, .6F, 0};
inline anima::Mat4 view(anima::Vec3 offset = {}) {
    using anima::operator*;
    return anima::perspective(4.F / 3, .1F, 300) * anima::look_at(eye + offset, target + offset);
}
// Four cascades to 100 m of 512 texels, coarse enough that the window resolves each edge's transition.
inline anima::Environment lighting(std::uint32_t count = 4, std::uint32_t resolution = 512) {
    anima::Environment environment;
    environment.sun.direction = sun;
    environment.sun.radiance = {3, 3, 3};
    environment.fill.radiance = {};
    environment.ambient_sky = environment.ambient_ground = {.1F, .1F, .1F};
    environment.shadow_cascades.enabled = true;
    environment.shadow_cascades.count = count;
    environment.shadow_cascades.resolution = resolution;
    return environment;
}
// The ground point (x, 0, z) under the sun: where a point at height y above (x, z) shadows the ground.
inline double shadow_x(double x, double y) { return x - y * double(sun.x) / double(sun.y); }
// The width on the ground across a shadow edge that runs along Z of one texel of @p cascade.
inline double ground_texel(const anima::ShadowCascade &cascade) {
    return cascade.texel / (double(sun.y) / std::hypot(double(sun.x), double(sun.y)));
}
// The cascade of @p cascades whose depths hold ground point (x, 0, z) under view(), or the coarser one where two blend.
inline const anima::ShadowCascade &holding(const std::vector<anima::ShadowCascade> &cascades, double z) {
    const auto forward = anima::normalized(target - eye);
    const auto depth = double(anima::dot(anima::Vec3{0, 0, float(z)} - eye, forward));
    for (std::size_t i = 0; i < cascades.size(); ++i) {
        const auto &cascade = cascades[i];
        const double band = .1 * (double(cascade.end) - cascade.begin);
        if (depth < cascade.end)
            return depth > cascade.end - band && i + 1 < cascades.size() ? cascades[i + 1] : cascade;
    }
    return cascades.back();
}
// The summed linear channels of @p image at pixel position @p at, decoded from the capture's sRGB and bilinearly
// interpolated between pixel centers. Edges are measured in linear light, where the filter's share of lit texels scales
// the sun's light; halfway between two sRGB levels lies nearer the darker one in linear light.
inline double luminance(const Image &image, std::array<float, 2> at) {
    const double x = at[0] - .5, y = at[1] - .5;
    const auto x0 = long(std::floor(x)), y0 = long(std::floor(y));
    double sum = 0;
    for (const long dy : {0L, 1L})
        for (const long dx : {0L, 1L}) {
            const auto px = std::clamp(x0 + dx, 0L, long(image.width) - 1),
                       py = std::clamp(y0 + dy, 0L, long(image.height) - 1);
            const auto c = gpu_check::pixel(image, std::size_t(px), std::size_t(py));
            const double weight =
                (dx ? x - double(x0) : 1 - (x - double(x0))) * (dy ? y - double(y0) : 1 - (y - double(y0)));
            sum += weight * (material_test::srgb_to_linear(c[0]) + material_test::srgb_to_linear(c[1]) +
                             material_test::srgb_to_linear(c[2]));
        }
    return sum;
}
// Where a shadow edge that runs along Z lies on the ground line at @p z, read from @p image under @p view_projection
// between @p lit_x, outside the shadow, and @p dark_x, inside it: the points where the light falls to 90, 50 and 10
// percent of the way from its lit to its shadowed level.
struct Edge {
    double x90{}, x50{}, x10{};
    double width() const { return std::abs(x10 - x90); }
};
inline std::optional<Edge> find_edge(const Image &image, const anima::Mat4 &view_projection, double z, double lit_x,
                                     double dark_x) {
    constexpr int samples = 400, ends = 20;
    std::vector<double> light(samples + 1);
    for (int i = 0; i <= samples; ++i) {
        const auto x = lit_x + (dark_x - lit_x) * i / samples;
        light[std::size_t(i)] = luminance(
            image, environment_test::project(view_projection, {float(x), 0, float(z)}, image.width, image.height));
    }
    double lit = 0, dark = 0;
    for (int i = 0; i < ends; ++i) {
        lit += light[std::size_t(i)] / ends;
        dark += light[std::size_t(samples - i)] / ends;
    }
    constexpr double least_contrast = .5; // Summed linear channels between the lit and the shadowed ground.
    if (lit - dark < least_contrast)
        return std::nullopt;
    const auto crossing = [&](double share) -> std::optional<double> {
        const auto level = dark + share * (lit - dark);
        for (int i = 1; i <= samples; ++i)
            if (light[std::size_t(i)] <= level) {
                const auto before = light[std::size_t(i - 1)], after = light[std::size_t(i)];
                const auto t = before == after ? 0. : (before - level) / (before - after);
                return lit_x + (dark_x - lit_x) * (i - 1 + t) / samples;
            }
        return std::nullopt;
    };
    const auto x90 = crossing(.9), x50 = crossing(.5), x10 = crossing(.1);
    if (!x90 || !x50 || !x10)
        return std::nullopt;
    return Edge{*x90, *x50, *x10};
}

inline int run(int argc, char **argv) {
    using resource_test::require;
    require(argc == 3, "Usage: consumer --cascades OUTPUT");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    gpu_check::Video video;
    const auto window = gpu_check::window("Anima shadow cascade verification", 800, 600);
    anima::RendererOptions options;
    options.validation = true;
    options.profile = true;
    anima::VulkanRenderer renderer(window.get(), options);
    gpu_check::Captures captures(argv[2]);
    const auto began = std::chrono::steady_clock::now();
    const auto present = [&] {
        for (;;) {
            require(std::chrono::steady_clock::now() - began < gpu_check::watchdog, "Cascade watchdog");
            SDL_Event event{};
            while (SDL_PollEvent(&event))
                require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                        "Cascade check interrupted");
            if (renderer.draw())
                return;
            SDL_Delay(5);
        }
    };
    const auto draw = [&](const std::string &name) {
        renderer.request_capture();
        present();
        captures.add(name, gpu_check::take(renderer));
        return renderer.resource_stats();
    };
    const auto ground = anima::Mesh::compile(*environment_test::box_fixture({200, 0, 200}, true));
    // A square plate @p half wide each way and 4 cm thick, @p height above the ground at @p z.
    const auto plate = [&](anima::Scene &scene, float half, float height, float z) {
        const auto mesh = anima::Mesh::compile(*environment_test::box_fixture({half, .02F, half}));
        const auto id = scene.add(mesh);
        auto world = anima::identity();
        anima::set_translation(world, {0, height, z});
        scene.set_pose(id, mesh->rest_pose(), world);
    };

    // The same plate near the camera and far from it.
    constexpr float near_half = .6F, near_height = .4F, near_z = -4, far_half = 6, far_height = 2, far_z = -60;
    auto plates = std::make_shared<anima::Scene>();
    (void)plates->add(ground);
    plate(*plates, near_half, near_height, near_z);
    plate(*plates, far_half, far_height, far_z);
    renderer.set_scenes({plates});
    renderer.set_environment(lighting());
    renderer.set_view(view());
    const auto plates_stats = draw("plates");
    const auto cascades = anima::fit_shadow_cascades(lighting(), view());
    require(cascades.size() == 4, "The view fits fewer than four cascades");
    require(plates_stats.shadow_bytes >= 4ULL * 512 * 512 * 2, "The four cascades' layers were not allocated");
    // Each plate's shadow begins where the sun projects its top edge nearest -X.
    const auto &near_cascade = holding(cascades, near_z), &far_cascade = holding(cascades, far_z);
    require(&near_cascade != &far_cascade, "The plates lie in one cascade");
    // Measures the edge within 6 of @p cascade's texels either way of @p edge_x, or within @p limit, which keeps the
    // shadowed end inside a narrow shadow.
    const auto measure = [&](const std::string &name, const anima::Mat4 &view_projection, double z, double edge_x,
                             const anima::ShadowCascade &cascade, double limit = 1e9) {
        const auto span = std::min(6 * ground_texel(cascade), limit);
        const auto found = find_edge(captures[name], view_projection, z, edge_x - span, edge_x + span);
        captures.require(found.has_value(), "No shadow edge at " + std::to_string(z) + " m in " + name, {name});
        return *found;
    };
    const auto near_edge_x = shadow_x(-near_half, near_height + .02),
               far_edge_x = shadow_x(-far_half, far_height + .02);
    const auto near_edge = measure("plates", view(), near_z, near_edge_x, near_cascade);
    const auto far_edge = measure("plates", view(), far_z, far_edge_x, far_cascade);
    // The filter's bilinearly weighted 3x3 texels ramp a straight edge's light over 3 texels, so from 90 to 10 percent
    // over 2.4.
    const auto near_texels = near_edge.width() / ground_texel(near_cascade),
               far_texels = far_edge.width() / ground_texel(far_cascade);
    const auto measured_ratio = far_edge.width() / near_edge.width(),
               stated_ratio = double(far_cascade.texel) / near_cascade.texel;
    std::cout << "CASCADES {\"near_texel_m\":" << near_cascade.texel << ",\"far_texel_m\":" << far_cascade.texel
              << ",\"near_edge_m\":" << near_edge.width() << ",\"far_edge_m\":" << far_edge.width()
              << ",\"near_edge_texels\":" << near_texels << ",\"far_edge_texels\":" << far_texels
              << ",\"measured_ratio\":" << measured_ratio << ",\"stated_ratio\":" << stated_ratio << "}\n";
    for (const auto texels : {near_texels, far_texels})
        captures.require(texels > 2 && texels < 2.8,
                         "A shadow edge spans " + std::to_string(texels) + " texels of its cascade, not about 2.4",
                         {"plates"});
    captures.require(std::abs(measured_ratio / stated_ratio - 1) < .2,
                     "The far edge widens by " + std::to_string(measured_ratio) +
                         " where the cascades' texels differ by " + std::to_string(stated_ratio),
                     {"plates"});

    // Moving the camera by less than the near cascade's texel keeps the cascade's texels, and so the edge, in place.
    const anima::Vec3 step{.37F * near_cascade.texel, 0, -.21F * near_cascade.texel};
    renderer.set_view(view(step));
    (void)draw("plates-moved");
    const auto moved = anima::fit_shadow_cascades(lighting(), view(step));
    require(moved.size() == cascades.size() && moved[0].radius == cascades[0].radius,
            "A camera moved by less than a texel changed its cascades' size");
    const auto moved_edge = measure("plates-moved", view(step), near_z, near_edge_x, near_cascade);
    const auto shift = std::abs(moved_edge.x50 - near_edge.x50) / ground_texel(near_cascade);
    std::cout << "CASCADES {\"subtexel_move\":" << .37 << ",\"edge_shift_texels\":" << shift << "}\n";
    captures.require(shift < .2,
                     "Moving the camera by part of a texel moved a shadow edge by " + std::to_string(shift) + " texels",
                     {"plates", "plates-moved"});
    captures.discard({"plates", "plates-moved"});

    // A wall 1 m tall along -Z, whose shadow edge crosses every seam between cascades, inside and outside their blends,
    // under cascades of 2048 texels. The biases, in texels of each cascade, lift the shadow off the wall's foot by a
    // few texels, which at 512 texels would leave the farthest cascade no fully shadowed ground in the wall's 1.7 m
    // wide shadow.
    const auto wall_lighting = lighting(4, 2048);
    const auto wall_cascades = anima::fit_shadow_cascades(wall_lighting, view());
    auto wall = std::make_shared<anima::Scene>();
    (void)wall->add(ground);
    {
        const auto mesh = anima::Mesh::compile(*environment_test::box_fixture({.05F, .5F, 44}));
        const auto id = wall->add(mesh);
        auto world = anima::identity();
        anima::set_translation(world, {0, .5F, -46});
        wall->set_pose(id, mesh->rest_pose(), world);
    }
    renderer.set_scenes({wall});
    renderer.set_environment(wall_lighting);
    renderer.set_view(view());
    (void)draw("wall");
    // The wall's shadow on the ground reaches from its foot at x = -0.05 to its top edge's shadow; each measurement
    // keeps its shadowed end short of the foot.
    const auto wall_edge_x = shadow_x(-.05, 1), wall_shadow = -.05 - wall_edge_x;
    const auto forward = anima::normalized(target - eye);
    double worst = 0;
    std::size_t seams = 0;
    for (std::size_t i = 0; i + 1 < wall_cascades.size(); ++i) {
        const auto split = double(wall_cascades[i].end), band = .1 * (split - wall_cascades[i].begin);
        for (const auto depth : {split - 2 * band, split - band * .5, split + band * .5}) {
            // The ground point straight ahead at this view depth: depth = dot((0, -eye.y, z - eye.z), forward).
            const auto at = double(eye.z) + (depth + double(eye.y) * forward.y) / double(forward.z);
            const auto &cascade = holding(wall_cascades, at);
            const auto edge = measure("wall", view(), at, wall_edge_x, cascade, .9 * wall_shadow);
            // Each texel holds the wall's silhouette at its center, which may lie half a texel from the true edge.
            const auto error = std::abs(edge.x50 - wall_edge_x) / ground_texel(cascade);
            worst = std::max(worst, error);
            ++seams;
            captures.require(error < .75,
                             "The wall's shadow edge lies " + std::to_string(error) + " texels from its place at " +
                                 std::to_string(at) + " m",
                             {"wall"});
        }
    }
    std::cout << "CASCADES {\"seam_samples\":" << seams << ",\"worst_edge_error_texels\":" << worst << "}\n";
    captures.discard({"wall"});

    // A plate 40 m toward the sun from the ground ahead, outside the view and beyond every cascade's sphere, still
    // shadows that ground: the first cascade's depth extends toward the sun over it.
    {
        constexpr anima::Vec3 shaded{0, 0, -4.5F}, lit{-2, 0, -4.5F};
        const auto toward_sun = anima::normalized(sun);
        auto overhead = std::make_shared<anima::Scene>();
        (void)overhead->add(ground);
        const auto mesh = anima::Mesh::compile(*environment_test::box_fixture({.5F, .02F, .5F}));
        const auto id = overhead->add(mesh);
        auto world = anima::identity();
        anima::set_translation(world, shaded + toward_sun * 40.F);
        overhead->set_pose(id, mesh->rest_pose(), world);
        renderer.set_scenes({overhead});
        renderer.set_environment(lighting());
        renderer.set_view(view());
        (void)draw("overhead");
        const auto &first = cascades.front();
        captures.require(anima::length(anima::Vec3{world[12], world[13], world[14]} - first.center) > first.radius + 1,
                         "The overhead plate lies within the first cascade's sphere", {"overhead"});
        const auto at = [&](anima::Vec3 point) {
            return luminance(captures["overhead"], environment_test::project(view(), point, captures["overhead"].width,
                                                                             captures["overhead"].height));
        };
        const auto shadowed = at(shaded), sunlit = at(lit);
        std::cout << "CASCADES {\"overhead_shadowed\":" << shadowed << ",\"overhead_lit\":" << sunlit << "}\n";
        captures.require(shadowed < .5 * sunlit,
                         "A caster toward the sun beyond the first cascade's sphere left the ground lit: " +
                             std::to_string(shadowed) + " against " + std::to_string(sunlit),
                         {"overhead"});
        captures.discard({"overhead"});
    }

    // The cost of each count of cascades over 10,000 copies, as medians of 60 frames after 10 more.
    const auto copies = placements_test::field(100);
    const auto shape = placements_test::pyramid();
    auto field = std::make_shared<anima::Scene>();
    (void)field->add(anima::Mesh::compile(*environment_test::box_fixture({200, 0, 200}, true)));
    field->create("field", shape).renderer().set_placements(anima::MeshPlacements::create(shape, copies));
    renderer.set_scenes({field});
    {
        using anima::operator*;
        renderer.set_view(anima::perspective(16.F / 9, .1F, 400) * anima::look_at({-4, 2, -4}, {150, 0, 150}));
    }
    for (std::uint32_t count = 1; count <= 4; ++count) {
        auto environment = lighting(count, 2048);
        environment.shadow_cascades.distance = 150;
        renderer.set_environment(environment);
        std::vector<double> shadow_ms, gpu_ms;
        for (int frame = 0; frame < 70; ++frame) {
            present();
            const auto profile = renderer.frame_profile();
            if (frame >= 10 && profile.gpu_available) {
                shadow_ms.push_back(profile.gpu_shadow_ms);
                gpu_ms.push_back(profile.gpu_ms);
            }
        }
        const auto median = [](std::vector<double> values) {
            if (values.empty())
                return -1.0;
            std::nth_element(values.begin(), values.begin() + std::ptrdiff_t(values.size() / 2), values.end());
            return values[values.size() / 2];
        };
        const auto stats = renderer.resource_stats();
        require(stats.shadow_draw_calls > 0, "The cascades drew no casters");
        std::cout << "BENCH {\"cascades\":" << count << ",\"copies\":" << copies.size()
                  << ",\"shadow_draw_calls\":" << stats.shadow_draw_calls
                  << ",\"shadow_submitted_indices\":" << stats.shadow_submitted_indices
                  << ",\"shadow_ms\":" << median(shadow_ms) << ",\"gpu_ms\":" << median(gpu_ms) << "}\n";
    }

    const auto stats = renderer.shutdown();
    require(!stats.validation_errors && !stats.validation_warnings, "Cascade validation failed");
    std::cout
        << "PASS shadow cascades: edges span their cascades' stated texels near and far, stay put under sub-texel "
           "camera motion and across seams; validation_warnings=0 validation_errors=0\n";
    return 0;
}
} // namespace cascades_test
