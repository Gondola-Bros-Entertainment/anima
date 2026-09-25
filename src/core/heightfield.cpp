#include <anima/core/heightfield.hpp>
#include <limits>
#include <stdexcept>

namespace anima {
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::invalid_argument(message);
}
bool finite(Vec3 p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
void dimensions(HeightfieldView f) {
    require(f.columns >= 2 && f.rows >= 2 && f.columns <= std::numeric_limits<std::size_t>::max() / f.rows &&
                f.heights.size() == f.columns * f.rows,
            "Invalid heightfield dimensions");
    require(std::isfinite(f.origin_x) && std::isfinite(f.origin_z) && std::isfinite(f.spacing_x) &&
                std::isfinite(f.spacing_z) && f.spacing_x > 0 && f.spacing_z > 0,
            "Invalid heightfield coordinates");
    const double max_x = double(f.origin_x) + (f.columns - 1) * double(f.spacing_x);
    const double max_z = double(f.origin_z) + (f.rows - 1) * double(f.spacing_z);
    require(std::isfinite(max_x) && std::isfinite(max_z) && max_x <= std::numeric_limits<float>::max() &&
                max_z <= std::numeric_limits<float>::max() && float(max_x) > f.origin_x && float(max_z) > f.origin_z,
            "Heightfield extent exceeds coordinate precision");
}
struct Plane {
    double x{}, z{}, height{}, dx{}, dz{};
    double at(double px, double pz) const { return height + dx * (px - x) + dz * (pz - z); }
    Vec3 normal() const {
        const auto length = std::hypot(dx, 1.0, dz);
        return {float(-dx / length), float(1 / length), float(-dz / length)};
    }
};
Plane triangle(HeightfieldView f, std::size_t x, std::size_t z, bool lower) {
    const auto index = z * f.columns + x;
    const double a = f.heights[index], b = f.heights[index + 1], c = f.heights[index + f.columns],
                 d = f.heights[index + f.columns + 1];
    require(std::isfinite(a) && std::isfinite(b) && std::isfinite(c) && std::isfinite(d), "Nonfinite terrain height");
    return {double(f.origin_x) + x * double(f.spacing_x), double(f.origin_z) + z * double(f.spacing_z), a,
            (lower ? b - a : d - c) / f.spacing_x, (lower ? d - b : c - a) / f.spacing_z};
}
std::optional<std::pair<double, double>> coordinates(HeightfieldView f, double x, double z) {
    if (!std::isfinite(x) || !std::isfinite(z))
        return {};
    const auto gx = (x - f.origin_x) / f.spacing_x, gz = (z - f.origin_z) / f.spacing_z;
    if (gx < 0 || gz < 0 || gx > double(f.columns - 1) || gz > double(f.rows - 1))
        return {};
    return std::pair{gx, gz};
}
bool clip_axis(double start, double delta, double maximum, double &enter, double &leave) {
    if (delta == 0)
        return start >= 0 && start <= maximum;
    auto a = -start / delta, b = (maximum - start) / delta;
    if (a > b)
        std::swap(a, b);
    enter = std::max(enter, a);
    leave = std::min(leave, b);
    return enter <= leave;
}
// Amanatides/Woo-style grid traversal, split again at the cell diagonal. The
// callback may stop early. Work scales with crossed cells, not total map size.
template <class Visit> void segments(HeightfieldView f, Vec3 from, Vec3 to, Visit visit) {
    const double gx = (double(from.x) - f.origin_x) / f.spacing_x, gz = (double(from.z) - f.origin_z) / f.spacing_z;
    const double dx = (double(to.x) - from.x) / f.spacing_x, dz = (double(to.z) - from.z) / f.spacing_z;
    double enter = 0, leave = 1;
    if (!clip_axis(gx, dx, double(f.columns - 1), enter, leave) || !clip_axis(gz, dz, double(f.rows - 1), enter, leave))
        return;
    const auto cell = [](double value, double direction, std::size_t count) {
        auto index = std::floor(value);
        if (direction < 0 && index == value)
            --index;
        return static_cast<std::size_t>(std::clamp(index, 0.0, double(count - 2)));
    };
    auto x = cell(gx + dx * enter, dx, f.columns), z = cell(gz + dz * enter, dz, f.rows);
    const auto infinity = std::numeric_limits<double>::infinity();
    auto cursor = enter;
    for (;;) {
        const auto tx = dx == 0 ? infinity : (double(x) + (dx > 0 ? 1 : 0) - gx) / dx;
        const auto tz = dz == 0 ? infinity : (double(z) + (dz > 0 ? 1 : 0) - gz) / dz;
        const auto end = std::min({leave, tx, tz});
        const auto diagonal = dx == dz ? infinity : (double(x) - double(z) - gx + gz) / (dx - dz);
        const auto part = [&](double a, double b) {
            const auto middle = (a + b) * .5;
            const bool lower = gx + dx * middle - double(x) >= gz + dz * middle - double(z);
            return visit(a, b, triangle(f, x, z, lower));
        };
        if (end >= cursor) {
            if (diagonal > cursor && diagonal < end) {
                if (!part(cursor, diagonal) || !part(diagonal, end))
                    return;
            } else if (!part(cursor, end))
                return;
        }
        if (end >= leave)
            return;
        if (tx <= end) {
            if (dx > 0) {
                if (x + 2 >= f.columns)
                    return;
                ++x;
            } else {
                if (!x)
                    return;
                --x;
            }
        }
        if (tz <= end) {
            if (dz > 0) {
                if (z + 2 >= f.rows)
                    return;
                ++z;
            } else {
                if (!z)
                    return;
                --z;
            }
        }
        cursor = end;
    }
}
Vec3 along(Vec3 from, Vec3 to, double t) {
    return {float(double(from.x) + (double(to.x) - from.x) * t), float(double(from.y) + (double(to.y) - from.y) * t),
            float(double(from.z) + (double(to.z) - from.z) * t)};
}
} // namespace
void validate_heightfield(HeightfieldView f) {
    dimensions(f);
    for (const auto h : f.heights)
        require(std::isfinite(h), "Nonfinite terrain height");
}
std::optional<HeightfieldSample> sample_heightfield(HeightfieldView f, float x, float z) {
    dimensions(f);
    const auto grid = coordinates(f, x, z);
    if (!grid)
        return {};
    const auto [gx, gz] = *grid;
    const auto ix = std::min(static_cast<std::size_t>(gx), f.columns - 2),
               iz = std::min(static_cast<std::size_t>(gz), f.rows - 2);
    const auto plane = triangle(f, ix, iz, gx - ix >= gz - iz);
    return HeightfieldSample{float(plane.at(x, z)), plane.normal()};
}
std::optional<HeightfieldHit> intersect_heightfield(HeightfieldView f, Vec3 from, Vec3 to, float clearance) {
    dimensions(f);
    require(finite(from) && finite(to) && std::isfinite(clearance) && clearance >= 0, "Invalid terrain segment");
    std::optional<HeightfieldHit> hit;
    segments(f, from, to, [&](double begin, double end, const Plane &plane) {
        const auto a = along(from, to, begin), b = along(from, to, end);
        const auto da = double(a.y) - plane.at(a.x, a.z) - clearance, db = double(b.y) - plane.at(b.x, b.z) - clearance;
        if (da > 0 && db > 0)
            return true;
        const auto t = da <= 0 ? begin : begin + (end - begin) * da / (da - db);
        hit = HeightfieldHit{t, along(from, to, t), plane.normal()};
        return false;
    });
    return hit;
}
HeightfieldMotion move_on_heightfield(HeightfieldView f, Vec3 from, Vec3 to, double budget, float minimum_up) {
    dimensions(f);
    require(finite(from) && finite(to) && std::isfinite(budget) && budget >= 0 && std::isfinite(minimum_up) &&
                minimum_up >= 0 && minimum_up <= 1,
            "Invalid terrain motion");
    const auto start = sample_heightfield(f, from.x, from.z);
    require(bool(start) && bool(coordinates(f, to.x, to.z)), "Terrain motion leaves the heightfield");
    HeightfieldMotion result{{from.x, start->height, from.z}, 0, 0, false};
    segments(f, from, to, [&](double begin, double end, const Plane &plane) {
        if (plane.normal().y < minimum_up) {
            result.blocked = true;
            return false;
        }
        auto a = along(from, to, begin), b = along(from, to, end);
        a.y = float(plane.at(a.x, a.z));
        b.y = float(plane.at(b.x, b.z));
        const auto length = std::hypot(double(b.x) - a.x, double(b.y) - a.y, double(b.z) - a.z);
        const auto available = std::max(0.0, budget - result.distance);
        const auto fraction = length > available ? available / length : 1.0;
        result.fraction = begin + (end - begin) * fraction;
        result.position = along(a, b, fraction);
        result.distance += length * fraction;
        return fraction == 1;
    });
    return result;
}
} // namespace anima
