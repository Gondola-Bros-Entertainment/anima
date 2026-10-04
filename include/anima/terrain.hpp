#pragma once
#include <anima/core/heightfield.hpp>
#include <anima/scene.hpp>

/// @file
/// Owned terrain samples and render-mesh compilation for heightfields. Part of the `anima::assets` target.
///
/// Terrain follows the anima::HeightfieldView layout: columns advance along +X and rows along +Z, heights are world Y
/// in row-major order, and each cell splits along its (0, 0) to (1, 1) diagonal. Invalid arguments throw
/// `std::invalid_argument`.

namespace anima {
/// Immutable owned heightfield samples, for applications that do not already own a Heightfield. Queries use the
/// field's own coordinates.
class TerrainData {
  public:
    /// Takes ownership of @p field. Throws when validate_heightfield() rejects it.
    explicit TerrainData(Heightfield field) : field_(std::move(field)) { validate_heightfield(field_.view()); }
    /// View of the owned samples, valid while this object lives.
    [[nodiscard]] HeightfieldView view() const { return field_.view(); }
    /// Height and upward face normal at (@p x, @p z), as sample_heightfield() computes them; empty outside the grid
    /// or for nonfinite coordinates.
    [[nodiscard]] std::optional<HeightfieldSample> sample(float x, float z) const {
        return sample_heightfield(view(), x, z);
    }
    /// First contact of the segment from @p from to @p to with the terrain raised by @p clearance, as
    /// intersect_heightfield() computes it; empty when the segment misses. The endpoints must be finite, and
    /// @p clearance finite and nonnegative.
    [[nodiscard]] std::optional<HeightfieldHit> intersect(Vec3 from, Vec3 to, float clearance = 0) const {
        return intersect_heightfield(view(), from, to, clearance);
    }

  private:
    Heightfield field_;
};

/// Optional render inputs for Terrain::compile, read only during compilation.
struct TerrainAppearance {
    /// One finite normal per sample, in height order, used as given. Empty computes them from the grid's own heights
    /// with terrain_normals()'s formula, so they are one-sided at the grid border, which for a TerrainGrid region is
    /// the region's border: chunks of one lattice pass terrain_normals() of the lattice, or authored normals, to shade
    /// the samples they share alike.
    std::span<const Vec3> normals;
    /// One finite vertex color per sample, in height order. Empty makes every vertex white.
    std::span<const Vec3> colors;
    /// Scales vertex X and Z into texture coordinates; must be finite.
    float uv_scale = 1;
    /// Materials copied into the mesh; the terrain uses the first. Empty uses one white, untextured material named
    /// `Terrain`.
    std::span<const Material> materials;
    /// Textures copied into the mesh for the materials to reference; the copies share their images.
    std::span<const Texture> textures;
    /// Name of the mesh's single node.
    std::string node_name = "Terrain";
    /// How long the mesh holds its textures' texels; see Mesh::compile.
    TexelRetention texel_retention = TexelRetention::keep;
};

/// A region of a shared sample lattice, compiled as one terrain chunk.
///
/// Region sample (x, z) lies at X = `origin_x + (column_offset + x) * spacing_x` and
/// Z = `origin_z + (row_offset + z) * spacing_z`, evaluated in double and then narrowed to float, so chunks that share
/// the origin and spacing produce bit-identical X and Z on their common border. Default normals see only the region's
/// heights, so they differ on that border; pass terrain_normals() of the lattice as TerrainAppearance::normals to
/// make them bit-identical too.
struct TerrainGrid {
    /// Samples along X, at least 2.
    std::size_t columns{};
    /// Samples along Z, at least 2.
    std::size_t rows{};
    /// Finite X of lattice sample (0, 0).
    double origin_x{};
    /// Finite Z of lattice sample (0, 0).
    double origin_z{};
    /// Finite, positive distance between columns.
    double spacing_x = 1;
    /// Finite, positive distance between rows.
    double spacing_z = 1;
    /// `columns * rows` finite heights in row-major order, read only during compilation.
    std::span<const float> heights;
    /// Lattice column of the region's first sample.
    std::int64_t column_offset{};
    /// Lattice row of the region's first sample.
    std::int64_t row_offset{};
};

/// Normals for a region of @p lattice, one per sample in the region's height order, for TerrainAppearance::normals.
///
/// The region holds @p columns by @p rows samples from lattice column @p column_offset and row @p row_offset. A
/// sample's normal is `normalized({-dx, 1, -dz})`, where `dx` and `dz`, the slopes of the heights along X and Z by
/// central differences over its neighbouring lattice samples, one-sided only at the lattice border, are evaluated in
/// double and narrowed to float. Terrain::compile applies the same formula to its own samples when
/// TerrainAppearance::normals is empty. A normal depends only on the heights next to its sample and the spacing, not on
/// the region or the origin, so regions that share a sample get bit-identical normals for it, and a lattice with the
/// same spacing that holds a chunk plus a one-sample margin wherever the chunk has a neighbour gives the chunk the
/// normals of the whole lattice. Reads the heights within one sample of the region, and @p lattice only during the
/// call.
///
/// Throws `std::invalid_argument` ("Invalid terrain normal lattice") unless @p lattice has at least 2 columns and
/// rows, exactly `columns * rows` heights and finite positive spacing; ("Terrain normal region exceeds the lattice")
/// for an empty region or one that extends past the lattice; and ("Nonfinite terrain height") when a height within one
/// sample of the region is not finite.
[[nodiscard]] std::vector<Vec3> terrain_normals(HeightfieldView lattice, std::size_t column_offset,
                                                std::size_t row_offset, std::size_t columns, std::size_t rows);

/// Render mesh compiled from shared TerrainData, in the data's coordinates. Drawing the mesh on a transformed object
/// moves only the visuals, not data() queries.
class Terrain {
  public:
    /// Compiles the mesh for @p data with @p appearance. Throws for a null @p data or when compilation rejects
    /// @p data or @p appearance.
    explicit Terrain(std::shared_ptr<const TerrainData> data, TerrainAppearance appearance = {});
    [[nodiscard]] const TerrainData &data() const { return *data_; }
    [[nodiscard]] const std::shared_ptr<const Mesh> &mesh() const { return mesh_; }
    /// Compiles samples the application already owns, as compile(TerrainGrid, TerrainAppearance) does with zero
    /// offsets. @p field is borrowed only during the call.
    [[nodiscard]] static std::shared_ptr<const Mesh> compile(HeightfieldView field, TerrainAppearance appearance = {});
    /// Compiles @p grid into a mesh with one node and one primitive of two triangles per cell.
    ///
    /// The mesh has one vertex per sample, in height order, and indexes them cell by cell in the same order. The
    /// triangles split each cell along the same diagonal as the heightfield queries and wind counterclockwise seen
    /// from +Y. Throws when @p grid or @p appearance breaks a member constraint, when neighbouring samples do not
    /// stay distinct and finite at float precision, for more than 715,827,882 cells, or when Mesh::compile rejects
    /// the materials, textures or vertices.
    [[nodiscard]] static std::shared_ptr<const Mesh> compile(TerrainGrid grid, TerrainAppearance appearance = {});

  private:
    std::shared_ptr<const TerrainData> data_;
    std::shared_ptr<const Mesh> mesh_;
};
} // namespace anima
