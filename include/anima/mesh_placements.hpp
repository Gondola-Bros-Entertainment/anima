#pragma once
#include <anima/mesh.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

/// @file
/// Placements that draw copies of one rigid Mesh from a single scene object, for repeated scenery such as trees,
/// rocks and grass. Part of the `anima::assets` target.

namespace anima {
/// Copies of one rigid Mesh, each placed by an affine matrix relative to the object that draws them.
///
/// Scene::set_placements makes a renderer draw its mesh once per placement instead of once at its object: copy `p`
/// draws node `n` at `world * p * rest[n]`, where `world` is the object's world matrix and `rest` is the mesh's
/// rest pose (Mesh::rest_pose()). Such a renderer always draws the rest pose. Creation groups the placements into
/// clusters of at most #cluster_size near one another, ordered along a space-filling curve through their
/// translations, and records the bounds of each cluster's copies, so a renderer can skip the clusters that a view
/// or shadow region cannot see. transforms() lists the placements in that order, not in the order given; the
/// copies are interchangeable, so only blended materials, whose copies blend in that order, can tell.
///
/// Immutable after creation and safe to read from several threads; scenes and renderers share it. VulkanRenderer
/// uploads its transforms once, 48 bytes per placement, and keeps them on the GPU until only the renderer holds
/// the set, as it keeps meshes. Creation takes time proportional to the placements times the bounds parts of the
/// mesh, plus sorting them.
class MeshPlacements {
  public:
    /// Most placements in one set.
    static constexpr std::size_t max_count = std::size_t{1} << 20;
    /// Most placements in one cluster.
    static constexpr std::size_t cluster_size = 64;
    /// A run of transforms() whose copies a renderer culls together.
    struct Cluster {
        /// Index of the cluster's first placement in transforms().
        std::uint32_t first{};
        /// Number of placements, from 1 to #cluster_size.
        std::uint32_t count{};
        /// Bounds of every primitive of the cluster's copies, hidden ones included, and of each copy's center of
        /// Mesh::rest_bounds(), which visibility ranges measure to, relative to the object.
        RenderBounds bounds;
        /// Largest axis scale among the cluster's placements: the length of the longest of the first three columns
        /// of their matrices, which levels of detail scale their errors by.
        float scale{};
    };

    /// Places copies of @p mesh at @p transforms.
    ///
    /// Throws `std::invalid_argument` for a null mesh ("Placements require a mesh"), a mesh with a skinned draw
    /// ("Placements draw only rigid meshes"), no transforms or more than #max_count ("Placement count must be from
    /// 1 to 1048576"), and a transform with a nonfinite element or a last row other than (0, 0, 0, 1) within
    /// `0.00001` ("Placement transforms must be finite and affine"), and copies whose bounds leave the finite `float`
    /// range ("Placed copies exceed the finite range").
    [[nodiscard]] static std::shared_ptr<const MeshPlacements> create(std::shared_ptr<const Mesh> mesh,
                                                                      std::span<const Mat4> transforms);
    /// Mesh that the placements copy.
    [[nodiscard]] const std::shared_ptr<const Mesh> &mesh() const noexcept { return mesh_; }
    /// Placements in cluster order.
    [[nodiscard]] std::span<const Mat4> transforms() const noexcept { return transforms_; }
    /// Clusters in order, covering transforms() without gaps.
    [[nodiscard]] std::span<const Cluster> clusters() const noexcept { return clusters_; }
    /// Bounds of each primitive's copies relative to the object, by Mesh::draws() index; invalid for an empty draw.
    [[nodiscard]] std::span<const RenderBounds> primitive_bounds() const noexcept { return primitive_bounds_; }
    /// Union of primitive_bounds().
    [[nodiscard]] const RenderBounds &bounds() const noexcept { return bounds_; }

  private:
    MeshPlacements() = default;
    std::shared_ptr<const Mesh> mesh_;
    std::vector<Mat4> transforms_;
    std::vector<Cluster> clusters_;
    std::vector<RenderBounds> primitive_bounds_;
    RenderBounds bounds_;
};
} // namespace anima
