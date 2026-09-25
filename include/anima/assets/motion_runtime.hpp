#pragma once
#include <anima/assets/evaluation.hpp>
#include <anima/assets/preview.hpp>
#include <map>
#include <span>
namespace anima {
struct MotionLayer {
    std::string clip, mask;
    double time{};
    float weight = 1;
    anima::LayerMode mode = anima::LayerMode::override_pose;
    std::string reference_clip;
    double reference_time{};
};
struct JointOffset {
    std::string joint;
    anima::Mat4 delta = anima::identity();
    float weight = 1;
};
struct MotionContact {
    std::string chain;
    anima::Vec3 target{}, pole{};
    float weight = 1;
    std::optional<anima::Quat> end_rotation;
};
struct MotionControls {
    // Explicit application order: layers, local offsets, then contacts.
    // These are presentation controls; they do not move the gameplay actor.
    std::vector<MotionLayer> layers;
    std::vector<JointOffset> offsets;
    std::vector<MotionContact> contacts;
    bool empty() const { return layers.empty() && offsets.empty() && contacts.empty(); }
};
struct MotionEvaluation {
    anima::Pose pose;
    struct Contact {
        std::string chain;
        float error;
        bool reachable;
        float weight = 1;
    };
    std::vector<Contact> contacts;
};
class MotionRuntime {
  public:
    MotionRuntime(std::shared_ptr<const Asset> asset, const Manifest &manifest, std::string_view contract);
    static std::shared_ptr<const MotionRuntime> load(std::shared_ptr<const Asset> asset, const Manifest &manifest);
    const EvaluationRig &rig() const;
    bool has_mask(std::string_view name) const;
    std::size_t contact_end_node(std::string_view chain) const;
    bool contact_affects_node(std::string_view chain, std::size_t node) const;
    bool contacts_overlap(std::string_view first, std::string_view second) const;
    const Animation &clip(std::string_view name) const;
    const ClipMetadata &metadata(std::string_view name) const;
    const std::map<std::string, ClipMetadata, std::less<>> &clips() const;
    bool is_layer(std::string_view name) const;
    const std::string &layer_mask(std::string_view name) const;
    Pose sample(std::string_view name, double time) const;
    Pose compose(std::string_view motion, double time, std::string_view carry) const;
    void validate_carries(std::span<const std::string_view> carries) const;
    Pose compose_loadout(std::string_view motion, double time, std::span<const std::string_view> carries) const;
    Pose blend(const Pose &from, const Pose &to, float weight) const;
    MotionEvaluation evaluate(const Pose &source, const MotionControls &controls) const;

  private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};
} // namespace anima
