#pragma once
#include <anima/assets/mesh_snapshot.hpp>

namespace anima {
struct RenderBounds {
    Vec3 minimum{}, maximum{};
    bool valid{};
};
struct IndexedDraw {
    std::uint32_t first_index{}, index_count{}, palette_offset{};
    bool skinned{};
    int material = -1;
    std::uint32_t node{};
    std::string node_name, mesh_name;
};

struct MeshCompileOptions {
    std::size_t max_vertices{};  // Zero leaves geometry unsplit.
    unsigned max_texture_edge{}; // Zero preserves authored texture dimensions.
};

// Compiled immutable snapshot. Indexing preserves every source vertex attribute.
// Compile once and share the returned resource across independently posed instances.
class Mesh {
  public:
    [[nodiscard]] static std::shared_ptr<const Mesh> load(const std::filesystem::path &path) {
        return compile(*load_asset(path));
    }
    [[nodiscard]] static std::shared_ptr<const Mesh> compile(const Asset &source);
    [[nodiscard]] static std::vector<std::shared_ptr<const Mesh>> compile_static(const Asset &source,
                                                                                 MeshCompileOptions options = {});
    [[nodiscard]] std::span<const SourceVertex> vertices() const { return vertices_; }
    [[nodiscard]] std::span<const std::uint32_t> indices() const { return indices_; }
    [[nodiscard]] std::span<const IndexedDraw> draws() const { return draws_; }
    [[nodiscard]] const std::shared_ptr<const MeshSnapshot> &materials() const { return materials_; }
    [[nodiscard]] const Pose &rest_pose() const { return rest_; }
    [[nodiscard]] std::size_t palette_size() const { return palette_size_; }
    [[nodiscard]] bool accepts_animation_source(const Asset &source) const;

  private:
    friend class Scene;
    struct BoundPart {
        std::uint32_t palette;
        RenderBounds bound;
    };
    std::vector<SourceVertex> vertices_;
    std::vector<std::uint32_t> indices_;
    std::vector<IndexedDraw> draws_;
    std::shared_ptr<const MeshSnapshot> materials_;
    Pose rest_;
    std::vector<std::pair<std::string, int>> nodes_;
    std::vector<AssetSkin> skins_;
    std::vector<std::vector<BoundPart>> bounds_;
    std::size_t palette_size_{};
};

} // namespace anima
