#pragma once
#include <anima/animation.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// Animation state machines: states that play one clip or a one-dimensional blend of clips, and
/// transitions between them with conditions on parameters, exit times and crossfades, run on a
/// GameObject's mesh by the StateMachineAnimator component.
///
/// Part of the `anima::assets` target. The application authors the machine, as a Definition or a
/// document, under its own names, and sets its parameters; Anima evaluates it. Times are in
/// seconds. A state's normalized time starts where the state is entered and counts passes through
/// its clips: each second it grows by the state's rate (see StateMachineAnimator::update), so 1 is
/// one full pass, its whole part counts the passes completed, and it keeps growing after a
/// state that does not loop reaches its end, whose clips then hold their last pose.

namespace anima {
namespace detail {
struct AnimationStateMachineData;
struct StateMachineAnimatorAccess;
// A state that a StateMachineAnimator plays: its normalized time, the time through which exit times
// were checked, and whether the events at its entry point are still to be reported.
struct AnimationStatePlaying {
    std::size_t state{};
    double time{};
    double checked{};
    bool fresh{};
};
// A crossfade: its transition, the state it leaves, and that state while it plays or the pose frozen
// when the crossfade interrupted another.
struct AnimationStateFade {
    std::size_t transition{};
    std::size_t source_state{};
    std::optional<AnimationStatePlaying> source;
    std::shared_ptr<const Pose> frozen;
    double elapsed{};
};
struct AnimationStateRuntime {
    // Parameter values by index: floats and integers as themselves, bools and triggers as 0 or 1.
    std::vector<double> parameters;
    AnimationStatePlaying current;
    std::optional<AnimationStateFade> fade;
};
} // namespace detail
/// A state machine over the clips of one asset: parameters, states and transitions. Immutable after
/// construction; StateMachineAnimator runs it, and animators share one machine through
/// `std::shared_ptr<const AnimationStateMachine>`.
class AnimationStateMachine {
  public:
    /// Type of a Parameter.
    enum class ParameterType {
        real,    ///< A finite float; `"float"` in documents.
        integer, ///< A `std::int32_t`; `"int"` in documents.
        boolean, ///< A bool; `"bool"` in documents.
        trigger  ///< A flag that stays set until a transition that tests it starts; `"trigger"` in documents.
    };
    /// A value that the application sets and that conditions, blends and speeds read.
    struct Parameter {
        /// Nonempty name, unique among the parameters.
        std::string name;
        ParameterType type = ParameterType::real;
        /// Initial value: for a float, a finite number within the float range, rounded to float; for an
        /// integer, a whole number in the `std::int32_t` range; for a bool, 0 (false) or 1 (true); for a
        /// trigger, 0 (unset).
        double initial = 0;
    };
    /// One clip of a Blend.
    struct BlendClip {
        /// Name of the clip.
        std::string clip;
        /// Parameter value at which the clip plays alone; finite.
        float threshold = 0;
    };
    /// A one-dimensional blend of clips positioned by a float parameter.
    ///
    /// A parameter value between two neighboring thresholds blends their clips with blend_pose, by
    /// its fraction of the way from the lower threshold to the upper; at or below the first
    /// threshold the first clip plays alone, and at or above the last the last does. The clips share
    /// the state's normalized time, so they stay in phase, and the state's duration is the mean of
    /// the two blended clips' durations weighted as their poses are.
    struct Blend {
        /// Name of a float parameter.
        std::string parameter;
        /// Two or more clips with strictly increasing thresholds, which all loop or all do not, and
        /// which all have zero duration or all have a positive one.
        std::vector<BlendClip> clips;
    };
    /// A state: one clip, or a blend of clips, played at a rate.
    struct State {
        /// Nonempty name, unique among the states.
        std::string name;
        /// Name of the clip the state plays; empty when #blend is set.
        std::string clip;
        /// Blend the state plays instead of #clip.
        std::optional<Blend> blend;
        /// Rate multiplier, finite and at least 0.
        double speed = 1;
        /// Name of a float parameter that also multiplies the rate, or empty for none. Its value must be at
        /// least 0 whenever the state advances.
        std::string speed_parameter;
    };
    /// How a Condition tests its parameter.
    enum class ConditionMode {
        is_true,  ///< A bool is true, or a trigger is set.
        is_false, ///< A bool is false.
        greater,  ///< A float or integer is greater than the threshold.
        less,     ///< A float or integer is less than the threshold.
        equals,   ///< An integer equals the threshold.
        not_equal ///< An integer differs from the threshold.
    };
    /// A test of one parameter.
    struct Condition {
        /// Name of the parameter.
        std::string parameter;
        /// A mode that the parameter's type supports; triggers support only ConditionMode::is_true.
        ConditionMode mode = ConditionMode::is_true;
        /// Value compared with a float or integer: for a float, a finite number within the float range,
        /// compared in float precision; for an integer, a whole number in the `std::int32_t` range. It
        /// must be 0 for a bool or trigger.
        double threshold = 0;
    };
    /// Which transitions may interrupt a crossfade; see StateMachineAnimator::update.
    enum class Interruption {
        none,                    ///< None: the crossfade runs to its end.
        source,                  ///< Transitions leaving its source state that precede it in priority.
        destination,             ///< Transitions leaving the state it enters.
        source_then_destination, ///< Both, the source's first.
        destination_then_source  ///< Both, the destination's first.
    };
    /// A transition from one state, or from any state, to a state.
    struct Transition {
        /// Name of the state the transition leaves, or empty for a transition from any state.
        std::optional<std::string> from;
        /// Name of the state it enters, which may be #from.
        std::string to;
        /// Conditions that must all hold for it to start. May be empty only with an #exit_time.
        std::vector<Condition> conditions;
        /// Normalized time of #from that it waits for, finite and at least 0; see
        /// StateMachineAnimator::update. A transition from any state has none.
        std::optional<double> exit_time;
        /// Crossfade length in seconds, finite and at least 0; 0 switches states at once.
        double duration = 0;
        /// Normalized time at which #to starts, in [0, 1).
        double offset = 0;
        /// Which transitions may interrupt its crossfade.
        Interruption interruption = Interruption::none;
        /// Whether a transition from any state may start while #to is the state that the machine plays
        /// or is entering. Only a transition from any state may set it.
        bool to_self = false;
    };
    /// Everything the machine evaluates, under the application's names.
    struct Definition {
        /// Parameters in any order.
        std::vector<Parameter> parameters;
        /// One or more states; the machine starts in the first.
        std::vector<State> states;
        /// Transitions. Their priority is their order, with every transition from any state ahead of
        /// every transition from a named state.
        std::vector<Transition> transitions;
    };

    /// Validates @p definition over the clips of @p source and keeps both; @p source is retained.
    ///
    /// @p clips gives the playback policy of the clips that the states play (whether each loops, and
    /// its events), such as Manifest::clips. Each clip that a state names must be named by exactly one
    /// entry of @p clips and by exactly one clip of @p source, whose duration must be finite and at
    /// least 0 with every event inside it, as Playback::select requires; unnamed entries are ignored.
    /// A clip of zero duration, as load_asset imports one whose keys all sit at time 0, is a pose
    /// that its state holds; see StateMachineAnimator::update for its rate and events.
    ///
    /// Throws `std::invalid_argument` for a null @p source, two entries of @p clips with one name, or a
    /// @p definition that breaks a rule stated on its members, including a name that matches no
    /// state, parameter or clip, a parameter of the wrong type, and an enumerator outside its type.
    AnimationStateMachine(std::shared_ptr<const Asset> source, std::span<const ClipMetadata> clips,
                          Definition definition);
    /// Decodes an `anima.animation-state-machine` version 1 document and constructs the machine from it
    /// as the constructor does.
    ///
    /// The document is UTF-8 JSON of at most 4 MiB, nested at most 16 levels deep, with exactly
    /// `version`, `kind`, `parameters`, `states` and `transitions`. Repeated fields are rejected
    /// while parsing; then a `version` other than the integer 1, or another `kind`, before the
    /// other fields; then missing and unknown fields at every level. Each parameter has `name` and
    /// `type` (`"float"`, `"int"`, `"bool"` or `"trigger"`) and, except for a trigger, an optional
    /// `initial`: a number, or a boolean for a bool, 0 or false by default. Each state has `name`,
    /// exactly one of `clip` and `blend` (`parameter`, and `clips`, each with `clip` and
    /// `threshold`), and optional `speed` (1 by default) and `speed_parameter`. Each transition has
    /// `from` (a state name, or null for any state) and `to`, and optional `conditions` (each with
    /// `parameter`, `mode`, named as the ConditionMode enumerators are, and an optional
    /// `threshold`, 0 by default), `exit_time`, `duration` (0 by default), `offset` (0 by default),
    /// `interruption` (named as the Interruption enumerators are, `"none"` by default) and
    /// `to_self` (false by default).
    ///
    /// Throws `std::invalid_argument` for invalid content, including malformed JSON and values of the
    /// wrong JSON type.
    [[nodiscard]] static AnimationStateMachine
    deserialize(std::shared_ptr<const Asset> source, std::span<const ClipMetadata> clips, std::string_view document);
    /// The definition, as validated.
    [[nodiscard]] const Definition &definition() const noexcept { return definition_; }
    /// The asset whose clips the states play.
    [[nodiscard]] const std::shared_ptr<const Asset> &source() const noexcept { return source_; }

  private:
    friend class StateMachineAnimator;
    friend struct detail::StateMachineAnimatorAccess;
    std::shared_ptr<const Asset> source_;
    Definition definition_;
    std::shared_ptr<const detail::AnimationStateMachineData> data_;
};

/// Returns the persistent key of a machine when a StateMachineAnimator is captured.
using AnimationStateMachineName = std::function<std::string(const std::shared_ptr<const AnimationStateMachine> &)>;
/// Returns the machine for a key when a StateMachineAnimator is restored; returning null rejects the
/// payload.
using AnimationStateMachineResolver = std::function<std::shared_ptr<const AnimationStateMachine>(std::string_view)>;

/// A clip event that StateMachineAnimator::update crossed.
struct AnimationStateEvent {
    /// Seconds into the update at which the clip crossed it, from 0 to the update's step.
    double offset{};
    /// Name of the state that played the clip.
    std::string state;
    /// Name of the clip.
    std::string clip;
    /// The event, as its clip's ClipMetadata gives it; its ClipEvent::time is in clip seconds.
    ClipEvent event;
    /// The clip's weight in the pose at #offset: its weight in its state's blend times its state's
    /// crossfade weight, in [0, 1].
    float weight{};
};

/// A crossfade in progress; see StateMachineAnimator::update.
struct AnimationCrossfade {
    /// Index of its transition in AnimationStateMachine::Definition::transitions.
    std::size_t transition{};
    /// Name of the state it leaves: the transition's source, or for a transition from any state, the
    /// state the machine played or was entering when it started.
    std::string source;
    /// Normalized time of #source while the crossfade fades it out as it keeps playing; empty when the
    /// crossfade interrupted another and fades out the pose frozen when it started.
    std::optional<double> source_time;
    /// Seconds since it started, less than the transition's duration.
    double elapsed{};
    /// Weight of the entered state's pose, `elapsed / duration`, in [0, 1).
    float weight{};
};

/// Component that runs an AnimationStateMachine on its GameObject's mesh.
///
/// Like Animator, it retains its machine's source asset, publishes each pose through
/// MeshRenderer::set_pose, advances in on_update during Scene::update when attached as a component
/// and through update() when standalone, and needs one driver. Replacing the object's mesh requires
/// a new StateMachineAnimator: every call that publishes a pose then throws `std::logic_error`.
/// Poses are sampled with sample_pose and blended with blend_pose, so they have local transforms.
class StateMachineAnimator {
  public:
    /// Binds @p machine to the mesh of @p owner's object, with its parameters at their initial values,
    /// enters its first state at normalized time 0 and publishes that pose. Throws
    /// `std::invalid_argument` for a null machine or one whose source asset does not match the mesh, as
    /// Animator requires; fails as GameObject::renderer does when the object has no mesh.
    StateMachineAnimator(ComponentOwner owner, std::shared_ptr<const AnimationStateMachine> machine);
    /// Sets float parameter @p name. Throws `std::out_of_range` for an unknown parameter and
    /// `std::invalid_argument` for a parameter of another type or a nonfinite value.
    void set_float(std::string_view name, float value);
    /// Sets integer parameter @p name. Throws as set_float() does, except for the value.
    void set_integer(std::string_view name, std::int32_t value);
    /// Sets bool parameter @p name. Throws as set_integer() does.
    void set_bool(std::string_view name, bool value);
    /// Sets trigger @p name until a transition that tests it starts, or reset_trigger(). Throws as
    /// set_integer() does.
    void set_trigger(std::string_view name);
    /// Unsets trigger @p name. Throws as set_integer() does.
    void reset_trigger(std::string_view name);
    /// Value of float parameter @p name. Throws as set_integer() does.
    [[nodiscard]] float get_float(std::string_view name) const;
    /// Value of integer parameter @p name. Throws as set_integer() does.
    [[nodiscard]] std::int32_t get_integer(std::string_view name) const;
    /// Value of bool parameter @p name, or whether trigger @p name is set. Throws as set_integer() does
    /// for other types.
    [[nodiscard]] bool get_bool(std::string_view name) const;
    /// Enters state @p name at normalized time @p normalized_time, ending any crossfade, and publishes
    /// its pose. As on entry through a transition, the next update that advances the state reports
    /// the events at its entry point. Parameters are unchanged. Throws `std::out_of_range` for an
    /// unknown state and `std::invalid_argument` for a negative or nonfinite time; the machine is
    /// unchanged on failure.
    void play(std::string_view name, double normalized_time = 0);
    /// Starts at most one transition, advances by @p seconds, publishes the pose and returns the clip
    /// events crossed, ordered by AnimationStateEvent::offset.
    ///
    /// First, transitions are checked in priority order, and the first whose conditions all hold,
    /// and that has no exit time or whose source state crossed its exit time, starts. Outside a
    /// crossfade the candidates are the transitions from any state, then the transitions from the
    /// current state. During a crossfade they are those its transition's Interruption admits,
    /// except that transition itself: with `source`, the transitions from any state that precede it
    /// and then those from its source state that precede it; with `destination`, the transitions
    /// from any state and then those from the state it enters; with the two combined, both lists in
    /// the stated order, each transition once. A transition from any state is skipped while its
    /// target is the state the machine plays or is entering, unless Transition::to_self is set. A
    /// state crosses exit time `e` when its normalized time passes `e`, having been below it, during
    /// the advances since the state was entered or transitions were last checked; a looping state
    /// also crosses `e + k` for each whole `k` when `e` is below 1. A transition with an exit time
    /// can start only while its source state plays, not after a crossfade froze it.
    ///
    /// A starting transition resets the triggers its conditions test and enters its target at
    /// Transition::offset. With a zero duration the machine then plays the target alone. Otherwise the
    /// pose crossfades for Transition::duration seconds from the outgoing pose to the target's,
    /// blended with blend_pose by `elapsed / duration`, where elapsed counts the seconds advanced
    /// since the start. The outgoing pose is the state the machine played, which keeps playing and
    /// reporting events until the crossfade ends, or, when the transition interrupts a crossfade,
    /// the pose last published, frozen, so the pose stays continuous.
    ///
    /// Then every state that plays advances its normalized time by @p seconds times its rate:
    /// State::speed, times its speed parameter, divided by its duration, which is its clip's or its
    /// blend's, or by one second when its clips have zero duration, so that such a state reaches its
    /// exit times. A looping clip samples the fraction of its duration that the normalized time's
    /// fractional part gives, and a clip that does not loop samples the fraction min(time, 1); a clip
    /// of zero duration therefore holds its pose. Each clip that advances reports the events that it
    /// crosses as Playback::advance does: those after its previous position and at or before its new
    /// one, once per loop crossed, and on the first advance of a nonzero amount after its state was
    /// entered, also those at its entry point. The events of a clip of zero duration lie at normalized
    /// time 0, so a looping state crosses them once per pass. A blend advances only its clips of
    /// nonzero weight. Events at equal offsets keep this order: the outgoing state's before the
    /// entered state's, a blend's clips in threshold order, and a clip's events in the order its
    /// ClipMetadata lists them.
    ///
    /// Throws `std::invalid_argument` for a negative or nonfinite step, a negative speed parameter of a
    /// state that plays, and a step longer than 10,000 passes of a looping state; `std::runtime_error`
    /// when a normalized time overflows; and `std::logic_error` once the mesh was replaced, even for a
    /// zero step. The machine is unchanged on failure.
    [[nodiscard]] std::vector<AnimationStateEvent> update(double seconds);
    /// Component hook: runs update() and keeps its events for events().
    void on_update(double seconds) { events_ = update(seconds); }
    /// Events from the most recent on_update; each call replaces them, so nothing accumulates.
    [[nodiscard]] std::span<const AnimationStateEvent> events() const noexcept { return events_; }
    /// Name of the state that the machine plays, or enters during a crossfade.
    [[nodiscard]] const std::string &state() const;
    /// Normalized time of state().
    [[nodiscard]] double time() const noexcept { return runtime_.current.time; }
    /// The crossfade in progress, or empty.
    [[nodiscard]] std::optional<AnimationCrossfade> crossfade() const;
    /// The machine this animator runs.
    [[nodiscard]] const std::shared_ptr<const AnimationStateMachine> &machine() const noexcept { return machine_; }
    /// The last successfully published pose.
    [[nodiscard]] const Pose &pose() const noexcept { return pose_; }

  private:
    friend struct detail::StateMachineAnimatorAccess;
    std::size_t parameter(std::string_view name, AnimationStateMachine::ParameterType type) const;
    void commit(detail::AnimationStateRuntime next);
    GameObject object_;
    std::shared_ptr<const Mesh> mesh_;
    std::shared_ptr<const AnimationStateMachine> machine_;
    detail::AnimationStateRuntime runtime_;
    Pose pose_;
    std::vector<AnimationStateEvent> events_;
};

/// Registers the `anima.state-machine-animator.v1` component codec for StateMachineAnimator.
///
/// The payload is a JSON object of at most 16 MiB with exactly `machine`, the key that @p name returns
/// for StateMachineAnimator::machine; `state` and `time`, StateMachineAnimator::state and
/// StateMachineAnimator::time; and `parameters`, an object with every parameter's value by name: a
/// number for a float or integer, and a boolean for a bool or for whether a trigger is set. Keys are 1
/// to 4,096 bytes without NUL, both when captured and when restored. A crossfade is not persisted: a
/// restored animator plays `state` alone at `time` and publishes that pose, and, unlike play(), does
/// not report the events at that time again. Events and the pose are not persisted.
///
/// Restoring rejects missing, unknown, repeated and mistyped fields, a key that @p resolve maps to null, a
/// parameter that the machine lacks or that the payload omits, a value of the wrong type or outside
/// its type's range, an unknown state and a negative time with `std::invalid_argument`, and a machine
/// whose source does not match the object's mesh as the constructor does. Capture throws
/// `std::invalid_argument` for an invalid key and a payload over the limit. Neither callback may
/// mutate scenes or components. Throws `std::invalid_argument` for an empty callback or when
/// @p codecs already has a StateMachineAnimator codec or that key, leaving @p codecs unchanged.
void add_state_machine_animator_codec(ComponentCodecs &codecs, AnimationStateMachineName name,
                                      AnimationStateMachineResolver resolve);
} // namespace anima
