#pragma once
#include <anima/assets/asset.hpp>
#include <numbers>
#include <span>

/// @file
/// Affine pose evaluation over an explicit joint hierarchy: masked override and additive layers,
/// and two-bone contact solving.
///
/// Part of the `anima::assets` target. Evaluation transforms must be finite affine matrices whose
/// determinant is not below `-1e-12`, so reflections are rejected. Scale and shear are kept, and
/// so is a collapsed transform, whose determinant is within `1e-12` of 0, such as a joint scaled to
/// zero. Failures throw `std::invalid_argument` or anima::MathError, which derives from it, unless
/// stated.

namespace anima {
/// One joint of an EvaluationRig.
struct EvaluationJoint {
    /// Unique, nonempty joint name.
    std::string name;
    /// Asset node the joint drives; unique within the rig.
    std::size_t asset_node{};
    /// Index of the parent joint in the rig, or -1 for a root. The rig hierarchy may differ from
    /// the asset's.
    int parent = -1;
};
/// Joint-local affine matrices, one per rig joint.
struct EvaluationPose {
    std::vector<Mat4> local;
};
/// How a motion layer combines its contribution with its base. MotionRuntime::evaluate applies an
/// override with EvaluationRig::override_layer and an additive layer with
/// EvaluationRig::additive_layer.
enum class LayerMode {
    override_pose, ///< Blends from the base toward the contribution.
    additive       ///< Applies the contribution's change from a reference pose on top of the base.
};

/// Blends two affine transforms by @p weight in [0, 1].
///
/// The rotations of their polar decompositions interpolate along the shortest arc, and their
/// symmetric stretch (scale and shear) and translation linearly. A collapsed input has no rotation
/// to extract, so then the two interpolate element by element: blended toward a transform scaled
/// to zero, the other shrinks in place. A weight of 0 or 1 returns that input exactly.
[[nodiscard]] Mat4 blend_affine(const Mat4 &from, const Mat4 &to, float weight);
/// Rotation of the polar decomposition of @p transform, as a unit quaternion. Throws for a
/// collapsed transform, which has no rotation.
[[nodiscard]] Quat affine_rotation(const Mat4 &transform);

/// Joint hierarchy for evaluation, mapped onto an asset's nodes.
///
/// The hierarchy is explicit and may differ from the asset's skin hierarchy, so existing
/// flattened exports keep their bind and geometry.
class EvaluationRig {
  public:
    /// Throws unless @p joints is nonempty with unique nonempty names, unique in-range asset nodes
    /// and valid, acyclic parents, every skin joint of @p asset is mapped, and @p asset's own
    /// hierarchy is valid and acyclic.
    EvaluationRig(const Asset &asset, std::vector<EvaluationJoint> joints);
    /// Index of the joint named @p name. Throws `std::out_of_range` for an unknown name.
    [[nodiscard]] std::size_t joint(std::string_view name) const;
    /// Asset node of joint @p joint. Throws `std::out_of_range` for an invalid index.
    [[nodiscard]] std::size_t asset_node(std::size_t joint) const { return joints_.at(joint).asset_node; }
    /// Index of the parent joint of joint @p joint, or -1 for a root. Throws `std::out_of_range` for
    /// an invalid index.
    [[nodiscard]] int parent(std::size_t joint) const;
    /// Number of joints.
    [[nodiscard]] std::size_t size() const { return joints_.size(); }
    /// Whether joint @p child is joint @p ancestor or below it. Throws for an invalid index.
    [[nodiscard]] bool descendant(std::size_t child, std::size_t ancestor) const;
    /// Joint-local pose of the mapped joints of @p source, computed from its world matrices, so
    /// world-only poses are accepted.
    ///
    /// A collapsed evaluation parent's world matrix cannot be inverted, so a joint below one takes
    /// its local transform from the local transforms of @p source along the asset hierarchy
    /// instead. That needs @p source to have local transforms and the joint to lie below its
    /// evaluation parent in the asset hierarchy. Throws unless @p source has one world matrix per
    /// asset node.
    [[nodiscard]] EvaluationPose encode(const Pose &source) const;
    /// Model-space matrix of each joint of @p pose. Throws unless @p pose has one valid local
    /// matrix per joint.
    [[nodiscard]] std::vector<Mat4> world(const EvaluationPose &pose) const;
    /// Joint-local pose from @p world, one valid model-space matrix per joint. Throws for a joint
    /// whose evaluation parent is collapsed; encode() recovers such joints from a pose's local
    /// transforms.
    [[nodiscard]] EvaluationPose from_world(std::span<const Mat4> world) const;
    /// Per-joint weights: @p weight, in [0, 1], for joint @p root and its descendants, 0 elsewhere.
    [[nodiscard]] std::vector<float> subtree_mask(std::size_t root, float weight = 1) const;
    /// Blends @p base toward @p contribution, joint by joint, with blend_affine by @p weights, each
    /// in [0, 1]; a weight of 0 keeps the base joint exactly.
    ///
    /// Throws `std::invalid_argument` "Layer pose/mask count mismatch" unless both poses and
    /// @p weights have one entry per joint, and "Layer/contact weight must be in [0,1]" for a
    /// weight outside that range or nonfinite, and as blend_affine does for a joint it blends.
    [[nodiscard]] EvaluationPose override_layer(const EvaluationPose &base, const EvaluationPose &contribution,
                                                std::span<const float> weights) const;
    /// Applies the change of @p contribution from @p reference on top of @p base, joint by joint:
    /// `base * blend_affine(identity(), inverse(reference) * contribution, weight)` by @p weights,
    /// each in [0, 1]; a weight of 0 keeps the base joint exactly.
    ///
    /// Throws `std::invalid_argument` "Layer pose/mask count mismatch" unless the three poses and
    /// @p weights have one entry per joint, and "Layer/contact weight must be in [0,1]" for a
    /// weight outside that range or nonfinite, and as inverse() and blend_affine do for a joint
    /// with a nonzero weight, so such a reference joint must be invertible.
    [[nodiscard]] EvaluationPose additive_layer(const EvaluationPose &base, const EvaluationPose &contribution,
                                                const EvaluationPose &reference, std::span<const float> weights) const;
    /// World-only Pose of the asset for MeshRenderer::set_pose: mapped nodes take their
    /// @p evaluated matrices, and unmapped nodes keep their @p source transform relative to their
    /// parent, taken from the local transform of @p source when that parent is collapsed.
    ///
    /// The result has no local transforms, so blend_pose rejects it. Throws unless @p source has one
    /// world matrix per asset node, and local transforms where a parent is collapsed.
    [[nodiscard]] Pose render_pose(const Pose &source, const EvaluationPose &evaluated) const;

  private:
    EvaluationPose from_world(std::span<const Mat4> world, const Pose *source) const;
    Mat4 local_below_collapsed(std::size_t joint, const Pose *source) const;
    std::vector<EvaluationJoint> joints_;
    std::vector<std::size_t> order_, asset_order_;
    std::vector<int> asset_parents_, mapping_;
};

/// A two-bone contact request for solve_contact. Positions are in rig (model) space, which
/// includes the root joint's transform.
struct TwoBoneContact {
    /// Rig joint at the root of the chain.
    std::size_t start{};
    /// Joint that bends; below #start.
    std::size_t middle{};
    /// Joint that reaches for #target; below #middle.
    std::size_t end{};
    /// Target position for #end.
    Vec3 target{};
    /// Point that #middle bends toward.
    Vec3 pole{};
    /// Blend toward the solved pose, in [0, 1].
    float weight = 1;
    /// Smallest interior angle at #middle, in radians; 0 allows a fully folded limb.
    float minimum_angle = 0;
    /// Largest interior angle at #middle, in radians, at most pi (straight). The limits bound the
    /// reach and never stretch a limb.
    float maximum_angle = std::numbers::pi_v<float>;
    /// Optional orientation for #end.
    std::optional<Quat> end_rotation;
};
/// Result of solve_contact.
struct ContactResult {
    /// Solved and weighted pose.
    EvaluationPose pose;
    /// Distance from the end joint to the target in the final pose.
    float error{};
    /// Distance from the start joint to the middle joint.
    float upper_length{};
    /// Distance from the middle joint to the end joint.
    float lower_length{};
    /// Whether the target distance was within the reach that the angle limits allow, within
    /// `1e-5`.
    bool reachable{};
};
/// Rotates a two-bone chain so its end reaches toward the target, bending toward the pole.
///
/// The start and middle joints rotate rigidly with their whole subtrees, so limb lengths and
/// affine stretch are preserved; an unreachable target is approached as far as the limits allow.
/// TwoBoneContact::end_rotation then orients the end joint. Only the local matrices of the start
/// and middle joints, and of the end joint with an end rotation, change: each is solved relative
/// to its rig parent and blended from @p pose by the weight (see blend_affine). Every other joint,
/// including one between the chain's joints or a collapsed one, keeps its local matrix from
/// @p pose exactly, and a weight of 0 returns @p pose. Callers order competing contacts.
///
/// Throws for a weight outside [0, 1], a chain that is not ordered and distinct, angle limits
/// that break `0 <= minimum_angle < maximum_angle <= pi`, a nonfinite target or pole, or a limb
/// shorter than `1e-6`. With a weight above 0, also throws when a joint whose local matrix changes
/// has a collapsed rig parent, and for an end rotation of a collapsed end joint.
[[nodiscard]] ContactResult solve_contact(const EvaluationRig &rig, const EvaluationPose &pose,
                                          const TwoBoneContact &contact);
} // namespace anima
