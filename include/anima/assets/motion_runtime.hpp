#pragma once
#include <anima/assets/evaluation.hpp>
#include <anima/assets/preview.hpp>
#include <map>
#include <span>

/// @file
/// Independent motion: clips from a separate motion GLB bound to a model, evaluated with masks,
/// layers, joint offsets and two-bone contacts.
///
/// Part of the `anima::assets` target. The model and its motion are separate resources: the model
/// GLB holds geometry and a skin but no clips, and the motion GLB (see load_motion_asset) holds
/// nodes and clips only. A version 3 motion contract (a JSON document; see
/// Manifest::motion_contract) declares the evaluation joints, masks, contact chains, base clips
/// and handling layers.
/// MotionRuntime checks node identity, ancestry and bind pose before transferring motion onto the
/// model. Evaluated poses are presentation only; they never move a gameplay actor. Callers
/// supply rig recipes, clip choice and gameplay rules.
///
/// Contracts are UTF-8 JSON of at most 4 MiB and 64 nesting levels; duplicate and unknown fields
/// are rejected at every level. Invalid contracts and arguments, including JSON syntax errors,
/// missing fields, values of the wrong JSON type and names that match no node, joint, clip, mask or
/// chain or several nodes, throw `std::invalid_argument` unless stated. A clip, layer, mask, chain
/// or joint name argument that the runtime lacks throws `std::out_of_range`, and motion GLB
/// failures throw `std::runtime_error`.

namespace anima {
/// One layer for MotionRuntime::evaluate.
struct MotionLayer {
    /// Base clip or handling layer to sample.
    std::string clip;
    /// Contract mask that the layer affects; a handling layer must use its declared mask.
    std::string mask;
    /// Seconds into #clip.
    double time{};
    /// Scale of the mask weights, in [0, 1].
    float weight = 1;
    anima::LayerMode mode = anima::LayerMode::override_pose;
    /// Clip whose pose an additive layer is relative to; empty for an override.
    std::string reference_clip;
    /// Seconds into #reference_clip.
    double reference_time{};
};
/// An extra transform on one evaluation joint.
struct JointOffset {
    /// Evaluation joint name.
    std::string joint;
    /// Affine transform applied in the joint's local frame, after its local matrix.
    anima::Mat4 delta = anima::identity();
    /// Blend from identity toward #delta, in [0, 1].
    float weight = 1;
};
/// A two-bone contact request for a contract chain; see TwoBoneContact.
struct MotionContact {
    /// Contract chain name.
    std::string chain;
    /// Model-space target for the chain's end joint.
    anima::Vec3 target{};
    /// Model-space point that the chain's middle joint bends toward.
    anima::Vec3 pole{};
    /// In [0, 1].
    float weight = 1;
    /// Optional model-space orientation for the chain's end joint.
    std::optional<anima::Quat> end_rotation;
};
/// Controls for MotionRuntime::evaluate: at most 8 layers, 32 offsets and 8 contacts, applied in
/// that order.
struct MotionControls {
    std::vector<MotionLayer> layers;
    std::vector<JointOffset> offsets;
    std::vector<MotionContact> contacts;
    /// Whether there are no controls, in which case evaluation returns its source unchanged.
    bool empty() const { return layers.empty() && offsets.empty() && contacts.empty(); }
};
/// Result of MotionRuntime::evaluate.
struct MotionEvaluation {
    /// Evaluated pose; world-only unless the controls were empty.
    anima::Pose pose;
    /// Outcome of one contact solve; see ContactResult.
    struct Contact {
        std::string chain;
        /// Distance from the end joint to the target after weighting.
        float error{};
        /// Whether the target was within the chain's reach.
        bool reachable{};
        /// Requested weight.
        float weight = 1;
    };
    /// One entry per solved contact, in solve order.
    std::vector<Contact> contacts;
};
/// A motion contract bound to a model. Immutable after construction; copies share it.
class MotionRuntime {
  public:
    /// Binds the motion contract @p contract to @p asset, the model of @p manifest.
    ///
    /// The contract has `version` 3; `skeleton` (`id`, `bind_signature` and `joint_count`, equal
    /// to the manifest's); `resource`, a relative `.glb` path without `..`, resolved beside the
    /// manifest's contract file; and `evaluation` with `version` 1, a nonempty `id`, `parents` (one
    /// entry per manifest joint: joint name to its parent's name or null), `masks` (name to a
    /// nonempty list of unique root joints; a mask covers their subtrees) and `chains` (name to
    /// `joints` [start, middle, end], `minimum_angle` and `maximum_angle`, as in TwoBoneContact).
    /// `clips` lists the base clips: unique nonempty `name`, `loop`, `events` (`time` and nonempty
    /// `name`, in nondecreasing time within the clip) and optional positive finite
    /// `reference_speed`. `layers` maps each handling layer to its `mask`, `owned_joints` and
    /// `context_joints`.
    ///
    /// The model and manifest must have no clips. Each joint name must name one model node and one
    /// motion node, and every motion node must match a uniquely named model node with the same
    /// parent name and a rest world matrix within `1e-5`. A layer's `owned_joints` must list
    /// exactly its mask's joints and `context_joints` their parents outside the mask, and its clip
    /// may animate only those joints. Every motion clip must be exactly one base clip or layer,
    /// with at least one base clip.
    MotionRuntime(std::shared_ptr<const Asset> asset, const Manifest &manifest, std::string_view contract);
    /// Reads the file that Manifest::motion_contract names beside the manifest and constructs a
    /// runtime from it. Throws `std::invalid_argument` when the manifest names no contract or the
    /// file is missing or larger than 4 MiB.
    static std::shared_ptr<const MotionRuntime> load(std::shared_ptr<const Asset> asset, const Manifest &manifest);
    /// Evaluation rig built from the contract's `parents`; joint names are model node names.
    const EvaluationRig &rig() const;
    /// Whether the contract declares mask @p name.
    bool has_mask(std::string_view name) const;
    /// Model node of the end joint of chain @p chain. Throws `std::out_of_range` for an unknown
    /// chain.
    std::size_t contact_end_node(std::string_view chain) const;
    /// Whether model node @p node is at or below the start joint of chain @p chain, so that solving
    /// the chain moves it. Throws `std::out_of_range` for an unknown chain or node and
    /// `std::invalid_argument` when the node is not an evaluation joint.
    bool contact_affects_node(std::string_view chain, std::size_t node) const;
    /// Whether one chain's start joint is at or below the other's. Throws `std::out_of_range` for an
    /// unknown chain.
    bool contacts_overlap(std::string_view first, std::string_view second) const;
    /// Motion clip @p name, a base clip or handling layer. Throws `std::out_of_range` for an unknown
    /// name.
    const Animation &clip(std::string_view name) const;
    /// Metadata of base clip @p name. Throws `std::out_of_range` for handling layers and unknown
    /// names.
    const ClipMetadata &metadata(std::string_view name) const;
    /// Base clips by name, without handling layers.
    const std::map<std::string, ClipMetadata, std::less<>> &clips() const;
    /// Whether @p name is a handling layer.
    bool is_layer(std::string_view name) const;
    /// Mask of handling layer @p name. Throws `std::out_of_range` for other names.
    const std::string &layer_mask(std::string_view name) const;
    /// Model pose with clip @p name sampled at @p time seconds, clamped to the clip rather than
    /// looped. Model nodes that the motion lacks keep their rest transforms.
    Pose sample(std::string_view name, double time) const;
    /// Samples base clip @p motion at @p time and overrides the mask of handling layer @p carry with
    /// that layer, sampled at the same fraction of its duration. An empty @p carry returns the
    /// sampled pose, with local transforms; otherwise the result is world-only.
    Pose compose(std::string_view motion, double time, std::string_view carry) const;
    /// Checks that the masks of the nonempty handling layers in @p carries share no joint. Throws
    /// `std::invalid_argument` for overlapping masks and `std::out_of_range` for an unknown layer.
    void validate_carries(std::span<const std::string_view> carries) const;
    /// compose() with several carry layers, applied in order after validate_carries().
    Pose compose_loadout(std::string_view motion, double time, std::span<const std::string_view> carries) const;
    /// Blends two model poses by @p weight in [0, 1] over the evaluation rig. A weight of 0 or 1
    /// returns that input unchanged; otherwise the result is world-only and unmapped nodes follow
    /// @p to.
    Pose blend(const Pose &from, const Pose &to, float weight) const;
    /// Applies @p controls to @p source: each layer, then each offset, then each contact, solved on
    /// the result so far.
    ///
    /// Empty controls return @p source unchanged; otherwise the pose is world-only. Throws beyond
    /// the MotionControls limits, for an invalid weight, a handling layer on another mask or an
    /// override with a reference clip, and `std::out_of_range` for an unknown clip, mask, chain or
    /// joint.
    MotionEvaluation evaluate(const Pose &source, const MotionControls &controls) const;

  private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};
} // namespace anima
