#pragma once
#include <algorithm>
#include <anima/assets/action.hpp>
#include <anima/assets/motion_runtime.hpp>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

/// @file
/// Action catalogs bound to a MotionRuntime: per-phase pose layers, prop tracks and contact
/// weights over an ActionTimeline.
///
/// Part of the `anima::assets` target. Documents are UTF-8 JSON of at most 4 MiB and 64 nesting
/// levels; duplicate and unknown fields are rejected. Invalid documents and arguments, including
/// JSON syntax errors and values of the wrong JSON type, throw `std::invalid_argument` unless
/// stated. An action id argument that the runtime lacks throws `std::out_of_range`.
///
/// The runtime checks what an action declares, its handling profiles and required roles, and
/// evaluates it on the caller's clock. The caller chooses the action and the profile that performs
/// it, and owns any rule that relates the action's phases to its own timing.

namespace anima {
/// One evaluation of an action, on the caller's clock. A caller that reports cues keeps the
/// performance's identity itself and passes it to ActionCueCursor::advance.
struct ActionRequest {
    /// Action id.
    std::string action;
    /// Seconds since the action started.
    double elapsed{};
    /// Release time of a held action, on the same clock.
    std::optional<double> released_at{};
    /// Seconds that a timed action should last; its phases are rescaled to fit. Held actions keep
    /// their declared phase timing and reject this.
    std::optional<double> duration{};
};
/// One key of an ActionWeight.
struct ActionWeightKey {
    /// Phase progress, in [0, 1].
    double progress{};
    /// Weight at #progress, in [0, 1].
    float weight{};
};
/// Piecewise-linear weight curve over phase progress. Immutable after construction; const member
/// functions may run concurrently on any thread.
class ActionWeight {
  public:
    /// Largest number of keys.
    static constexpr std::size_t maximum_keys = 32;
    /// A constant weight of 1: the keys (0, 1) and (1, 1).
    ActionWeight() = default;
    /// Takes ownership of @p keys. Throws `std::invalid_argument` ("Action weight requires 2..32
    /// keys covering 0..1") unless there are 2 to #maximum_keys keys whose first progress is
    /// exactly 0 and whose last is exactly 1, and ("Action weight keys must be ordered and
    /// normalized") unless every progress is finite and strictly greater than the one before it
    /// and every weight is finite in [0, 1].
    explicit ActionWeight(std::vector<ActionWeightKey> keys) : keys_(std::move(keys)) {
        if (keys_.size() < 2 || keys_.size() > maximum_keys || keys_.front().progress != 0 ||
            keys_.back().progress != 1)
            throw std::invalid_argument("Action weight requires 2..32 keys covering 0..1");
        double previous = -1;
        for (const auto &[progress, weight] : keys_) {
            if (!std::isfinite(progress) || progress <= previous || !std::isfinite(weight) || weight < 0 || weight > 1)
                throw std::invalid_argument("Action weight keys must be ordered and normalized");
            previous = progress;
        }
    }
    /// Weight at @p progress, interpolated linearly between the keys around it; finite progress
    /// outside [0, 1] clamps to the end keys. Throws `std::invalid_argument` ("Action weight
    /// progress must be finite") for a nonfinite @p progress.
    [[nodiscard]] float sample(double progress) const {
        if (!std::isfinite(progress))
            throw std::invalid_argument("Action weight progress must be finite");
        for (std::size_t i = 1; i < keys_.size(); ++i)
            if (progress <= keys_[i].progress) {
                const auto &a = keys_[i - 1];
                const auto &b = keys_[i];
                return a.weight +
                       (b.weight - a.weight) *
                           static_cast<float>(std::clamp((progress - a.progress) / (b.progress - a.progress), 0., 1.));
            }
        return keys_.back().weight;
    }
    /// The keys, in increasing progress; valid while the curve lives.
    [[nodiscard]] std::span<const ActionWeightKey> keys() const noexcept { return keys_; }

  private:
    std::vector<ActionWeightKey> keys_{{0., 1.F}, {1., 1.F}};
};
/// Two normalized positions that a phase maps linearly onto its progress. ActionRuntime decodes
/// each in [0, 1]; an interval whose #begin exceeds its #end runs backward.
struct NormalizedInterval {
    /// Position at phase progress 0.
    double begin = 0;
    /// Position at phase progress 1.
    double end = 1;
};
/// One pose layer of an action phase.
struct ActionLayer {
    /// Base clip or layer clip of the motion to sample.
    std::string clip;
    /// Motion mask that the layer affects; empty for a full-body layer, which must be the phase's
    /// first layer and an override.
    std::string mask;
    /// Normalized clip times mapped over the phase; a reversed interval plays backward.
    NormalizedInterval interval;
    /// Layer weight over phase progress.
    ActionWeight weight;
    anima::LayerMode mode = anima::LayerMode::override_pose;
    /// Reference clip of an additive layer; empty for an override.
    std::string reference;
    /// Normalized time in the reference clip, in [0, 1].
    double reference_at{};
};
/// An animated prop track that an action phase drives.
struct ActionPropTrack {
    /// Attachment role whose prop plays the track; unique within the phase.
    std::string role;
    /// Semantic track name, resolved through AttachmentVisual::animation_tracks.
    std::string track;
    /// Normalized track progress mapped over the phase.
    NormalizedInterval interval;
    /// Whether the action requires the role's visual to have the track; see
    /// validate_attachment_action.
    bool required = true;
};
/// Layers, prop tracks and contact curves of one phase.
struct ActionPhaseBinding {
    /// 1 to 8 pose layers, applied in order.
    std::vector<ActionLayer> layers;
    /// At most 8 prop tracks.
    std::vector<ActionPropTrack> props;
    /// Weight curve per motion contact chain.
    std::map<std::string, ActionWeight, std::less<>> contacts;
};
/// One decoded action.
struct ActionDefinition {
    std::string id;
    anima::ActionTimeline timeline;
    /// Bindings parallel to the timeline's phases.
    std::vector<ActionPhaseBinding> phases;
    /// Handling profiles that can perform the action.
    std::set<std::string, std::less<>> handling;
    /// Attachment roles the action requires, each with the handling profiles it accepts.
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>> required_roles;
};
/// Result of ActionRuntime::sample.
struct ActionSample {
    /// Pose after every layer of the current phase: the base pose exactly when every layer has
    /// weight 0, and the pose of the full-body layer's clip exactly when that layer has weight 1 and
    /// every other layer weight 0; otherwise world-only.
    anima::Pose pose;
    /// Position on the action's declared timeline: ActionTimeline::sample at
    /// ActionRequest::elapsed and ActionRequest::released_at times ActionRuntime::scale(), so
    /// ActionTime::elapsed and ActionTime::start are in the declared seconds that
    /// ActionTimeline::cues and ActionCueCursor use, not the caller's.
    anima::ActionTime clock;
    /// Clip of the current phase's first layer.
    std::string clip;
    /// Progress of one prop track.
    struct Prop {
        std::string role;
        std::string track;
        /// Normalized track progress.
        double progress{};
        bool required{};
    };
    std::vector<Prop> props;
    /// Weight per contact chain at the current phase progress.
    std::map<std::string, float, std::less<>> contacts;
};
/// An action catalog bound to one MotionRuntime. Immutable after construction; copies share it.
/// Const member functions may run concurrently on any thread.
class ActionRuntime {
  public:
    using Definitions = std::map<std::string, ActionDefinition, std::less<>>;
    /// Decodes @p document for @p motion, which it retains; the actions pose the model @p motion is
    /// bound to.
    ///
    /// The document has the integer `version` 2, checked before any other field ("Unsupported action
    /// catalog version" otherwise), and 1 to 4096 `actions`. Each action has a unique `id`, a
    /// nonempty list of unique `handling` profiles, `roles` (at most 8, each role mapped to a
    /// nonempty list of unique handling profiles) and `phases`. Each phase has `id`, `duration`,
    /// `held`, 1 to 8 `layers`, `cues` (`id` and `at`), at most 8 `props` (`role`, `track`,
    /// `interval` and `required`) and `contacts` (chain name to weight curve); phases and cues
    /// follow the ActionTimeline rules. Each layer has `clip`, `mask` (a mask name, or null for a
    /// full-body layer), `interval`, `mode` (`"override"` or `"additive"`), `weight` and
    /// `reference` (`clip` and `at`), which an additive layer needs and an override has as null.
    /// Every field is required, with `[]` or `{}` for an empty list or map. Intervals are two
    /// numbers in [0, 1]; weight curves are as in weight(). Masks must exist in @p motion, and a
    /// layer that samples a layer clip must use that clip's mask.
    ///
    /// Clips and contact chains must also exist in @p motion. Throws also for a null @p motion.
    ///
    /// May run concurrently on any thread. Reads @p document and the C locale, which must not change
    /// during the call, as prefab.hpp describes for documents.
    ActionRuntime(std::shared_ptr<const MotionRuntime> motion, std::string_view document);
    /// Action @p id. Throws `std::out_of_range` for an unknown id.
    [[nodiscard]] const ActionDefinition &definition(std::string_view id) const;
    /// Checks @p roles, which maps attachment roles to handling profiles, against the roles that
    /// action @p id requires: each must be present with a profile the action accepts for it.
    /// Roles the action does not require are ignored.
    ///
    /// The runtime does not choose the handling profile that performs the action; the caller
    /// passes its choice to sample(). Throws `std::out_of_range` for an unknown action, and
    /// `std::invalid_argument` naming the first unmet role in role-name order.
    void validate_roles(std::string_view id, const std::map<std::string, std::string, std::less<>> &roles) const;
    /// Every action, by id.
    [[nodiscard]] const Definitions &definitions() const;
    /// Playback rate for @p request: 1, or the action's duration divided by
    /// ActionRequest::duration when that is set. Throws `std::out_of_range` for an unknown action,
    /// and `std::invalid_argument` for a duration that is not positive and finite or that is set for
    /// a held action.
    [[nodiscard]] double scale(const ActionRequest &request) const;
    /// Evaluates @p request over @p base, a pose of the motion's model.
    ///
    /// The timeline is sampled at ActionRequest::elapsed and ActionRequest::released_at times
    /// scale(). Each layer of the current phase then samples its clip at the interval position
    /// for the phase progress, weighted by its curve. Layers at weight 0 are skipped, and a
    /// full-body layer at weight 1 replaces @p base with its clip's pose (see
    /// MotionRuntime::sample). The other layers apply in order in one MotionRuntime::evaluate of
    /// that pose, a full-body layer through an empty mask, which blends it as MotionRuntime::blend
    /// does. So a world-only @p base that they apply to fails as that function states for its
    /// source, such as below a collapsed joint. Prop track progress and contact weights are reported
    /// for the same progress. @p handling is the caller's choice among ActionDefinition::handling;
    /// throws also when it is not one of them.
    [[nodiscard]] ActionSample sample(const Pose &base, const ActionRequest &request, std::string_view handling) const;
    /// Decodes a JSON weight curve: 2 to 32 [progress, weight] pairs, both in [0, 1], with
    /// progress strictly increasing from 0 to 1. A value that is not an array of at most 32 keys
    /// throws as an ActionWeight with too many keys does, a key that is not two numbers in [0, 1]
    /// throws before the curve is built, and the decoded keys are then checked by the ActionWeight
    /// constructor. May run concurrently on any thread; reads the C locale as the constructor does.
    [[nodiscard]] static ActionWeight weight(std::string_view document);

  private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};
} // namespace anima
