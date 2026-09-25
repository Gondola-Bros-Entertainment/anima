#include <anima/terrain.hpp>
#include <limits>

namespace anima {
Terrain::Terrain(std::shared_ptr<const TerrainData> data, TerrainAppearance appearance) : data_(std::move(data)) {
    if (!data_)
        throw std::invalid_argument("Null TerrainData");
    mesh_ = compile(data_->view(), appearance);
}
std::shared_ptr<const Mesh> Terrain::compile(HeightfieldView field, TerrainAppearance appearance) {
    return compile(TerrainGrid{field.columns, field.rows, field.origin_x, field.origin_z, field.spacing_x,
                               field.spacing_z, field.heights},
                   appearance);
}
std::shared_ptr<const Mesh> Terrain::compile(TerrainGrid field, TerrainAppearance appearance) {
    validate_heightfield(
        {field.columns, field.rows, 0, 0, float(field.spacing_x), float(field.spacing_z), field.heights});
    if (!std::isfinite(field.origin_x) || !std::isfinite(field.origin_z) || !std::isfinite(field.spacing_x) ||
        !std::isfinite(field.spacing_z) || field.spacing_x <= 0 || field.spacing_z <= 0)
        throw std::invalid_argument("Invalid terrain sample lattice");
    const auto axis = [](double origin, double spacing, std::int64_t offset, std::size_t count) {
        float previous = float(origin + double(offset) * spacing);
        if (!std::isfinite(previous))
            throw std::invalid_argument("Terrain origin exceeds render precision");
        for (std::size_t i = 1; i < count; ++i) {
            const float next = float(origin + (double(offset) + double(i)) * spacing);
            if (!std::isfinite(next) || next <= previous)
                throw std::invalid_argument("Terrain samples collapse at render precision");
            previous = next;
        }
    };
    axis(field.origin_x, field.spacing_x, field.column_offset, field.columns);
    axis(field.origin_z, field.spacing_z, field.row_offset, field.rows);
    const auto samples = field.heights.size();
    if ((!appearance.normals.empty() && appearance.normals.size() != samples) ||
        (!appearance.colors.empty() && appearance.colors.size() != samples) || !std::isfinite(appearance.uv_scale))
        throw std::invalid_argument("Invalid terrain appearance");
    const auto quads = (field.columns - 1) * (field.rows - 1);
    if (quads > std::numeric_limits<std::uint32_t>::max() / 6)
        throw std::invalid_argument("Terrain mesh exceeds index capacity");
    Asset asset;
    asset.nodes.resize(1);
    asset.nodes[0].name = std::move(appearance.node_name);
    asset.materials.assign(appearance.materials.begin(), appearance.materials.end());
    asset.textures.assign(appearance.textures.begin(), appearance.textures.end());
    if (asset.materials.empty())
        asset.materials.push_back({"Terrain", {1, 1, 1}, -1});
    asset.primitives.resize(1);
    auto &primitive = asset.primitives[0];
    primitive.material = 0;
    primitive.vertices.reserve(quads * 6);
    const auto height = [&](std::size_t x, std::size_t z) { return field.heights[z * field.columns + x]; };
    const auto vertex = [&](std::size_t x, std::size_t z) {
        const auto index = z * field.columns + x;
        SourceVertex v;
        v.position = {float(field.origin_x + (double(field.column_offset) + double(x)) * field.spacing_x), height(x, z),
                      float(field.origin_z + (double(field.row_offset) + double(z)) * field.spacing_z)};
        if (!appearance.normals.empty())
            v.normal = appearance.normals[index];
        else {
            const auto left = x ? x - 1 : x, right = std::min(x + 1, field.columns - 1);
            const auto back = z ? z - 1 : z, front = std::min(z + 1, field.rows - 1);
            const float dx =
                float((double(height(right, z)) - height(left, z)) / (double(right - left) * field.spacing_x));
            const float dz =
                float((double(height(x, front)) - height(x, back)) / (double(front - back) * field.spacing_z));
            v.normal = normalized({-dx, 1, -dz});
        }
        if (!appearance.colors.empty())
            v.color = appearance.colors[index];
        v.uv = {v.position.x * appearance.uv_scale, v.position.z * appearance.uv_scale};
        const auto tangent = normalized(Vec3{v.normal.y, -v.normal.x, 0});
        v.tangent = {tangent.x, tangent.y, tangent.z, -1};
        return v;
    };
    for (std::size_t z = 0; z + 1 < field.rows; ++z)
        for (std::size_t x = 0; x + 1 < field.columns; ++x)
            for (const auto &[dx, dz] :
                 std::array<std::pair<unsigned, unsigned>, 6>{{{0, 0}, {1, 1}, {1, 0}, {0, 0}, {0, 1}, {1, 1}}})
                primitive.vertices.push_back(vertex(x + dx, z + dz));
    return Mesh::compile(asset);
}
} // namespace anima
