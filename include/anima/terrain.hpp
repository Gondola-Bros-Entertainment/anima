#pragma once
#include <anima/core/heightfield.hpp>
#include <anima/scene.hpp>

namespace anima {
// Immutable owned samples for applications that do not already own a heightfield.
// Queries use the field's coordinates, independently of any presentation object.
class TerrainData {
  public:
    explicit TerrainData(Heightfield field) : field_(std::move(field)) { validate_heightfield(field_.view()); }
    [[nodiscard]] HeightfieldView view() const { return field_.view(); }
    [[nodiscard]] std::optional<HeightfieldSample> sample(float x, float z) const {
        return sample_heightfield(view(), x, z);
    }
    [[nodiscard]] std::optional<HeightfieldHit> intersect(Vec3 from, Vec3 to, float clearance = 0) const {
        return intersect_heightfield(view(), from, to, clearance);
    }

  private:
    Heightfield field_;
};

struct TerrainAppearance {
    // Borrowed only during compilation. Empty arrays select computed normals/white.
    std::span<const Vec3> normals, colors;
    float uv_scale = 1;
    std::span<const Material> materials;
    std::span<const Texture> textures;
    std::string node_name = "Terrain";
};

// A region of a common sample lattice. Integer offsets keep neighbouring chunks'
// shared vertices bit-identical even with fractional spacing and distant origins.
struct TerrainGrid {
    std::size_t columns{}, rows{};
    double origin_x{}, origin_z{}, spacing_x = 1, spacing_z = 1;
    std::span<const float> heights;
    std::int64_t column_offset{}, row_offset{};
};

class Terrain {
  public:
    explicit Terrain(std::shared_ptr<const TerrainData> data, TerrainAppearance appearance = {});
    [[nodiscard]] const TerrainData &data() const { return *data_; }
    [[nodiscard]] const std::shared_ptr<const Mesh> &mesh() const { return mesh_; }
    // Adapter for consumers that already own validated packaged heightfield samples.
    // Rendering uses the query API's diagonal and upward triangle winding.
    [[nodiscard]] static std::shared_ptr<const Mesh> compile(HeightfieldView field, TerrainAppearance appearance = {});
    [[nodiscard]] static std::shared_ptr<const Mesh> compile(TerrainGrid grid, TerrainAppearance appearance = {});

  private:
    std::shared_ptr<const TerrainData> data_;
    std::shared_ptr<const Mesh> mesh_;
};
} // namespace anima
