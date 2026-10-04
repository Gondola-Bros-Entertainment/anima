#include "near.hpp"
#include <anima/terrain.hpp>
#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace anima;
namespace {
constexpr double normal_tolerance = 1e-6;
// Corners of a cell's two triangles as (column, row) steps from its first sample, in the documented order.
constexpr std::array<std::pair<std::size_t, std::size_t>, 6> cell_corners{
    {{0, 0}, {1, 1}, {1, 0}, {0, 0}, {0, 1}, {1, 1}}};
std::array<std::uint32_t, 3> bits(Vec3 v) { return std::bit_cast<std::array<std::uint32_t, 3>>(v); }
// An uneven lattice, so that neighbouring samples' slopes differ and one-sided differences differ from central ones.
Heightfield uneven_lattice(std::size_t columns, std::size_t rows) {
    Heightfield field{columns, rows, -2, 3, .5F, .75F, {}};
    for (std::size_t z = 0; z < rows; ++z)
        for (std::size_t x = 0; x < columns; ++x)
            field.heights.push_back(
                float(std::sin(1.3 * double(x)) * 2 + std::cos(.7 * double(z)) * double(z) * .4 + double(x * z) * .05));
    return field;
}
// The heights of a region of a lattice, as an application that streams chunks holds them.
std::vector<float> region_heights(const Heightfield &field, std::size_t column_offset, std::size_t row_offset,
                                  std::size_t columns, std::size_t rows) {
    std::vector<float> heights;
    for (std::size_t z = 0; z < rows; ++z)
        for (std::size_t x = 0; x < columns; ++x)
            heights.push_back(field.heights[(row_offset + z) * field.columns + column_offset + x]);
    return heights;
}
TerrainGrid region_grid(const Heightfield &field, std::span<const float> heights, std::size_t column_offset,
                        std::size_t row_offset, std::size_t columns, std::size_t rows) {
    TerrainGrid grid{columns, rows, field.origin_x, field.origin_z, field.spacing_x, field.spacing_z, heights};
    grid.column_offset = std::int64_t(column_offset);
    grid.row_offset = std::int64_t(row_offset);
    return grid;
}
} // namespace

TEST_CASE("Chunks compiled with terrain normals shade their shared samples as the whole lattice does") {
    const auto lattice = uneven_lattice(7, 5);
    const auto whole = Terrain::compile(lattice.view());
    REQUIRE(whole->vertices().size() == lattice.heights.size());
    // Four chunks of 4 by 3 samples that share column 3 and row 2.
    constexpr std::size_t columns = 4, rows = 3;
    bool default_border_differs = false;
    for (const std::size_t column_offset : {0, 3})
        for (const std::size_t row_offset : {0, 2}) {
            CAPTURE(column_offset);
            CAPTURE(row_offset);
            const auto heights = region_heights(lattice, column_offset, row_offset, columns, rows);
            const auto normals = terrain_normals(lattice.view(), column_offset, row_offset, columns, rows);
            REQUIRE(normals.size() == columns * rows);
            TerrainAppearance appearance;
            appearance.normals = normals;
            const auto grid = region_grid(lattice, heights, column_offset, row_offset, columns, rows);
            const auto chunk = Terrain::compile(grid, appearance);
            const auto plain = Terrain::compile(grid);
            REQUIRE(chunk->vertices().size() == columns * rows);
            for (std::size_t z = 0; z < rows; ++z)
                for (std::size_t x = 0; x < columns; ++x) {
                    CAPTURE(x);
                    CAPTURE(z);
                    const auto &vertex = chunk->vertices()[z * columns + x];
                    const auto &reference = whole->vertices()[(row_offset + z) * lattice.columns + column_offset + x];
                    CHECK(bits(vertex.position) == bits(reference.position));
                    CHECK(bits(vertex.normal) == bits(reference.normal));
                    CHECK(bits(normals[z * columns + x]) == bits(reference.normal));
                    if (bits(plain->vertices()[z * columns + x].normal) != bits(reference.normal))
                        default_border_differs = true;
                }
        }
    // Without them, each chunk's normals are one-sided at its own border, which is the seam they prevent.
    CHECK(default_border_differs);
}

TEST_CASE("Terrain normals of a chunk need only a one-sample margin around it") {
    const auto lattice = uneven_lattice(7, 5);
    // Columns 3 to 6 and rows 0 to 2, with the margin on the sides that have neighbours: column 2 and row 3.
    const auto margin_heights = region_heights(lattice, 2, 0, 5, 4);
    const HeightfieldView margin{5, 4, 0, 0, lattice.spacing_x, lattice.spacing_z, margin_heights};
    const auto from_margin = terrain_normals(margin, 1, 0, 4, 3);
    const auto from_lattice = terrain_normals(lattice.view(), 3, 0, 4, 3);
    REQUIRE(from_margin.size() == from_lattice.size());
    for (std::size_t i = 0; i < from_margin.size(); ++i) {
        CAPTURE(i);
        CHECK(bits(from_margin[i]) == bits(from_lattice[i]));
    }
}

TEST_CASE("Default terrain normals take central differences, one-sided at the grid border") {
    const auto field = uneven_lattice(5, 4);
    const auto mesh = Terrain::compile(field.view());
    const auto normals = terrain_normals(field.view(), 0, 0, field.columns, field.rows);
    REQUIRE(mesh->vertices().size() == field.heights.size());
    REQUIRE(normals.size() == field.heights.size());
    const auto height = [&](std::size_t x, std::size_t z) { return double(field.heights[z * field.columns + x]); };
    for (std::size_t z = 0; z < field.rows; ++z)
        for (std::size_t x = 0; x < field.columns; ++x) {
            CAPTURE(x);
            CAPTURE(z);
            const auto left = x == 0 ? x : x - 1, right = x + 1 == field.columns ? x : x + 1;
            const auto back = z == 0 ? z : z - 1, front = z + 1 == field.rows ? z : z + 1;
            const double dx = (height(right, z) - height(left, z)) / (double(right - left) * field.spacing_x);
            const double dz = (height(x, front) - height(x, back)) / (double(front - back) * field.spacing_z);
            const double length = std::sqrt(dx * dx + 1 + dz * dz);
            const auto &normal = mesh->vertices()[z * field.columns + x].normal;
            CHECK(normal.x == Near{-dx / length, normal_tolerance});
            CHECK(normal.y == Near{1 / length, normal_tolerance});
            CHECK(normal.z == Near{-dz / length, normal_tolerance});
            CHECK(bits(normals[z * field.columns + x]) == bits(normal));
        }
}

TEST_CASE("Terrain compiles one vertex per sample and indexes the surface that welded triangles give") {
    const auto field = uneven_lattice(4, 3);
    std::vector<Vec3> colors;
    for (std::size_t i = 0; i < field.heights.size(); ++i)
        colors.push_back({float(i) / 16, .5F, 1 - float(i) / 16});
    TerrainAppearance appearance;
    appearance.colors = colors;
    appearance.uv_scale = .25F;
    const auto mesh = Terrain::compile(field.view(), appearance);
    const auto quads = (field.columns - 1) * (field.rows - 1);
    REQUIRE(mesh->vertices().size() == field.heights.size());
    REQUIRE(mesh->indices().size() == 6 * quads);
    REQUIRE(mesh->primitives().size() == 1);
    CHECK(mesh->primitives()[0].first_index == 0);
    CHECK(mesh->primitives()[0].index_count == 6 * quads);
    for (std::size_t z = 0; z < field.rows; ++z)
        for (std::size_t x = 0; x < field.columns; ++x) {
            CAPTURE(x);
            CAPTURE(z);
            const auto &vertex = mesh->vertices()[z * field.columns + x];
            CHECK(vertex.position.x == float(double(field.origin_x) + double(x) * double(field.spacing_x)));
            CHECK(vertex.position.y == field.heights[z * field.columns + x]);
            CHECK(vertex.position.z == float(double(field.origin_z) + double(z) * double(field.spacing_z)));
            CHECK(bits(vertex.color) == bits(colors[z * field.columns + x]));
        }

    // The same corners as an unindexed triangle list, which Mesh::compile welds.
    Asset triangles;
    triangles.nodes.resize(1);
    triangles.nodes[0].name = "Terrain";
    triangles.materials.push_back({"Terrain", {1, 1, 1}, -1});
    triangles.primitives.resize(1);
    triangles.primitives[0].material = 0;
    std::size_t corner = 0;
    for (std::size_t z = 0; z + 1 < field.rows; ++z)
        for (std::size_t x = 0; x + 1 < field.columns; ++x)
            for (const auto &[dx, dz] : cell_corners) {
                const auto sample = (z + dz) * field.columns + x + dx;
                CHECK(mesh->indices()[corner] == sample);
                ++corner;
                triangles.primitives[0].vertices.push_back(mesh->vertices()[sample]);
            }
    const auto welded = Mesh::compile(triangles);
    REQUIRE(welded->indices().size() == mesh->indices().size());
    CHECK(welded->vertices().size() == mesh->vertices().size());
    for (std::size_t i = 0; i < mesh->indices().size(); ++i) {
        CAPTURE(i);
        const auto &a = mesh->vertices()[mesh->indices()[i]];
        const auto &b = welded->vertices()[welded->indices()[i]];
        CHECK(bits(a.position) == bits(b.position));
        CHECK(bits(a.normal) == bits(b.normal));
        CHECK(bits(a.color) == bits(b.color));
        CHECK(a.uv == b.uv);
        CHECK(a.tangent == b.tangent);
    }
    // Every triangle winds counterclockwise seen from +Y.
    for (std::size_t i = 0; i < mesh->indices().size(); i += 3) {
        CAPTURE(i);
        const auto a = mesh->vertices()[mesh->indices()[i]].position,
                   b = mesh->vertices()[mesh->indices()[i + 1]].position,
                   c = mesh->vertices()[mesh->indices()[i + 2]].position;
        CHECK(cross(b - a, c - a).y > 0);
    }
    const auto &bounds = mesh->rest_bounds(), &welded_bounds = welded->rest_bounds();
    REQUIRE(bounds.valid);
    REQUIRE(welded_bounds.valid);
    CHECK(bits(bounds.minimum) == bits(welded_bounds.minimum));
    CHECK(bits(bounds.maximum) == bits(welded_bounds.maximum));
    CHECK(mesh->primitives()[0].material == welded->primitives()[0].material);
    CHECK(mesh->palette_size() == welded->palette_size());
}

TEST_CASE("Terrain normals reject invalid lattices, regions outside them and nonfinite heights they read") {
    const auto field = uneven_lattice(4, 3);
    auto lattice = field.view();
    const std::array<float, 3> column{0, 1, 2};
    CHECK_THROWS_WITH_AS((void)terrain_normals({1, 3, 0, 0, 1, 1, column}, 0, 0, 1, 1),
                         "Invalid terrain normal lattice", std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)terrain_normals({4, 4, 0, 0, 1, 1, field.heights}, 0, 0, 1, 1),
                         "Invalid terrain normal lattice", std::invalid_argument);
    for (const float spacing :
         {0.F, -1.F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        CAPTURE(spacing);
        lattice.spacing_x = spacing;
        CHECK_THROWS_WITH_AS((void)terrain_normals(lattice, 0, 0, 1, 1), "Invalid terrain normal lattice",
                             std::invalid_argument);
        lattice.spacing_x = field.spacing_x;
        lattice.spacing_z = spacing;
        CHECK_THROWS_WITH_AS((void)terrain_normals(lattice, 0, 0, 1, 1), "Invalid terrain normal lattice",
                             std::invalid_argument);
        lattice.spacing_z = field.spacing_z;
    }

    constexpr auto outside = "Terrain normal region exceeds the lattice";
    CHECK_THROWS_WITH_AS((void)terrain_normals(lattice, 0, 0, 0, 1), outside, std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)terrain_normals(lattice, 0, 0, 1, 0), outside, std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)terrain_normals(lattice, 1, 0, 4, 1), outside, std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)terrain_normals(lattice, 0, 1, 1, 3), outside, std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)terrain_normals(lattice, 4, 0, 1, 1), outside, std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)terrain_normals(lattice, std::numeric_limits<std::size_t>::max(), 0, 2, 1), outside,
                         std::invalid_argument);
    CHECK(terrain_normals(lattice, 3, 2, 1, 1).size() == 1);

    // Sample (0, 0) is within one sample of a region that starts at column 1, and two samples from one at column 2.
    auto heights = field.heights;
    heights[0] = std::numeric_limits<float>::quiet_NaN();
    lattice.heights = heights;
    CHECK_THROWS_WITH_AS((void)terrain_normals(lattice, 1, 1, 2, 1), "Nonfinite terrain height", std::invalid_argument);
    CHECK(terrain_normals(lattice, 2, 1, 2, 2).size() == 4);
}
