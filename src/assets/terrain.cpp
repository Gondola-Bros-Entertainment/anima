#include <anima/terrain.hpp>
#include <limits>

namespace anima {
namespace {
// Normal of sample (x, z) of a row-major grid by central differences over its neighbours, one-sided at the grid
// border. Terrain::compile and terrain_normals() share it, so that their normals agree to the bit.
Vec3 sample_normal(std::span<const float> heights, std::size_t columns, std::size_t rows, double spacing_x,
                   double spacing_z, std::size_t x, std::size_t z) {
    const auto height = [&](std::size_t column, std::size_t row) { return double(heights[row * columns + column]); };
    const auto left = x ? x - 1 : x, right = std::min(x + 1, columns - 1);
    const auto back = z ? z - 1 : z, front = std::min(z + 1, rows - 1);
    const float dx = float((height(right, z) - height(left, z)) / (double(right - left) * spacing_x));
    const float dz = float((height(x, front) - height(x, back)) / (double(front - back) * spacing_z));
    return normalized({-dx, 1, -dz});
}
} // namespace

std::vector<Vec3> terrain_normals(HeightfieldView lattice, std::size_t column_offset, std::size_t row_offset,
                                  std::size_t columns, std::size_t rows) {
    if (lattice.columns < 2 || lattice.rows < 2 ||
        lattice.columns > std::numeric_limits<std::size_t>::max() / lattice.rows ||
        lattice.heights.size() != lattice.columns * lattice.rows || !std::isfinite(lattice.spacing_x) ||
        !std::isfinite(lattice.spacing_z) || lattice.spacing_x <= 0 || lattice.spacing_z <= 0)
        throw std::invalid_argument("Invalid terrain normal lattice");
    if (!columns || !rows || column_offset >= lattice.columns || columns > lattice.columns - column_offset ||
        row_offset >= lattice.rows || rows > lattice.rows - row_offset)
        throw std::invalid_argument("Terrain normal region exceeds the lattice");
    const auto left = column_offset ? column_offset - 1 : 0,
               right = std::min(column_offset + columns + 1, lattice.columns);
    const auto back = row_offset ? row_offset - 1 : 0, front = std::min(row_offset + rows + 1, lattice.rows);
    for (auto z = back; z < front; ++z)
        for (auto x = left; x < right; ++x)
            if (!std::isfinite(lattice.heights[z * lattice.columns + x]))
                throw std::invalid_argument("Nonfinite terrain height");
    std::vector<Vec3> normals;
    normals.reserve(columns * rows);
    for (std::size_t z = 0; z < rows; ++z)
        for (std::size_t x = 0; x < columns; ++x)
            normals.push_back(sample_normal(lattice.heights, lattice.columns, lattice.rows, lattice.spacing_x,
                                            lattice.spacing_z, column_offset + x, row_offset + z));
    return normals;
}
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
        asset.materials.push_back({"Terrain", {1, 1, 1}, no_index});
    asset.primitives.resize(1);
    auto &primitive = asset.primitives[0];
    primitive.material = 0;
    // One vertex per sample, each computed once; axis() keeps their positions distinct, so there is nothing to weld.
    primitive.vertices.reserve(samples);
    for (std::size_t z = 0; z < field.rows; ++z)
        for (std::size_t x = 0; x < field.columns; ++x) {
            const auto index = z * field.columns + x;
            SourceVertex v;
            v.position = {float(field.origin_x + (double(field.column_offset) + double(x)) * field.spacing_x),
                          field.heights[index],
                          float(field.origin_z + (double(field.row_offset) + double(z)) * field.spacing_z)};
            v.normal = appearance.normals.empty() ? sample_normal(field.heights, field.columns, field.rows,
                                                                  field.spacing_x, field.spacing_z, x, z)
                                                  : appearance.normals[index];
            if (!appearance.colors.empty())
                v.color = appearance.colors[index];
            v.uv = {v.position.x * appearance.uv_scale, v.position.z * appearance.uv_scale};
            const auto tangent = normalized(Vec3{v.normal.y, -v.normal.x, 0});
            v.tangent = {tangent.x, tangent.y, tangent.z, -1};
            primitive.vertices.push_back(v);
        }
    std::vector<std::uint32_t> indices;
    indices.reserve(quads * 6);
    for (std::size_t z = 0; z + 1 < field.rows; ++z)
        for (std::size_t x = 0; x + 1 < field.columns; ++x)
            for (const auto &[dx, dz] :
                 std::array<std::pair<unsigned, unsigned>, 6>{{{0, 0}, {1, 1}, {1, 0}, {0, 0}, {0, 1}, {1, 1}}})
                indices.push_back(static_cast<std::uint32_t>((z + dz) * field.columns + x + dx));
    return Mesh::compile_indexed(asset, std::span(&indices, 1), {.texel_retention = appearance.texel_retention});
}
} // namespace anima
