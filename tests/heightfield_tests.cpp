#include "near.hpp"
#include <anima/core/heightfield.hpp>
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <vector>

using namespace anima;
namespace {
constexpr auto invalid_dimensions = "Invalid heightfield dimensions";
constexpr double tolerance = 1e-5;          // Heights, normals and query results.
constexpr double analytic_tolerance = 1e-8; // Double-precision fractions and distances on a planar ramp.
constexpr double oracle_tolerance = 2e-5;   // Fractions compared with the float triangle oracle.
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
} // namespace

TEST_CASE("Samples come from the triangle under a point and never from outside the grid") {
    const Heightfield field{2, 2, -2, -3, 2, 4, {0, 2, 4, 8}};
    CHECK_NOTHROW(validate_heightfield(field.view()));
    const auto height = [&](float x, float z) { return sample_heightfield(field.view(), x, z).value().height; };
    CHECK(height(-1, -2) == Near{2.5, tolerance});
    CHECK(height(-1.5F, 0) == Near{4, tolerance});
    CHECK(height(0, 1) == Near{8, tolerance});
    CHECK_FALSE_MESSAGE(sample_heightfield(field.view(), -2.01F, 0), "Outside grid silently clamped");
    CHECK_FALSE_MESSAGE(sample_heightfield(field.view(), std::numeric_limits<float>::quiet_NaN(), 0),
                        "Nonfinite sample accepted");
    const auto n = sample_heightfield(field.view(), -1, -2).value().normal;
    CHECK(length(n) == Near{1, tolerance});
    CHECK_MESSAGE(n.y > 0, "Terrain normal points below ground");
}

TEST_CASE("Segments hit the surface, or a clearance above it, and miss clear or outside paths") {
    const Heightfield field{2, 2, -2, -3, 2, 4, {0, 2, 4, 8}};
    auto hit = intersect_heightfield(field.view(), {-1, 10, -2}, {-1, -10, -2});
    REQUIRE_MESSAGE(hit, "Vertical ray missed terrain");
    CHECK(hit->fraction == Near{.375, tolerance});
    CHECK(hit->position.y == Near{2.5, tolerance});
    hit = intersect_heightfield(field.view(), {-1, 10, -2}, {-1, -10, -2}, .5F);
    REQUIRE(hit);
    CHECK(hit->position.y == Near{3, tolerance});
    CHECK_FALSE_MESSAGE(intersect_heightfield(field.view(), {-5, 20, -2}, {5, 20, -2}), "Clear ray hit terrain");
    CHECK_FALSE_MESSAGE(intersect_heightfield(field.view(), {-5, 0, -8}, {5, 0, -8}), "Outside ray hit terrain");
    // A segment that starts below the surface hits it at once.
    CHECK(intersect_heightfield(field.view(), {-1, -1, -2}, {-1, 10, -2}).value().fraction == Near{0, tolerance});
}

TEST_CASE("Motion follows a walkable slope and stops at a steep initial triangle") {
    const Heightfield ramp{3, 2, 0, 0, 1, 1, {0, 1, 2, 0, 1, 2}};
    auto motion = move_on_heightfield(ramp.view(), {0, 0, .5F}, {2, 0, .5F}, 1, .7F);
    CHECK(motion.distance == Near{1, tolerance});
    CHECK(motion.position.x == Near{1 / std::sqrt(2.0), tolerance});
    CHECK(motion.position.y == Near{motion.position.x, tolerance});
    CHECK_FALSE_MESSAGE(motion.blocked, "Walkable slope was blocked");
    motion = move_on_heightfield(ramp.view(), {0, 0, .5F}, {2, 0, .5F}, 10, .8F);
    CHECK_MESSAGE(motion.blocked, "Steep initial triangle was allowed");
    CHECK(motion.fraction == 0);
}

TEST_CASE("A narrow steep ridge blocks motion and sightlines from either side") {
    const Heightfield ridge{5, 2, 0, 0, 1, 1, {0, 0, 3, 0, 0, 0, 0, 3, 0, 0}};
    for (const bool reverse : {false, true}) {
        CAPTURE(reverse);
        const Vec3 from{reverse ? 3.5F : .5F, 0, .5F}, to{reverse ? .5F : 3.5F, 0, .5F};
        const auto motion = move_on_heightfield(ridge.view(), from, to, 10, .8F);
        CHECK_MESSAGE(motion.blocked, "Narrow steep ridge was skipped");
        CHECK(motion.fraction == Near{1.0 / 6, tolerance});
        const auto hit = intersect_heightfield(ridge.view(), from + Vec3{0, 1, 0}, to + Vec3{0, 1, 0});
        REQUIRE_MESSAGE(hit, "Interior ridge did not obstruct sightline");
        CHECK(hit->fraction == Near{5.0 / 18, tolerance});
    }
}

TEST_CASE("A zero-length move stays on the surface at vertices and edges") {
    const Heightfield ramp{3, 2, 0, 0, 1, 1, {0, 1, 2, 0, 1, 2}};
    for (const auto point : {Vec3{0, 0, 0}, Vec3{2, 0, 1}, Vec3{1, 0, .5F}}) {
        CAPTURE(point.x);
        CAPTURE(point.z);
        const auto motion = move_on_heightfield(ramp.view(), point, point, 0);
        CHECK(motion.position.y == Near{point.x, tolerance});
        CHECK(motion.distance == Near{0, tolerance});
    }
}

TEST_CASE("Invalid layouts, segments and motion are rejected") {
    const Heightfield ramp{3, 2, 0, 0, 1, 1, {0, 1, 2, 0, 1, 2}};
    CHECK_THROWS_WITH_AS(move_on_heightfield(ramp.view(), {-1, 0, 0}, {1, 0, 0}, 1),
                         "Terrain motion leaves the heightfield", std::invalid_argument);
    CHECK_THROWS_WITH_AS(move_on_heightfield(ramp.view(), {0, 0, 0}, {1, 0, 0}, -1), "Invalid terrain motion",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(intersect_heightfield(ramp.view(), {}, {}, -1), "Invalid terrain segment",
                         std::invalid_argument);
    auto bad = ramp;
    bad.heights.pop_back();
    CHECK_THROWS_WITH_AS(validate_heightfield(bad.view()), invalid_dimensions, std::invalid_argument);
    bad = ramp;
    bad.spacing_x = 0;
    CHECK_THROWS_WITH_AS(validate_heightfield(bad.view()), "Invalid heightfield coordinates", std::invalid_argument);
    bad = ramp;
    bad.heights[1] = std::numeric_limits<float>::infinity();
    CHECK_THROWS_WITH_AS(validate_heightfield(bad.view()), "Nonfinite terrain height", std::invalid_argument);
    bad = ramp;
    // Two rows of this many columns wrap around std::size_t to exactly the six heights, so only the overflow check
    // rejects them.
    bad.columns = std::numeric_limits<std::size_t>::max() / 2 + 4;
    CHECK_THROWS_WITH_AS(validate_heightfield(bad.view()), invalid_dimensions, std::invalid_argument);
}

TEST_CASE("Translated planar ramps keep analytic contacts and distances") {
    // A planar ramp has analytic contact and distance results regardless of
    // the cell diagonal. Its internal crossings need not fit float coordinates.
    for (const float origin : {-1e6F, 0.F, 1e6F})
        for (const bool reverse : {false, true}) {
            CAPTURE(origin);
            CAPTURE(reverse);
            const Heightfield field{
                2, 2, origin, 0, 1, 1, reverse ? std::vector<float>{1, 0, 1, 0} : std::vector<float>{0, 1, 0, 1}};
            const Vec3 from{origin + (reverse ? 1 : 0), 0, .3F}, to{origin + (reverse ? 0 : 1), 0, .4F};
            for (const float height : {.2F, .8F}) {
                CAPTURE(height);
                const auto hit =
                    intersect_heightfield(field.view(), from + Vec3{0, height, 0}, to + Vec3{0, height, 0});
                REQUIRE_MESSAGE(hit, "Translated planar ramp lost segment contact");
                CHECK(hit->fraction == Near{height, analytic_tolerance});
                CHECK(hit->position.y == Near{height, tolerance});
            }
            const auto distance = std::hypot(1.0, 1.0, double(to.z) - from.z);
            for (const double budget : {.2, .75, 2.0}) {
                CAPTURE(budget);
                const auto motion = move_on_heightfield(field.view(), from, to, budget);
                CHECK(motion.fraction == Near{std::min(1.0, budget / distance), analytic_tolerance});
                CHECK(motion.distance == Near{std::min(budget, distance), analytic_tolerance});
                CHECK(motion.position.y == Near{motion.fraction, tolerance});
                CHECK_FALSE_MESSAGE(motion.blocked, "Translated planar ramp blocked motion");
            }
        }
}

TEST_CASE("Grid traversal agrees with an independent triangle oracle") {
    std::mt19937 random(715);
    std::uniform_real_distribution<float> unit(0, 1);
    Heightfield mesh{8, 7, -3, -2, .75F, 1.25F, {}};
    for (unsigned i = 0; i < 56; ++i)
        mesh.heights.push_back(unit(random) * 2);
    for (unsigned trial = 0; trial < 200; ++trial) {
        CAPTURE(trial);
        const Vec3 from{-3 + unit(random) * 5.25F, 5, -2 + unit(random) * 7.5F};
        const Vec3 to{-3 + unit(random) * 5.25F, -3, -2 + unit(random) * 7.5F};
        std::optional<double> reference;
        const auto vertex = [&](std::size_t x, std::size_t z) {
            return Vec3{mesh.origin_x + float(x) * mesh.spacing_x, mesh.heights[z * mesh.columns + x],
                        mesh.origin_z + float(z) * mesh.spacing_z};
        };
        for (std::size_t z = 0; z + 1 < mesh.rows; ++z)
            for (std::size_t x = 0; x + 1 < mesh.columns; ++x)
                for (const auto triangle : {std::array<Vec3, 3>{vertex(x, z), vertex(x + 1, z + 1), vertex(x + 1, z)},
                                            std::array<Vec3, 3>{vertex(x, z), vertex(x, z + 1), vertex(x + 1, z + 1)}})
                    if (const auto t = triangle_hit(from, to, triangle[0], triangle[1], triangle[2]);
                        t && (!reference || *t < *reference))
                        reference = t;
        const auto actual = intersect_heightfield(mesh.view(), from, to);
        CHECK_MESSAGE(actual.has_value() == reference.has_value(), "Grid traversal disagrees with triangle oracle");
        if (actual && reference)
            CHECK(actual->fraction == Near{*reference, oracle_tolerance});
    }
}
