#include <anima/core/heightfield.hpp>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void near(double a, double b, double tolerance = 1e-5) {
    require(std::abs(a - b) < tolerance, "Terrain numeric mismatch");
}
template <class F> void invalid(F action) {
    try {
        action();
    } catch (const std::invalid_argument &) {
        return;
    }
    throw std::runtime_error("Invalid terrain input accepted");
}
// Independent triangle intersection oracle, deliberately unaware of grid traversal.
std::optional<double> triangle_hit(anima::Vec3 from, anima::Vec3 to, anima::Vec3 a, anima::Vec3 b, anima::Vec3 c) {
    const auto ray = to - from, e1 = b - a, e2 = c - a;
    const auto p = anima::cross(ray, e2);
    const double determinant = anima::dot(e1, p);
    if (std::abs(determinant) < 1e-8)
        return {};
    const auto offset = from - a;
    const double u = anima::dot(offset, p) / determinant;
    const auto q = anima::cross(offset, e1);
    const double v = anima::dot(ray, q) / determinant;
    const double t = anima::dot(e2, q) / determinant;
    if (u >= 0 && v >= 0 && u + v <= 1 && t >= 0 && t <= 1)
        return t;
    return {};
}
void translated_queries() {
    using namespace anima;
    // A planar ramp has analytic contact and distance results regardless of
    // the cell diagonal. Its internal crossings need not fit float coordinates.
    for (const float origin : {-1e6F, 0.F, 1e6F})
        for (const bool reverse : {false, true}) {
            const Heightfield field{
                2, 2, origin, 0, 1, 1, reverse ? std::vector<float>{1, 0, 1, 0} : std::vector<float>{0, 1, 0, 1}};
            const Vec3 from{origin + (reverse ? 1 : 0), 0, .3F}, to{origin + (reverse ? 0 : 1), 0, .4F};
            for (const float height : {.2F, .8F}) {
                const auto hit =
                    intersect_heightfield(field.view(), from + Vec3{0, height, 0}, to + Vec3{0, height, 0});
                require(bool(hit), "Translated planar ramp lost segment contact");
                near(hit->fraction, height, 1e-8);
                near(hit->position.y, height);
            }
            const auto distance = std::hypot(1.0, 1.0, double(to.z) - from.z);
            for (const double budget : {.2, .75, 2.0}) {
                const auto motion = move_on_heightfield(field.view(), from, to, budget);
                near(motion.fraction, std::min(1.0, budget / distance), 1e-8);
                near(motion.distance, std::min(budget, distance), 1e-8);
                near(motion.position.y, motion.fraction);
                require(!motion.blocked, "Translated planar ramp blocked motion");
            }
        }
}
} // namespace
int main() {
    using namespace anima;
    try {
        Heightfield field{2, 2, -2, -3, 2, 4, {0, 2, 4, 8}};
        validate_heightfield(field.view());
        near(sample_heightfield(field.view(), -1, -2)->height, 2.5);
        near(sample_heightfield(field.view(), -1.5F, 0)->height, 4);
        near(sample_heightfield(field.view(), 0, 1)->height, 8);
        require(!sample_heightfield(field.view(), -2.01F, 0), "Outside grid silently clamped");
        require(!sample_heightfield(field.view(), std::numeric_limits<float>::quiet_NaN(), 0),
                "Nonfinite sample accepted");
        const auto n = sample_heightfield(field.view(), -1, -2)->normal;
        near(length(n), 1);
        require(n.y > 0, "Terrain normal points below ground");
        auto hit = intersect_heightfield(field.view(), {-1, 10, -2}, {-1, -10, -2});
        require(bool(hit), "Vertical ray missed terrain");
        near(hit->fraction, .375);
        near(hit->position.y, 2.5);
        hit = intersect_heightfield(field.view(), {-1, 10, -2}, {-1, -10, -2}, .5F);
        near(hit->position.y, 3);
        require(!intersect_heightfield(field.view(), {-5, 20, -2}, {5, 20, -2}), "Clear ray hit terrain");
        require(!intersect_heightfield(field.view(), {-5, 0, -8}, {5, 0, -8}), "Outside ray hit terrain");
        near(intersect_heightfield(field.view(), {-1, -1, -2}, {-1, 10, -2})->fraction, 0);
        Heightfield ramp{3, 2, 0, 0, 1, 1, {0, 1, 2, 0, 1, 2}};
        auto motion = move_on_heightfield(ramp.view(), {0, 0, .5F}, {2, 0, .5F}, 1, .7F);
        near(motion.distance, 1);
        near(motion.position.x, 1 / std::sqrt(2.0));
        near(motion.position.y, motion.position.x);
        require(!motion.blocked, "Walkable slope was blocked");
        motion = move_on_heightfield(ramp.view(), {0, 0, .5F}, {2, 0, .5F}, 10, .8F);
        require(motion.blocked && motion.fraction == 0, "Steep initial triangle was allowed");
        Heightfield ridge{5, 2, 0, 0, 1, 1, {0, 0, 3, 0, 0, 0, 0, 3, 0, 0}};
        for (const bool reverse : {false, true}) {
            const Vec3 from{reverse ? 3.5F : .5F, 0, .5F}, to{reverse ? .5F : 3.5F, 0, .5F};
            motion = move_on_heightfield(ridge.view(), from, to, 10, .8F);
            require(motion.blocked, "Narrow steep ridge was skipped");
            near(motion.fraction, 1.0 / 6);
            hit = intersect_heightfield(ridge.view(), from + Vec3{0, 1, 0}, to + Vec3{0, 1, 0});
            require(bool(hit), "Interior ridge did not obstruct sightline");
            near(hit->fraction, 5.0 / 18);
        }
        for (const auto point : {Vec3{0, 0, 0}, Vec3{2, 0, 1}, Vec3{1, 0, .5F}}) {
            motion = move_on_heightfield(ramp.view(), point, point, 0);
            near(motion.position.y, point.x);
            near(motion.distance, 0);
        }
        invalid([&] { (void)move_on_heightfield(ramp.view(), {-1, 0, 0}, {1, 0, 0}, 1); });
        invalid([&] { (void)move_on_heightfield(ramp.view(), {0, 0, 0}, {1, 0, 0}, -1); });
        invalid([&] { (void)intersect_heightfield(ramp.view(), {}, {}, -1); });
        auto bad = ramp;
        bad.heights.pop_back();
        invalid([&] { validate_heightfield(bad.view()); });
        bad = ramp;
        bad.spacing_x = 0;
        invalid([&] { validate_heightfield(bad.view()); });
        bad = ramp;
        bad.heights[1] = std::numeric_limits<float>::infinity();
        invalid([&] { validate_heightfield(bad.view()); });
        bad = ramp;
        bad.columns = std::numeric_limits<std::size_t>::max();
        invalid([&] { validate_heightfield(bad.view()); });

        translated_queries();
        std::mt19937 random(715);
        std::uniform_real_distribution<float> unit(0, 1);
        Heightfield mesh{8, 7, -3, -2, .75F, 1.25F, {}};
        for (unsigned i = 0; i < 56; ++i)
            mesh.heights.push_back(unit(random) * 2);
        for (unsigned trial = 0; trial < 200; ++trial) {
            const Vec3 from{-3 + unit(random) * 5.25F, 5, -2 + unit(random) * 7.5F};
            const Vec3 to{-3 + unit(random) * 5.25F, -3, -2 + unit(random) * 7.5F};
            std::optional<double> reference;
            const auto vertex = [&](std::size_t x, std::size_t z) {
                return Vec3{mesh.origin_x + float(x) * mesh.spacing_x, mesh.heights[z * mesh.columns + x],
                            mesh.origin_z + float(z) * mesh.spacing_z};
            };
            for (std::size_t z = 0; z + 1 < mesh.rows; ++z)
                for (std::size_t x = 0; x + 1 < mesh.columns; ++x)
                    for (const auto triangle :
                         {std::array<Vec3, 3>{vertex(x, z), vertex(x + 1, z + 1), vertex(x + 1, z)},
                          std::array<Vec3, 3>{vertex(x, z), vertex(x, z + 1), vertex(x + 1, z + 1)}})
                        if (const auto t = triangle_hit(from, to, triangle[0], triangle[1], triangle[2]);
                            t && (!reference || *t < *reference))
                            reference = t;
            const auto actual = intersect_heightfield(mesh.view(), from, to);
            require(bool(actual) == bool(reference), "Grid traversal disagrees with triangle oracle");
            if (actual)
                near(actual->fraction, *reference, 2e-5);
        }
        std::cout << "PASS terrain sampling, triangle normals, edges, budgeted slopes, narrow ridges, translated "
                     "queries and 200 "
                     "independent ray checks\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
