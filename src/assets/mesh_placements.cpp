#include "mesh_limits.hpp"
#include <algorithm>
#include <anima/mesh_placements.hpp>
#include <array>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace anima {
namespace {
constexpr float affine_tolerance = 1e-5F;
// Bits of each axis in a cluster ordering key; three of them fill 63 bits.
constexpr unsigned morton_bits = 21;

void require(bool value, const char *message) {
    if (!value)
        throw std::invalid_argument(message);
}
void expand(RenderBounds &bounds, Vec3 v) {
    require(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z), "Placed copies exceed the finite range");
    if (!bounds.valid) {
        bounds = {v, v, true};
        return;
    }
    bounds.minimum = {std::min(bounds.minimum.x, v.x), std::min(bounds.minimum.y, v.y),
                      std::min(bounds.minimum.z, v.z)};
    bounds.maximum = {std::max(bounds.maximum.x, v.x), std::max(bounds.maximum.y, v.y),
                      std::max(bounds.maximum.z, v.z)};
}
void expand(RenderBounds &bounds, const RenderBounds &other) {
    if (other.valid) {
        expand(bounds, other.minimum);
        expand(bounds, other.maximum);
    }
}
// Widens @p bounds by the margin Scene gives posed bounds, which also covers the rounding of composing the
// placement on the GPU in another order.
void pad(RenderBounds &bounds) {
    if (!bounds.valid)
        return;
    const auto margin = [](float lo, float hi) {
        return std::max({1.F, std::abs(lo), std::abs(hi)}) * (2 * mesh_limits::skin_weight_tolerance);
    };
    const Vec3 m{margin(bounds.minimum.x, bounds.maximum.x), margin(bounds.minimum.y, bounds.maximum.y),
                 margin(bounds.minimum.z, bounds.maximum.z)};
    bounds.minimum = bounds.minimum - m;
    bounds.maximum = bounds.maximum + m;
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
    require(
        std::none_of(mesh->draws_.begin(), mesh->draws_.end(), [](const IndexedDraw &draw) { return draw.skinned; }),
        "Placements draw only rigid meshes");
    require(!transforms.empty() && transforms.size() <= max_count, "Placement count must be from 1 to 1048576");
    for (const auto &m : transforms)
        require(std::all_of(m.begin(), m.end(), [](float v) { return std::isfinite(v); }) &&
                    std::abs(m[3]) < affine_tolerance && std::abs(m[7]) < affine_tolerance &&
                    std::abs(m[11]) < affine_tolerance && std::abs(m[15] - 1) < affine_tolerance,
                "Placement transforms must be finite and affine");

    // Order the placements along a Morton curve through their translations, quantized within their bounds, so
    // that each run of cluster_size holds near neighbours. The sort is stable, so equal keys keep their order.
    RenderBounds origins;
    for (const auto &m : transforms)
        expand(origins, translation_of(m));
    const auto extent = origins.maximum - origins.minimum;
    const auto quantize = [](float offset, float size) -> std::uint64_t {
        constexpr auto steps = double((std::uint64_t{1} << morton_bits) - 1);
        return size > 0 ? static_cast<std::uint64_t>(std::clamp(double(offset) / size, 0.0, 1.0) * steps) : 0;
    };
    std::vector<std::uint64_t> keys(transforms.size());
    for (std::size_t i = 0; i < transforms.size(); ++i) {
        const auto offset = translation_of(transforms[i]) - origins.minimum;
        keys[i] = spread(quantize(offset.x, extent.x)) | spread(quantize(offset.y, extent.y)) << 1 |
                  spread(quantize(offset.z, extent.z)) << 2;
    }
    std::vector<std::size_t> order(transforms.size());
    std::iota(order.begin(), order.end(), std::size_t{});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return keys[a] < keys[b]; });

    // Each bounds part's corners in mesh space under its rest node, so each copy only transforms points.
    const auto &draws = mesh->draws_;
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

    auto result = std::shared_ptr<MeshPlacements>(new MeshPlacements);
    result->mesh_ = std::move(mesh);
    result->transforms_.reserve(transforms.size());
    result->clusters_.reserve((transforms.size() + cluster_size - 1) / cluster_size);
    result->primitive_bounds_.resize(draws.size());
    for (std::size_t first = 0; first < order.size(); first += cluster_size) {
        const auto count = std::min(cluster_size, order.size() - first);
        Cluster cluster{static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(count), {}};
        for (std::size_t k = first; k < first + count; ++k) {
            const auto &placement = transforms[order[k]];
            result->transforms_.push_back(placement);
            for (std::size_t i = 0; i < draws.size(); ++i)
                for (const auto &points : corners[i])
                    for (const auto &corner : points) {
                        const auto v = point(placement, corner);
                        expand(cluster.bounds, v);
                        expand(result->primitive_bounds_[i], v);
                    }
        }
        pad(cluster.bounds);
        result->clusters_.push_back(cluster);
    }
    for (auto &bounds : result->primitive_bounds_) {
        pad(bounds);
        expand(result->bounds_, bounds);
    }
    return result;
}
} // namespace anima
