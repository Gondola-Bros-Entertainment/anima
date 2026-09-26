#include "near.hpp"
#include <anima/core/triangle_query.hpp>
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <vector>

using namespace anima;
namespace {
constexpr auto invalid_segment = "Triangle query requires finite segment and nonnegative radius";
constexpr QueryTriangle face{{0, 0, 0}, {4, 0, 0}, {0, 4, 0}};
// Contact fractions compare within this. CHECK_MESSAGE takes its condition as one macro argument, so each Near in
// a condition is parenthesized: its comma stays inside the argument, and the comparison still shows both values.
constexpr double tolerance = 1e-6;
// The contact fraction of a hit; a miss throws std::bad_optional_access, which fails the enclosing check.
double fraction(const std::optional<TriangleHit> &hit) { return hit.value().fraction; }
// Independent plane/barycentric reference for noncoplanar rays in the BVH sweep.
std::optional<double> reference(QueryTriangle triangle, Vec3 from, Vec3 to) {
    const auto a = triangle.a, b = triangle.b, c = triangle.c, ray = to - from;
    const auto n = cross(b - a, c - a);
    const double speed = dot(n, ray);
    if (std::abs(speed) < 1e-9)
        return {};
    const double t = dot(n, a - from) / speed;
    if (t < 0 || t > 1)
        return {};
    const auto p = from + ray * float(t), v0 = b - a, v1 = c - a, v2 = p - a;
    const double d00 = dot(v0, v0), d01 = dot(v0, v1), d11 = dot(v1, v1), d20 = dot(v2, v0), d21 = dot(v2, v1);
    const double denominator = d00 * d11 - d01 * d01;
    if (denominator == 0)
        return {};
    const double u = (d11 * d20 - d01 * d21) / denominator, v = (d00 * d21 - d01 * d20) / denominator;
    return u >= 0 && v >= 0 && u + v <= 1 ? std::optional{t} : std::nullopt;
}
} // namespace

TEST_CASE("Rays and spheres contact a face from either side, its edges and its vertices") {
    const TriangleQuery query(std::span{&face, 1});
    CHECK_MESSAGE(fraction(query.intersect({1, 1, 2}, {1, 1, -2})) == (Near{.5, tolerance}), "Front face ray missed");
    CHECK_MESSAGE(fraction(query.intersect({1, 1, -2}, {1, 1, 2})) == (Near{.5, tolerance}), "Back face ray missed");
    CHECK_MESSAGE(fraction(query.intersect({1, 1, 2}, {1, 1, -2}, {.5F})) == (Near{.375, tolerance}),
                  "Sphere face contact is wrong");
    CHECK_MESSAGE(fraction(query.intersect({1, 1, -2}, {1, 1, 2}, {.5F})) == (Near{.375, tolerance}),
                  "Sphere back face contact is wrong");
    CHECK_FALSE_MESSAGE(query.intersect({3, 3, 2}, {3, 3, -2}), "Empty part of triangle bounds blocked a ray");
    CHECK_FALSE_MESSAGE(query.intersect({3, 3, 2}, {3, 3, -2}, {.2F}),
                        "Empty part of triangle bounds blocked a sphere");
    CHECK_MESSAGE(fraction(query.intersect({2, -.3F, 2}, {2, -.3F, -2}, {.5F})) == (Near{.4, tolerance}),
                  "Sphere missed the edge cylinder");
    CHECK_MESSAGE(fraction(query.intersect({-.3F, -.4F, 2}, {-.3F, -.4F, -2}, {1})) ==
                      (Near{(2 - std::sqrt(.75)) / 4, tolerance}),
                  "Sphere missed a rounded vertex");
}

TEST_CASE("An initial overlap is reported unless the policy ignores that triangle") {
    const TriangleQuery query(std::span{&face, 1});
    CHECK_MESSAGE(fraction(query.intersect({1, 1, .1F}, {1, 1, 2}, {.2F})) == (Near{0, tolerance}),
                  "Initial overlap was ignored");
    CHECK_FALSE_MESSAGE(query.intersect({1, 1, .1F}, {1, 1, 2}, {.2F, InitialOverlap::ignore}),
                        "Explicit initial-overlap policy retained the starting surface");
    const std::array overlap_and_wall{face, QueryTriangle{{0, 0, 1}, {4, 0, 1}, {0, 4, 1}}};
    CHECK_MESSAGE(
        fraction(TriangleQuery(overlap_and_wall).intersect({1, 1, .1F}, {1, 1, 2}, {.2F, InitialOverlap::ignore})) ==
            (Near{(.8 - .1) / 1.9, tolerance}),
        "Ignoring initial overlap also ignored another wall in the same mesh");
}

TEST_CASE("Stationary and endpoint contacts are found") {
    const TriangleQuery query(std::span{&face, 1});
    CHECK_MESSAGE(fraction(query.intersect({1, 1, 0}, {1, 1, 0})) == (Near{0, tolerance}),
                  "Stationary point on surface missed");
    CHECK_FALSE_MESSAGE(query.intersect({1, 1, 2}, {1, 1, 2}, {.2F}), "Stationary separated sphere hit");
    CHECK_MESSAGE(fraction(query.intersect({1, 1, 2}, {1, 1, 0})) == (Near{1, tolerance}), "Endpoint contact missed");
}

TEST_CASE("Degenerate triangles collide as segments and points, and an empty query as nothing") {
    const QueryTriangle line{{0, 0, 0}, {0, 0, 2}, {0, 0, 2}};
    const TriangleQuery degenerate(std::span{&line, 1});
    CHECK_MESSAGE(fraction(degenerate.intersect({2, 0, 1}, {-2, 0, 1}, {.5F})) == (Near{.375, tolerance}),
                  "Degenerate triangle edge sweep missed");
    const QueryTriangle point{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    CHECK_MESSAGE(fraction(TriangleQuery(std::span{&point, 1}).intersect({2, 0, 0}, {-2, 0, 0}, {.5F})) ==
                      (Near{.375, tolerance}),
                  "Degenerate point sweep missed");
    CHECK_FALSE_MESSAGE(TriangleQuery({}).intersect({0, 0, 0}, {1, 1, 1}), "Empty query hit");
}

TEST_CASE("Invalid queries are rejected before they change statistics") {
    const TriangleQuery query(std::span{&face, 1});
    const TriangleQuery empty({});
    for (const auto *selection : std::array<const TriangleQuery *, 2>{&query, &empty})
        for (const auto policy : {static_cast<InitialOverlap>(-1), static_cast<InitialOverlap>(2)}) {
            CAPTURE(selection == &empty);
            CAPTURE(policy);
            TriangleQueryStats unchanged{7, 9};
            CHECK_THROWS_WITH_AS(selection->intersect({1, 1, .1F}, {1, 1, 2}, {.2F, policy}, &unchanged),
                                 "Unknown triangle initial-overlap policy", std::invalid_argument);
            CHECK(unchanged.bounds_tested == 7u);
            CHECK(unchanged.triangles_tested == 9u);
        }
    CHECK_THROWS_WITH_AS(query.intersect({0, 0, 0}, {1, 1, 1}, {-1}), invalid_segment, std::invalid_argument);
    CHECK_THROWS_WITH_AS(query.intersect({std::numeric_limits<float>::quiet_NaN(), 0, 0}, {1, 1, 1}), invalid_segment,
                         std::invalid_argument);
}

TEST_CASE("The BVH visits only nearby triangles and agrees with an independent reference") {
    std::vector<QueryTriangle> triangles;
    for (unsigned z = 0; z < 40; ++z)
        for (unsigned x = 0; x < 40; ++x) {
            const float a = float(x) * 3, b = float(z) * 3;
            triangles.push_back({{a, b, 0}, {a + 2, b, 0}, {a, b + 2, 0}});
        }
    const TriangleQuery field(triangles);
    TriangleQueryStats stats;
    CHECK_MESSAGE(fraction(field.intersect({30.25F, 30.25F, 2}, {30.25F, 30.25F, -2}, {0}, &stats)) ==
                      (Near{.5, tolerance}),
                  "BVH omitted a triangle");
    CHECK_MESSAGE(stats.triangles_tested < triangles.size() / 10, "BVH query scanned unrelated geometry");
    std::mt19937 rng(4151);
    std::uniform_real_distribution<float> coordinate(-5, 125);
    for (unsigned sample = 0; sample < 400; ++sample) {
        CAPTURE(sample);
        const Vec3 start{coordinate(rng), coordinate(rng), 3}, end{coordinate(rng), coordinate(rng), -3};
        std::optional<double> expected;
        for (const auto triangle : triangles)
            if (const auto hit = reference(triangle, start, end); hit && (!expected || *hit < *expected))
                expected = hit;
        const auto actual = field.intersect(start, end);
        CHECK_MESSAGE(actual.has_value() == expected.has_value(), "BVH disagrees with independent ray reference");
        if (actual && expected)
            CHECK_MESSAGE(actual->fraction == (Near{*expected, tolerance}), "BVH returned a later contact");
    }
}

TEST_CASE("The nearest of stacked triangles is reported with its source index") {
    const std::array stacked{QueryTriangle{{0, 0, -1}, {4, 0, -1}, {0, 4, -1}}, face};
    const auto nearest = TriangleQuery(stacked).intersect({1, 1, 2}, {1, 1, -2});
    REQUIRE(nearest);
    CHECK_MESSAGE(nearest->fraction == (Near{.5, tolerance}), "Query did not select nearest triangle");
    CHECK_MESSAGE(nearest->triangle == 1u, "Query changed source triangle identity");
}
