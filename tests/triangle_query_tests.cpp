#include <anima/core/triangle_query.hpp>
#include <iostream>
#include <limits>
#include <random>

using namespace anima;
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void near(const std::optional<TriangleHit> &hit, double fraction, const char *message) {
    require(hit && std::abs(hit->fraction - fraction) < 1e-6, message);
}
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
int main() {
    try {
        const QueryTriangle face{{0, 0, 0}, {4, 0, 0}, {0, 4, 0}};
        TriangleQuery query(std::span{&face, 1});
        near(query.intersect({1, 1, 2}, {1, 1, -2}), .5, "Front face ray missed");
        near(query.intersect({1, 1, -2}, {1, 1, 2}), .5, "Back face ray missed");
        near(query.intersect({1, 1, 2}, {1, 1, -2}, {.5F}), .375, "Sphere face contact is wrong");
        near(query.intersect({1, 1, -2}, {1, 1, 2}, {.5F}), .375, "Sphere back face contact is wrong");
        require(!query.intersect({3, 3, 2}, {3, 3, -2}), "Empty part of triangle bounds blocked a ray");
        require(!query.intersect({3, 3, 2}, {3, 3, -2}, {.2F}), "Empty part of triangle bounds blocked a sphere");
        near(query.intersect({2, -.3F, 2}, {2, -.3F, -2}, {.5F}), .4, "Sphere missed the edge cylinder");
        near(query.intersect({-.3F, -.4F, 2}, {-.3F, -.4F, -2}, {1}), (2 - std::sqrt(.75)) / 4,
             "Sphere missed a rounded vertex");
        near(query.intersect({1, 1, .1F}, {1, 1, 2}, {.2F}), 0, "Initial overlap was ignored");
        require(!query.intersect({1, 1, .1F}, {1, 1, 2}, {.2F, InitialOverlap::ignore}),
                "Explicit initial-overlap policy retained the starting surface");
        const std::array overlap_and_wall{face, QueryTriangle{{0, 0, 1}, {4, 0, 1}, {0, 4, 1}}};
        near(TriangleQuery(overlap_and_wall).intersect({1, 1, .1F}, {1, 1, 2}, {.2F, InitialOverlap::ignore}),
             (.8 - .1) / 1.9, "Ignoring initial overlap also ignored another wall in the same mesh");
        near(query.intersect({1, 1, 0}, {1, 1, 0}), 0, "Stationary point on surface missed");
        require(!query.intersect({1, 1, 2}, {1, 1, 2}, {.2F}), "Stationary separated sphere hit");
        near(query.intersect({1, 1, 2}, {1, 1, 0}), 1, "Endpoint contact missed");
        const QueryTriangle line{{0, 0, 0}, {0, 0, 2}, {0, 0, 2}};
        TriangleQuery degenerate(std::span{&line, 1});
        near(degenerate.intersect({2, 0, 1}, {-2, 0, 1}, {.5F}), .375, "Degenerate triangle edge sweep missed");
        const QueryTriangle point{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
        near(TriangleQuery(std::span{&point, 1}).intersect({2, 0, 0}, {-2, 0, 0}, {.5F}), .375,
             "Degenerate point sweep missed");
        require(!TriangleQuery({}).intersect({0, 0, 0}, {1, 1, 1}), "Empty query hit");
        const TriangleQuery empty({});
        for (const auto *selection : std::array<const TriangleQuery *, 2>{&query, &empty})
            for (const auto policy : {static_cast<InitialOverlap>(-1), static_cast<InitialOverlap>(2)}) {
                bool invalid_policy = false;
                TriangleQueryStats unchanged{7, 9};
                try {
                    (void)selection->intersect({1, 1, .1F}, {1, 1, 2}, {.2F, policy}, &unchanged);
                } catch (const std::invalid_argument &) {
                    invalid_policy = true;
                }
                require(invalid_policy, "Unknown initial-overlap policy silently changed collision behavior");
                require(unchanged.bounds_tested == 7 && unchanged.triangles_tested == 9,
                        "Invalid initial-overlap policy changed query statistics");
            }
        bool rejected = false;
        try {
            (void)query.intersect({0, 0, 0}, {1, 1, 1}, {-1});
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        require(rejected, "Negative radius accepted");
        rejected = false;
        try {
            (void)query.intersect({std::numeric_limits<float>::quiet_NaN(), 0, 0}, {1, 1, 1});
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        require(rejected, "Nonfinite segment accepted");
        std::vector<QueryTriangle> triangles;
        for (unsigned z = 0; z < 40; ++z)
            for (unsigned x = 0; x < 40; ++x) {
                const float a = float(x) * 3, b = float(z) * 3;
                triangles.push_back({{a, b, 0}, {a + 2, b, 0}, {a, b + 2, 0}});
            }
        const TriangleQuery field(triangles);
        TriangleQueryStats stats;
        near(field.intersect({30.25F, 30.25F, 2}, {30.25F, 30.25F, -2}, {0}, &stats), .5, "BVH omitted a triangle");
        require(stats.triangles_tested < triangles.size() / 10, "BVH query scanned unrelated geometry");
        std::mt19937 rng(4151);
        std::uniform_real_distribution<float> coordinate(-5, 125);
        for (unsigned sample = 0; sample < 400; ++sample) {
            const Vec3 start{coordinate(rng), coordinate(rng), 3}, end{coordinate(rng), coordinate(rng), -3};
            std::optional<double> expected;
            for (const auto triangle : triangles)
                if (const auto hit = reference(triangle, start, end); hit && (!expected || *hit < *expected))
                    expected = hit;
            const auto actual = field.intersect(start, end);
            require(bool(actual) == bool(expected), "BVH disagrees with independent ray reference");
            if (expected)
                near(actual, *expected, "BVH returned a later contact");
        }
        const std::array stacked{QueryTriangle{{0, 0, -1}, {4, 0, -1}, {0, 4, -1}}, face};
        const auto nearest = TriangleQuery(stacked).intersect({1, 1, 2}, {1, 1, -2});
        near(nearest, .5, "Query did not select nearest triangle");
        require(nearest->triangle == 1, "Query changed source triangle identity");
        std::cout << "PASS triangle queries: surfaces, edges, vertices, overlap, degenerate geometry, "
                     "validation, nearest hit and BVH reference parity\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
