#include <anima/core/triangle_query.hpp>
#include <limits>
#include <numeric>
#include <utility>

namespace anima {
namespace {
// Double intermediates keep short contacts useful at ordinary float world coordinates.
struct D3 {
    double x{}, y{}, z{};
};
D3 wide(Vec3 v) { return {v.x, v.y, v.z}; }
D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 operator*(D3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool inside(D3 p, D3 a, D3 b, D3 c, D3 n) {
    return dot(cross(b - a, p - a), n) >= 0 && dot(cross(c - b, p - b), n) >= 0 && dot(cross(a - c, p - c), n) >= 0;
}
double edge_distance_squared(D3 p, D3 a, D3 b) {
    const auto edge = b - a;
    const double square = dot(edge, edge);
    const auto offset = p - (a + edge * (square > 0 ? std::clamp(dot(p - a, edge) / square, 0.0, 1.0) : 0));
    return dot(offset, offset);
}
// Roots of |offset + velocity*t|^2 = radius^2. The edge caller additionally
// checks the finite edge interval; its endpoint spheres supply the rounded caps.
template <class Accept> void roots(D3 offset, D3 velocity, double radius, Accept accept) {
    const double a = dot(velocity, velocity), b = dot(offset, velocity), c = dot(offset, offset) - radius * radius;
    if (a == 0)
        return;
    const double discriminant = b * b - a * c;
    if (discriminant < 0)
        return;
    const double root = std::sqrt(discriminant);
    // Avoid cancellation when one root lies very close to the start.
    const double q = -b - std::copysign(root, b);
    double first = q / a, second = q != 0 ? c / q : first;
    if (first > second)
        std::swap(first, second);
    accept(first);
    accept(second);
}
std::optional<double> contact(const QueryTriangle &triangle, D3 from, D3 delta, double radius, double limit,
                              InitialOverlap initial_overlap) {
    const auto a = wide(triangle.a), b = wide(triangle.b), c = wide(triangle.c);
    const auto n = cross(b - a, c - a);
    const double square = dot(n, n), height = dot(from - a, n);
    double closest = std::min(
        {edge_distance_squared(from, a, b), edge_distance_squared(from, b, c), edge_distance_squared(from, c, a)});
    if (square > 0 && inside(from - n * (height / square), a, b, c, n))
        closest = std::min(closest, height * height / square);
    if (closest <= radius * radius)
        return initial_overlap == InitialOverlap::report ? std::optional{0.0} : std::nullopt;
    std::optional<double> result;
    const auto accept = [&](double t) {
        if (t >= 0 && t <= limit) {
            limit = t;
            result = t;
        }
    };
    const double speed = dot(delta, n);
    if (square > 0 && speed != 0) {
        const double offset = radius * std::sqrt(square);
        for (const double side : {-offset, offset}) {
            const double t = (side - height) / speed;
            if (t >= 0 && t <= limit && inside(from + delta * t - n * (side / square), a, b, c, n))
                accept(t);
        }
    }
    for (const auto &edge : std::array{std::pair{a, b}, std::pair{b, c}, std::pair{c, a}}) {
        const auto direction = edge.second - edge.first, offset = from - edge.first;
        const double length_squared = dot(direction, direction);
        if (length_squared > 0) {
            const double along = dot(offset, direction) / length_squared, rate = dot(delta, direction) / length_squared;
            roots(offset - direction * along, delta - direction * rate, radius, [&](double t) {
                const double parameter = along + rate * t;
                if (parameter >= 0 && parameter <= 1)
                    accept(t);
            });
        }
        roots(offset, delta, radius, accept);
    }
    return result;
}
bool bounds(Vec3 minimum, Vec3 maximum, D3 from, D3 delta, double radius, double limit) {
    double enter = 0, leave = limit;
    const double a[]{from.x, from.y, from.z}, d[]{delta.x, delta.y, delta.z};
    const double low[]{minimum.x - radius, minimum.y - radius, minimum.z - radius};
    const double high[]{maximum.x + radius, maximum.y + radius, maximum.z + radius};
    for (unsigned axis = 0; axis < 3; ++axis) {
        if (d[axis] == 0) {
            if (a[axis] < low[axis] || a[axis] > high[axis])
                return false;
        } else {
            auto first = (low[axis] - a[axis]) / d[axis], last = (high[axis] - a[axis]) / d[axis];
            if (first > last)
                std::swap(first, last);
            enter = std::max(enter, first);
            leave = std::min(leave, last);
            if (enter > leave)
                return false;
        }
    }
    return true;
}
} // namespace

TriangleQuery::TriangleQuery(std::span<const QueryTriangle> triangles)
    : triangles_(triangles.begin(), triangles.end()) {
    for (const auto &triangle : triangles_)
        if (!finite(triangle.a) || !finite(triangle.b) || !finite(triangle.c))
            throw std::invalid_argument("Triangle query requires finite vertices");
    order_.resize(triangles_.size());
    std::iota(order_.begin(), order_.end(), 0);
    if (!order_.empty())
        (void)build(0, order_.size());
}
std::size_t TriangleQuery::build(std::size_t first, std::size_t count) {
    const auto index = nodes_.size();
    nodes_.emplace_back();
    const float maximum = std::numeric_limits<float>::max();
    Node node{{maximum, maximum, maximum}, {-maximum, -maximum, -maximum}, first, count, 0, 0};
    for (std::size_t i = first; i < first + count; ++i) {
        const auto &t = triangles_[order_[i]];
        for (const auto p : {t.a, t.b, t.c}) {
            node.minimum = {std::min(node.minimum.x, p.x), std::min(node.minimum.y, p.y),
                            std::min(node.minimum.z, p.z)};
            node.maximum = {std::max(node.maximum.x, p.x), std::max(node.maximum.y, p.y),
                            std::max(node.maximum.z, p.z)};
        }
    }
    // Leaf size controls traversal overhead, not scene capacity. Median splits
    // bound recursion depth even for coincident or degenerate geometry.
    constexpr std::size_t leaf_triangles = 8;
    if (count > leaf_triangles) {
        const auto extent = node.maximum - node.minimum;
        const unsigned axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : extent.y >= extent.z ? 1 : 2;
        const auto centroid = [&](std::size_t i) {
            const auto &t = triangles_[i];
            return axis == 0   ? double(t.a.x) + t.b.x + t.c.x
                   : axis == 1 ? double(t.a.y) + t.b.y + t.c.y
                               : double(t.a.z) + t.b.z + t.c.z;
        };
        const auto half = count / 2;
        std::nth_element(order_.begin() + first, order_.begin() + first + half, order_.begin() + first + count,
                         [&](auto a, auto b) {
                             const auto x = centroid(a), y = centroid(b);
                             return x == y ? a < b : x < y;
                         });
        node.left = build(first, half);
        node.right = build(first + half, count - half);
        node.count = 0;
    }
    nodes_[index] = node;
    return index;
}
std::optional<TriangleHit> TriangleQuery::intersect(Vec3 from, Vec3 to, TriangleQueryOptions options,
                                                    TriangleQueryStats *stats) const {
    const auto radius = options.radius;
    if (!finite(from) || !finite(to) || !std::isfinite(radius) || radius < 0)
        throw std::invalid_argument("Triangle query requires finite segment and nonnegative radius");
    if (options.initial_overlap != InitialOverlap::report && options.initial_overlap != InitialOverlap::ignore)
        throw std::invalid_argument("Unknown triangle initial-overlap policy");
    if (stats)
        *stats = {};
    if (nodes_.empty())
        return {};
    const auto start = wide(from), delta = wide(to) - start;
    std::optional<TriangleHit> result;
    const auto visit = [&](auto &&self, std::size_t index) -> void {
        const auto &node = nodes_[index];
        if (stats)
            ++stats->bounds_tested;
        if (!bounds(node.minimum, node.maximum, start, delta, radius, result ? result->fraction : 1))
            return;
        if (!node.count) {
            self(self, node.left);
            self(self, node.right);
            return;
        }
        for (std::size_t i = node.first; i < node.first + node.count; ++i) {
            const auto triangle = order_[i];
            if (stats)
                ++stats->triangles_tested;
            if (const auto hit = contact(triangles_[triangle], start, delta, radius, result ? result->fraction : 1,
                                         options.initial_overlap))
                if (!result || *hit < result->fraction || (*hit == result->fraction && triangle < result->triangle))
                    result = TriangleHit{*hit, triangle};
        }
    };
    visit(visit, 0);
    return result;
}
} // namespace anima
