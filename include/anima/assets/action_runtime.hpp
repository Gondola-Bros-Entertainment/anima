#pragma once
#include <anima/assets/action.hpp>
#include <anima/assets/motion_runtime.hpp>
#include <anima/core/capabilities.hpp>

/// @file
/// Action catalogs bound to a MotionRuntime: per-phase pose layers, prop tracks and contact
/// weights over an ActionTimeline.
///
/// Part of the `anima::assets` target. Documents are UTF-8 JSON of at most 4 MiB and 64 nesting
/// levels; duplicate and unknown fields are rejected. Invalid documents and arguments, including
/// JSON syntax errors and values of the wrong JSON type, throw `std::invalid_argument` unless
/// stated. An action id argument that the runtime lacks throws `std::out_of_range`.

namespace anima {
/// One evaluation of an action instance, on the caller's authoritative clock.
struct ActionRequest {
    /// Action id.
    std::string action;
    /// Nonzero identity of this performance.
    std::uint64_t instance{};
    /// Seconds since the action started.
    double elapsed{};
    /// Release time of a held action, on the same clock.
    std::optional<double> released_at;
    /// Seconds that a timed action should last; its phases are rescaled to fit. Held actions keep
    /// their declared phase timing and reject this.
    std::optional<double> duration;
};
/// Piecewise-linear weight curve over phase progress.
struct ActionWeight {
    /// Largest number of #keys.
    static constexpr std::size_t maximum_keys = 32;
    /// (phase, weight) keys: 2 to #maximum_keys, phases strictly increasing from exactly 0 to exactly
    /// 1, and weights finite in [0, 1]. The default is a constant 1.
    std::vector<std::pair<double, float>> keys{{0., 1.F}, {1., 1.F}};
    /// Weight at @p phase; finite phases outside [0, 1] clamp to the end keys. Validates #keys on
    /// every call, so curves built directly rather than decoded are checked too. Throws for a
    /// nonfinite @p phase or invalid keys.
    float sample(double phase) const {
        if (!std::isfinite(phase) || keys.size() < 2 || keys.size() > maximum_keys || keys.front().first != 0 ||
            keys.back().first != 1)
            throw std::invalid_argument("Action weight requires a finite phase and 2..32 keys covering 0..1");
        double previous = -1;
        for (const auto &[at, value] : keys) {
            if (!std::isfinite(at) || at <= previous || at < 0 || at > 1 || !std::isfinite(value) || value < 0 ||
                value > 1)
                throw std::invalid_argument("Action weight keys must be ordered and normalized");
            previous = at;
        }
        for (std::size_t i = 1; i < keys.size(); ++i)
            if (phase <= keys[i].first) {
                const auto [a, x] = keys[i - 1];
                const auto [b, y] = keys[i];
                return x + (y - x) * static_cast<float>(std::clamp((phase - a) / (b - a), 0., 1.));
            }
        return keys.back().second;
    }
};
/// One pose layer of an action phase.
struct ActionLayer {
    /// Base clip or handling layer of the motion to sample.
    std::string clip;
    /// Motion mask that the layer affects; empty for a full-body layer, which must be the phase's
    /// first layer and an override.
    std::string mask;
    /// Normalized clip times, each in [0, 1], mapped linearly over the phase; a reversed interval
    /// plays backward.
    std::array<double, 2> interval{0, 1};
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
    /// Normalized track progress, each in [0, 1], mapped linearly over the phase.
    std::array<double, 2> interval{0, 1};
    /// Whether the action requires the role to carry the track; see validate_attachment_action.
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
    std::map<std::string, Capabilities, std::less<>> required_roles;
};
/// Result of ActionRuntime::sample.
struct ActionSample {
    /// Pose after every layer of the current phase; world-only unless each layer was a full-body
    /// layer at weight 0 or 1.
    anima::Pose pose;
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
class ActionRuntime {
  public:
    using Definitions = std::map<std::string, ActionDefinition, std::less<>>;
    /// Decodes @p document for @p motion, which it retains; the actions pose the model @p motion is
    /// bound to.
    ///
    /// The document has `schema_version` 1 and 1 to 4096 `actions`. Each action has a unique
    /// `id`, a nonempty list of unique `handling` profiles, `phases`, and optional `roles` (at most
    /// 8, each role mapped to a nonempty list of unique handling profiles). Each phase has `id`,
    /// `duration` and 1 to 8 `layers`, and optional `held`, `cues` (`id` and `at`), at most 8
    /// `props` (`role`, `track`, `interval` and optional `required`) and `contacts` (chain name
    /// to weight curve); phases and cues follow the ActionTimeline rules. Each layer has `clip`
    /// and `interval`, and optional `mask`, `mode` (`"override"`, the default, or `"additive"`),
    /// `weight` and `reference` (`clip` and `at`), which additive layers need and overrides must
    /// not have. Intervals are two numbers in [0, 1]; weight curves are as in weight(). Masks must
    /// exist in @p motion, and a handling layer's clip must use its declared mask.
    ///
    /// Clips and contact chains must also exist in @p motion. Throws also for a null @p motion.
    ActionRuntime(std::shared_ptr<const MotionRuntime> motion, std::string_view document);
    /// Action @p id. Throws `std::out_of_range` for an unknown id.
    const ActionDefinition &definition(std::string_view id) const;
    /// Checks the caller's timing and handling for action @p id.
    ///
    /// @p handling must be one of its profiles, @p fixed_duration positive and finite, and
    /// @p windup present, finite and nonnegative exactly when the action is held. For a held
    /// action, the timed phases before the held phase must total @p windup and all timed phases
    /// @p fixed_duration, each within `1e-6` seconds.
    void validate_timing(std::string_view id, std::string_view handling, std::optional<double> windup,
                         double fixed_duration) const;
    /// Handling profile that performs action @p id with @p roles, which maps each attachment role
    /// to its handling profile.
    ///
    /// Every role the action requires must be present with an accepted profile. With no roles,
    /// returns @p empty_handling when the action accepts it; otherwise returns the first accepted
    /// profile in role-name order. Throws when no profile qualifies.
    std::string loadout_handling(std::string_view id, const std::map<std::string, std::string, std::less<>> &roles,
                                 std::string_view empty_handling) const;
    /// Every action, by id.
    const Definitions &definitions() const;
    /// Playback rate for @p request: 1, or the action's duration divided by
    /// ActionRequest::duration when that is set. Throws `std::out_of_range` for an unknown action,
    /// and for a zero instance or a duration that is not positive and finite or is set for a held
    /// action.
    double scale(const ActionRequest &request) const;
    /// Evaluates @p request over @p base, a pose of the motion's model.
    ///
    /// The timeline is sampled at ActionRequest::elapsed and ActionRequest::released_at times
    /// scale(). Each layer of the current phase then samples its clip at the interval position
    /// for the phase progress, weighted by its curve: full-body layers blend with
    /// MotionRuntime::blend and masked layers apply through MotionRuntime::evaluate. Prop track
    /// progress and contact weights are reported for the same progress. Throws also when
    /// @p handling is not one of the action's profiles.
    ActionSample sample(const Pose &base, const ActionRequest &request, std::string_view handling) const;
    /// Decodes a JSON weight curve: 2 to 32 [phase, weight] pairs, both in [0, 1], with phases
    /// strictly increasing from 0 to 1.
    static ActionWeight weight(std::string_view document);

  private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};
} // namespace anima
