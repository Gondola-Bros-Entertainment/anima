#include "../detail/affine.hpp"
#include "render_bounds.hpp"
#include <algorithm>
#include <anima/mesh_placements.hpp>
#include <array>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace anima {
namespace {
// Bits of each axis in a cluster ordering key; three of them fill 63 bits.
constexpr unsigned morton_bits = 21;

void require(bool value, const char *message) {
    if (!value)
        throw std::invalid_argument(message);
}
void expand(RenderBounds &bounds, Vec3 v) {
    require(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z), "Placed copies exceed the finite range");
    encapsulate(bounds, v);
}
void expand(RenderBounds &bounds, const RenderBounds &other) {
    if (other.valid) {
        expand(bounds, other.minimum);
        expand(bounds, other.maximum);
    }
}
// Spreads the low 21 bits of @p v so that two zero bits follow each.
std::uint64_t spread(std::uint64_t v) {
    v &= 0x1FFFFF;
    v = (v | v << 32) & 0x1F00000000FFFF;
    v = (v | v << 16) & 0x1F0000FF0000FF;
    v = (v | v << 8) & 0x100F00F00F00F00F;
    v = (v | v << 4) & 0x10C30C30C30C30C3;
    v = (v | v << 2) & 0x1249249249249249;
    return v;
}
} // namespace

std::shared_ptr<const MeshPlacements> MeshPlacements::create(std::shared_ptr<const Mesh> mesh,
                                                             std::span<const Mat4> transforms) {
    require(bool(mesh), "Placements require a mesh");
    require(std::none_of(mesh->primitives_.begin(), mesh->primitives_.end(),
                         [](const MeshPrimitive &draw) { return draw.skinned; }),
            "Placements draw only rigid meshes");
    static_assert(max_count == 1048576, "The count message states the limit");
    require(!transforms.empty() && transforms.size() <= max_count, "Placement count must be from 1 to 1048576");
    for (const auto &m : transforms)
        require(detail::is_affine(m), "Placement transforms must be finite and affine");

    // Order the placements along a Morton curve through their translations, quantized within their bounds, so
    // that each run of cluster_size holds near neighbours. The sort is stable, so equal keys keep their order.
    // Differences of finite floats can exceed the float range, so they are taken in double, where every one is
    // finite and each ratio lies in [0, 1].
    RenderBounds origins;
    for (const auto &m : transforms)
        expand(origins, translation_of(m));
    const auto quantize = [](float value, float minimum, float maximum) -> std::uint64_t {
        constexpr auto steps = double((std::uint64_t{1} << morton_bits) - 1);
        const auto size = double(maximum) - minimum;
        return size > 0 ? static_cast<std::uint64_t>((double(value) - minimum) / size * steps) : 0;
    };
    std::vector<std::uint64_t> keys(transforms.size());
    for (std::size_t i = 0; i < transforms.size(); ++i) {
        const auto t = translation_of(transforms[i]);
        keys[i] = spread(quantize(t.x, origins.minimum.x, origins.maximum.x)) |
                  spread(quantize(t.y, origins.minimum.y, origins.maximum.y)) << 1 |
                  spread(quantize(t.z, origins.minimum.z, origins.maximum.z)) << 2;
    }
    std::vector<std::size_t> order(transforms.size());
    std::iota(order.begin(), order.end(), std::size_t{});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return keys[a] < keys[b]; });

    // Each bounds part's corners in mesh space under its rest node, so each copy only transforms points.
    const auto &draws = mesh->primitives_;
    const auto &rest = mesh->rest_.world;
    std::vector<std::vector<std::array<Vec3, 8>>> corners(draws.size());
    for (std::size_t i = 0; i < draws.size(); ++i)
        for (const auto &part : mesh->bounds_[i]) {
            const auto &node = rest.at(part.palette);
            const auto &b = part.bound;
            auto &points = corners[i].emplace_back();
            for (unsigned c = 0; c < 8; ++c)
                points[c] = point(node, {c & 1 ? b.maximum.x : b.minimum.x, c & 2 ? b.maximum.y : b.minimum.y,
                                         c & 4 ? b.maximum.z : b.minimum.z});
        }
    // The center of the mesh's rest bounds, which visibility ranges measure each copy to; a cluster's bounds hold it
    // for every copy, so culling a cluster by its bounds never hides a copy that its range still draws.
    const auto &rest_bounds = mesh->rest_bounds_;
    const auto rest_center = center(rest_bounds);

    auto result = std::shared_ptr<MeshPlacements>(new MeshPlacements);
    result->mesh_ = std::move(mesh);
    result->transforms_.reserve(transforms.size());
    result->clusters_.reserve((transforms.size() + cluster_size - 1) / cluster_size);
    result->primitive_bounds_.resize(draws.size());
    for (std::size_t first = 0; first < order.size(); first += cluster_size) {
        const auto count = std::min(cluster_size, order.size() - first);
        Cluster cluster{static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(count), {}, 0};
        for (std::size_t k = first; k < first + count; ++k) {
            const auto &placement = transforms[order[k]];
            result->transforms_.push_back(placement);
            cluster.scale = std::max(
                {cluster.scale, length(axis_x(placement)), length(axis_y(placement)), length(axis_z(placement))});
            for (std::size_t i = 0; i < draws.size(); ++i)
                for (const auto &points : corners[i])
                    for (const auto &corner : points) {
                        const auto v = point(placement, corner);
                        expand(cluster.bounds, v);
                        expand(result->primitive_bounds_[i], v);
                    }
            if (rest_bounds.valid)
                expand(cluster.bounds, point(placement, rest_center));
        }
        detail::pad_posed_bounds(cluster.bounds);
        result->clusters_.push_back(cluster);
    }
    for (auto &bounds : result->primitive_bounds_) {
        detail::pad_posed_bounds(bounds);
        expand(result->bounds_, bounds);
    }
    return result;
}
} // namespace anima
