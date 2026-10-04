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
/// GLB holds geometry and a skin, and the motion GLB (see load_motion_asset) holds nodes and clips
/// only; clips that the model carries itself are not used. A version 4 motion contract (a JSON
/// document; see Manifest::motion_contract) declares the evaluation joints, masks, contact chains,
/// base clips and layer clips.
/// MotionRuntime checks node identity, ancestry and bind pose before transferring motion onto the
/// model. Evaluated poses are presentation only; they never move a gameplay actor. Callers
/// supply rig recipes, the clips and layers to evaluate, any per-call budget and gameplay rules.
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
    /// Base clip or layer clip to sample.
    std::string clip;
    /// Contract mask that the layer affects, or empty for every evaluation joint; a layer clip must
    /// use its declared mask.
    std::string mask;
    /// Seconds into #clip.
    double time{};
    /// Scale of the mask weights, in [0, 1].
    float weight = 1;
    anima::LayerMode mode = anima::LayerMode::override_pose;
    /// Clip whose pose an additive layer is relative to; required for an additive layer and empty
    /// for an override.
    std::string reference_clip{};
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
/// Controls for MotionRuntime::evaluate, applied in the order layers, offsets, contacts.
///
/// The counts are the caller's: evaluation has no fixed limit, and its work grows linearly with
/// them, each layer sampling one or two clips and each contact solving one chain over the rig.
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
/// A motion contract bound to a model. Immutable after construction; copies share it. Const member
/// functions may run concurrently on any thread.
class MotionRuntime {
  public:
    /// Binds the motion contract @p contract to @p asset, the model of @p manifest. The runtime holds
    /// @p asset, and with it the images of its textures; TexelRetention describes how to let them go.
    ///
    /// The contract has the integer `version` 4, checked before any other field ("Unsupported motion
    /// contract version" otherwise); `skeleton` (`id`, `bind_signature` and `joint_count`, equal
    /// to the manifest's); `resource`, a relative `.glb` path without `..`, resolved beside the
    /// manifest's contract file; and `evaluation` with `version` 1, a nonempty `id`, `parents` (one
    /// entry per manifest joint: joint name to its parent's name or null), `masks` (name to a
    /// nonempty list of unique root joints; a mask covers their subtrees) and `chains` (name to
    /// `joints` [start, middle, end], `minimum_angle` and `maximum_angle`, as in TwoBoneContact).
    /// `clips` lists the base clips: unique nonempty `name`, `loop`, `reference_speed` (positive and
    /// finite, or null for none) and `events` (`time` and nonempty `name`, in nondecreasing time
    /// within the clip). Each base clip must have a positive duration in the motion GLB, so a pose
    /// clip, whose keys all sit at time 0, throws "Motion requires a positive duration". `layers`
    /// maps each layer clip to its `mask`, `owned_joints` and `context_joints`. Every field is
    /// required.
    ///
    /// Clips of the model and of the manifest are ignored. Each joint name must name one model
    /// node and one motion node, and every motion node must match a uniquely named model node with
    /// the same parent name and a rest world matrix within `1e-5`. A layer clip's `owned_joints`
    /// must list exactly its mask's joints and `context_joints` their parents outside the mask,
    /// and the clip may animate only those joints. Every motion clip must be exactly one base clip
    /// or layer clip, with at least one base clip.
    ///
    /// May run concurrently on any thread. Reads @p contract, the motion GLB file and the C locale,
    /// which must not change during the call, as load_motion_asset and prefab.hpp describe.
    MotionRuntime(std::shared_ptr<const Asset> asset, const Manifest &manifest, std::string_view contract);
    /// Reads the file that Manifest::motion_contract names beside the manifest and constructs a
    /// runtime from it. Throws `std::invalid_argument` when the manifest names no contract or the
    /// file is missing or larger than 4 MiB. May run concurrently on any thread, and reads the C
    /// locale as the constructor does.
    static std::shared_ptr<const MotionRuntime> load(std::shared_ptr<const Asset> asset, const Manifest &manifest);
    /// The model bound at construction, which the runtime holds; not null, and shared with every
    /// copy of the runtime. sample(), compose() and the other evaluations return poses of its nodes.
    [[nodiscard]] const std::shared_ptr<const Asset> &model() const noexcept;
    /// Evaluation rig built from the contract's `parents`; joint names are model node names.
    const EvaluationRig &rig() const;
    /// Whether the contract declares mask @p name.
    bool has_mask(std::string_view name) const;
    /// Model node of the end joint of chain @p chain. Throws `std::out_of_range` for an unknown
    /// chain.
    std::size_t contact_end_node(std::string_view chain) const;
    /// Whether solving chain @p chain can move model node @p node: whether the nearest evaluation
    /// joint at or above the node in the model's hierarchy is at or below the chain's start joint in
    /// the evaluation rig, whose hierarchy may differ. A node that is not an evaluation joint, such
    /// as a socket's helper node, follows its model parent (see EvaluationRig::render_pose), so a
    /// node with no evaluation joint at or above it never moves. The model's nodes and hierarchy are
    /// those the asset had at construction. Throws `std::out_of_range` for an unknown chain or a
    /// node index past the model.
    bool contact_affects_node(std::string_view chain, std::size_t node) const;
    /// Whether one chain's start joint is at or below the other's. Throws `std::out_of_range` for an
    /// unknown chain.
    bool contacts_overlap(std::string_view first, std::string_view second) const;
    /// Motion clip @p name, a base clip or layer clip. Throws `std::out_of_range` for an unknown
    /// name.
    const Animation &clip(std::string_view name) const;
    /// Metadata of base clip @p name. Throws `std::out_of_range` for layer clips and unknown names.
    const ClipMetadata &metadata(std::string_view name) const;
    /// Base clips by name, without layer clips.
    const std::map<std::string, ClipMetadata, std::less<>> &clips() const;
    /// Whether @p name is a layer clip.
    bool is_layer(std::string_view name) const;
    /// Mask of layer clip @p name. Throws `std::out_of_range` for other names.
    const std::string &layer_mask(std::string_view name) const;
    /// Model pose with clip @p name sampled at @p time seconds, clamped to the clip rather than
    /// looped. Model nodes that the motion lacks keep their rest transforms.
    Pose sample(std::string_view name, double time) const;
    /// Samples base clip @p motion at @p time and overrides the mask of layer clip @p layer with
    /// that clip, sampled at the same fraction of its duration. An empty @p layer returns the
    /// sampled pose, with local transforms; otherwise the result is world-only.
    Pose compose(std::string_view motion, double time, std::string_view layer) const;
    /// Checks that the masks of the nonempty layer clips in @p layers share no joint. Throws
    /// `std::invalid_argument` for overlapping masks and `std::out_of_range` for an unknown layer.
    void validate_layers(std::span<const std::string_view> layers) const;
    /// compose() with several layer clips, applied in order after validate_layers(); empty names
    /// are skipped.
    Pose compose_layers(std::string_view motion, double time, std::span<const std::string_view> layers) const;
    /// Blends two model poses by @p weight in [0, 1] over the evaluation rig. A weight of 0 or 1
    /// returns that input unchanged; otherwise the result is world-only and unmapped nodes follow
    /// @p to.
    Pose blend(const Pose &from, const Pose &to, float weight) const;
    /// Applies @p controls to @p source: each layer, then each offset, then each contact, solved on
    /// the result so far. A layer with an empty mask covers every evaluation joint.
    ///
    /// Empty controls return @p source unchanged; otherwise the pose is world-only. Nodes that are
    /// not evaluation joints keep their transforms relative to their parents (see
    /// EvaluationRig::render_pose) from the sampled pose of the last override layer with an empty
    /// mask and a weight above 0, as blend() takes them from its target, or from @p source when no
    /// layer is one. So controls holding only an override of base clip `c` at time `t`, with an
    /// empty mask and a weight `w` in (0, 1), give the pose that `blend(source, sample(c, t), w)`
    /// does.
    ///
    /// With nonempty controls, a world-only @p source, such as an earlier result, fails where the
    /// evaluation parent of a joint, or the asset parent of a node that is not an evaluation joint
    /// and follows @p source, is collapsed, since only local transforms recover what lies below it
    /// (see EvaluationRig::encode and EvaluationRig::render_pose). Throws for an invalid weight, a
    /// layer clip on another mask, an override with a reference clip or an additive layer without
    /// one, as EvaluationRig::encode and EvaluationRig::render_pose do for @p source and
    /// solve_contact for each contact, and `std::out_of_range` for an unknown clip, mask, chain or
    /// joint.
    MotionEvaluation evaluate(const Pose &source, const MotionControls &controls) const;

  private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};
} // namespace anima
