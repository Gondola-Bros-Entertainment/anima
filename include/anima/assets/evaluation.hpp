#pragma once
#include <anima/assets/asset.hpp>
#include <span>

namespace anima {
// Explicit evaluation ancestry is independent of the asset's skin hierarchy.
// Affine locals retain scale and shear from existing flattened exports.
struct EvaluationJoint {
    std::string name;
    std::size_t asset_node{};
    int parent = -1;
};
struct EvaluationPose {
    std::vector<Mat4> local;
};
enum class LayerMode { override_pose, additive };

// Rotation uses shortest-arc quaternion interpolation; the symmetric stretch
// tensor retains scale/shear. Reflections and singular transforms are unsupported.
[[nodiscard]] Mat4 blend_affine(const Mat4 &from, const Mat4 &to, float weight);
[[nodiscard]] Quat affine_rotation(const Mat4 &transform);

class EvaluationRig {
  public:
    // Every skin joint must be explicitly mapped. Names and asset nodes are unique.
    EvaluationRig(const Asset &asset, std::vector<EvaluationJoint> joints);
    [[nodiscard]] std::size_t joint(std::string_view name) const;
    [[nodiscard]] std::size_t asset_node(std::size_t joint) const { return joints_.at(joint).asset_node; }
    [[nodiscard]] std::size_t size() const { return joints_.size(); }
    [[nodiscard]] bool descendant(std::size_t child, std::size_t ancestor) const;
    [[nodiscard]] EvaluationPose encode(const Pose &source) const;
    [[nodiscard]] std::vector<Mat4> world(const EvaluationPose &pose) const;
    [[nodiscard]] EvaluationPose from_world(std::span<const Mat4> world) const;
    [[nodiscard]] std::vector<float> subtree_mask(std::size_t root, float weight = 1) const;
    // Additive order: base * blend(identity, inverse(reference) * layer, weight).
    [[nodiscard]] EvaluationPose layer(const EvaluationPose &base, const EvaluationPose &contribution,
                                       std::span<const float> weights, LayerMode mode = LayerMode::override_pose,
                                       const EvaluationPose *reference = nullptr) const;
    // A render-only Pose has world matrices and no TRS locals. It cannot be fed
    // accidentally into blend_pose (which requires matching TRS locals).
    // Unmapped descendants follow the mapped parent, retaining their source local.
    [[nodiscard]] Pose render_pose(const Pose &source, const EvaluationPose &evaluated) const;

  private:
    std::vector<EvaluationJoint> joints_;
    std::vector<std::size_t> order_, asset_order_;
    std::vector<int> asset_parents_, mapping_;
};

struct TwoBoneContact {
    std::size_t start{}, middle{}, end{};
    Vec3 target{}, pole{}; // Rig/model space, including the root transform.
    float weight = 1;
    // Interior angle in radians: pi is straight. Bounds never stretch a limb.
    float minimum_angle = 0, maximum_angle = 3.14159265358979323846F;
    std::optional<Quat> end_rotation;
};
struct ContactResult {
    EvaluationPose pose;
    float error{}, upper_length{}, lower_length{};
    bool reachable{};
};
// Rigid rotations update each chain subtree, preserving lengths and affine
// stretch. Calls are ordered by the application; competing contacts need an
// explicit recipe order. End orientation is optional and weightable.
[[nodiscard]] ContactResult solve_contact(const EvaluationRig &rig, const EvaluationPose &pose,
                                          const TwoBoneContact &contact);
} // namespace anima
