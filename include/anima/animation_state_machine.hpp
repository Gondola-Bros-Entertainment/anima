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
#include <variant>
#include <vector>

/// @file
/// Animation state machines: states that play one clip or a one-dimensional blend of clips, and
/// transitions between them with conditions on parameters, exit times and crossfades, run on a
/// GameObject's mesh by the StateMachineAnimator component. The clips are an asset's own, or the
/// base clips of a MotionRuntime, played on its model.
///
/// Part of the `anima::assets` target. The application authors the machine, as a Definition or a
/// document, under its own names, and sets its parameters; Anima evaluates it. Times are in
/// seconds. A state's normalized time starts where the state is entered and counts passes through
/// its clips: each second it grows by the state's rate (see StateMachineAnimator::update), so 1 is
/// one full pass, its whole part counts the passes completed, and it keeps growing after a
/// state that does not loop reaches its end, whose clips then hold their last pose.

namespace anima {
class MotionRuntime;
struct AnimationParameterId;
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
// A crossfade: its transition, or none for one that StateMachineAnimator::cross_fade started, its length in
// seconds, the state it leaves, and that state while it plays or the pose frozen when the crossfade interrupted
// another.
struct AnimationStateFade {
    std::optional<std::size_t> transition;
    double duration{};
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
/// A state machine over the clips of one asset, or the base clips of one MotionRuntime: parameters,
/// states and transitions. Immutable after construction; StateMachineAnimator runs it, and animators
/// share one machine through `std::shared_ptr<const AnimationStateMachine>`. Construction and const
/// member functions may run concurrently on any thread, so animators on different threads may share
/// one machine.
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
        /// The clip the state plays, by its nonempty name, or the blend it plays instead. A
        /// default-constructed state holds an empty clip name, which the machine rejects.
        std::variant<std::string, Blend> motion;
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
    /// Validates @p definition over the base clips of @p motion and keeps both; @p motion is retained,
    /// and with it its model, which source() returns.
    ///
    /// Each clip that a state names must be a base clip of @p motion, whose MotionRuntime::clips entry
    /// gives its playback policy; layer clips, and clips that the model carries itself, are not base
    /// clips. The states sample their clips with MotionRuntime::sample, so the poses cover the model's
    /// nodes, with local transforms, and nodes that the motion lacks keep their rest transforms. The
    /// rules on @p definition are the other constructor's.
    ///
    /// Throws `std::invalid_argument` for a null @p motion and for a @p definition that the other
    /// constructor would reject, including a clip name that is not a base clip of @p motion.
    AnimationStateMachine(std::shared_ptr<const MotionRuntime> motion, Definition definition);
    /// Decodes an `anima.animation-state-machine` version 2 document and constructs the machine from it
    /// as the constructor does.
    ///
    /// The document is UTF-8 JSON of at most 4 MiB, nested at most 16 levels deep, with exactly
    /// `version`, `kind`, `parameters`, `states` and `transitions`. Repeated fields are rejected
    /// while parsing; then a `version` other than the integer 2, or another `kind`, before the
    /// other fields; then missing and unknown fields at every level, since every field is required.
    /// Each parameter has `name`, `type` (`"float"`, `"int"`, `"bool"` or `"trigger"`) and
    /// `initial`: a number for a float or integer, a boolean for a bool, and false for a trigger.
    /// Each state has `name`, `clip` and `blend`, exactly one of which is null (a blend has
    /// `parameter`, and `clips`, each with `clip` and `threshold`), `speed`, and `speed_parameter`,
    /// a parameter name or null for none. Each transition has `from` (a state name, or null for any
    /// state), `to`, `conditions` (each with `parameter`, `mode`, named as the ConditionMode
    /// enumerators are, and `threshold`), `exit_time` (a number, or null for none), `duration`,
    /// `offset`, `interruption` (named as the Interruption enumerators are) and `to_self`.
    ///
    /// May run concurrently on any thread. Reads @p document and the C locale, which must not change
    /// during the call, as prefab.hpp describes for documents. Throws `std::invalid_argument` for
    /// invalid content, including malformed JSON and values of the wrong JSON type.
    [[nodiscard]] static AnimationStateMachine
    deserialize(std::shared_ptr<const Asset> source, std::span<const ClipMetadata> clips, std::string_view document);
    /// Decodes @p document as the other overload does and constructs the machine over the base clips
    /// of @p motion. Throws as that overload and the motion constructor do.
    [[nodiscard]] static AnimationStateMachine deserialize(std::shared_ptr<const MotionRuntime> motion,
                                                           std::string_view document);
    /// The definition, as validated.
    [[nodiscard]] const Definition &definition() const noexcept { return definition_; }
    /// The asset whose nodes the states pose: the asset whose clips they play, or the model of
    /// motion().
    [[nodiscard]] const std::shared_ptr<const Asset> &source() const noexcept { return source_; }
    /// The motion runtime whose base clips the states play, or null for a machine over the clips of
    /// source().
    [[nodiscard]] const std::shared_ptr<const MotionRuntime> &motion() const noexcept { return motion_; }
    /// Resolves parameter @p name to its id, which the StateMachineAnimator parameter accessors take
    /// to skip the lookup by name. The id stays valid for the machine's lifetime. Throws
    /// `std::out_of_range` with "Unknown animation parameter: " and @p name for an unknown parameter.
    [[nodiscard]] AnimationParameterId parameter(std::string_view name) const;

  private:
    friend class StateMachineAnimator;
    friend struct detail::StateMachineAnimatorAccess;
    // Validates definition_ over the clips of motion_, or of source_ when motion_ is null, and builds data_.
    void compile(std::span<const ClipMetadata> clips);
    std::shared_ptr<const Asset> source_;
    std::shared_ptr<const MotionRuntime> motion_;
    Definition definition_;
    std::shared_ptr<const detail::AnimationStateMachineData> data_;
};

/// A parameter of an AnimationStateMachine, resolved by AnimationStateMachine::parameter.
///
/// The StateMachineAnimator accessors check only that #index lies within their machine's parameters and
/// that the parameter there has #type, so an id resolved from another machine is accepted when the
/// parameter at its index has its type.
struct AnimationParameterId {
    /// Index of the parameter in AnimationStateMachine::Definition::parameters.
    std::size_t index{};
    /// Type of the parameter.
    AnimationStateMachine::ParameterType type = AnimationStateMachine::ParameterType::real;
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

/// A crossfade in progress; see StateMachineAnimator::update and StateMachineAnimator::cross_fade.
struct AnimationCrossfade {
    /// Index of its transition in AnimationStateMachine::Definition::transitions, or empty for a
    /// crossfade that StateMachineAnimator::cross_fade started.
    std::optional<std::size_t> transition;
    /// Name of the state it leaves: the transition's source, or for a transition from any state or a
    /// crossfade from cross_fade, the state the machine played or was entering when it started.
    std::string source;
    /// Normalized time of #source while the crossfade fades it out as it keeps playing; empty when the
    /// crossfade interrupted another and fades out the pose frozen when it started.
    std::optional<double> source_time;
    /// Its length in seconds, greater than 0: its transition's Transition::duration, or the seconds
    /// passed to cross_fade.
    double duration{};
    /// Seconds since it started, less than #duration.
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
/// Poses are sampled with sample_pose, or MotionRuntime::sample for a machine over a motion runtime,
/// and blended with blend_pose, so pose() has local transforms. A pose filter (set_pose_filter) can
/// then change the pose that is published, for example with MotionRuntime::evaluate to place contacts.
/// Not synchronized: use it only on the thread that uses its object's scene, as scene.hpp requires
/// of the scene's handles.
class StateMachineAnimator {
  public:
    /// Binds @p machine to the mesh of @p owner's object, with its parameters at their initial values,
    /// enters its first state at normalized time 0 and publishes that pose. Throws
    /// `std::invalid_argument` for a null machine or one whose source asset does not match the mesh, as
    /// Animator requires; fails as GameObject::renderer does when the object has no mesh. A machine
    /// over a motion runtime matches the meshes that its model matches.
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
    /// The overloads that take an AnimationParameterId act as those that take the parameter's name, without
    /// looking it up. Each throws `std::invalid_argument` with "Animation parameter id does not match the
    /// machine" for an @p id whose index lies outside the machine's parameters or whose type differs from
    /// the parameter's there; then, as its name form does, with "Animation parameter has another type: "
    /// and the parameter's name for a parameter of another type, and set_float() with "Animation float
    /// parameter must be finite: " and the name for a nonfinite value.
    void set_float(AnimationParameterId id, float value);
    /// Sets integer parameter @p id; see set_float(AnimationParameterId, float).
    void set_integer(AnimationParameterId id, std::int32_t value);
    /// Sets bool parameter @p id; see set_float(AnimationParameterId, float).
    void set_bool(AnimationParameterId id, bool value);
    /// Sets trigger @p id, as set_trigger(std::string_view) does; see set_float(AnimationParameterId, float).
    void set_trigger(AnimationParameterId id);
    /// Unsets trigger @p id; see set_float(AnimationParameterId, float).
    void reset_trigger(AnimationParameterId id);
    /// Value of float parameter @p id; see set_float(AnimationParameterId, float).
    [[nodiscard]] float get_float(AnimationParameterId id) const;
    /// Value of integer parameter @p id; see set_float(AnimationParameterId, float).
    [[nodiscard]] std::int32_t get_integer(AnimationParameterId id) const;
    /// Value of bool parameter @p id, or whether trigger @p id is set; see
    /// set_float(AnimationParameterId, float).
    [[nodiscard]] bool get_bool(AnimationParameterId id) const;
    /// Enters state @p name at normalized time @p normalized_time, ending any crossfade, and publishes
    /// its pose. As on entry through a transition, the next update that advances the state reports
    /// the events at its entry point. Parameters are unchanged. Throws `std::out_of_range` for an
    /// unknown state and `std::invalid_argument` for a negative or nonfinite time; the machine is
    /// unchanged on failure.
    void play(std::string_view name, double normalized_time = 0);
    /// Crossfades for @p seconds to state @p name, entered at normalized time @p normalized_offset,
    /// and publishes the pose at the crossfade's start; with @p seconds 0 it enters the state at once,
    /// as play() does.
    ///
    /// The crossfade runs as a transition's does (see update()), with @p seconds as its duration. The
    /// entered state reports the events at its entry point on its first advance. The outgoing pose is
    /// the state the machine plays, which keeps playing and reporting events until the crossfade ends,
    /// or, when it interrupts a crossfade in progress, pose(), frozen. @p name may be the state that
    /// the machine plays, which then fades into a new pass of itself. No transition interrupts the
    /// crossfade, so transitions are checked again by the first update after it ends; play() and
    /// cross_fade() can end it at any time. Parameters, including triggers, are unchanged.
    ///
    /// Throws `std::out_of_range` with "Unknown animation state: " and @p name for an unknown state;
    /// then `std::invalid_argument` with "Invalid animation crossfade duration" for a negative or
    /// nonfinite @p seconds, and with "Invalid animation crossfade offset" for a @p normalized_offset
    /// outside [0, 1), the range of Transition::offset. The machine is unchanged on failure.
    void cross_fade(std::string_view name, double seconds, double normalized_offset = 0);
    /// Starts at most one transition, advances by @p seconds, publishes the pose and returns the clip
    /// events crossed, ordered by AnimationStateEvent::offset.
    ///
    /// First, transitions are checked in priority order, and the first whose conditions all hold, and
    /// that has no exit time or whose source state crossed its exit time, starts. Outside a crossfade
    /// the candidates are the transitions from any state, then the transitions from the current state.
    /// During a crossfade they are those its transition's Interruption admits, except that transition
    /// itself: with `source`, the transitions from any state that precede it and then those from its
    /// source state that precede it; with `destination`, the transitions from any state and then those
    /// from the state it enters; with the two combined, both lists in the stated order, each transition
    /// once. A crossfade that cross_fade() started admits none. A transition from any state is skipped
    /// while its target is the state the machine plays or is entering, unless Transition::to_self is
    /// set. A state crosses exit time `e` when its normalized time passes `e`, having been below it,
    /// during the advances since the state was entered or transitions were last checked; a looping
    /// state also crosses `e + k` for each whole `k` when `e` is below 1. A transition with an exit
    /// time can start only while its source state plays, not after a crossfade froze it.
    ///
    /// A starting transition resets the triggers its conditions test and enters its target at
    /// Transition::offset. With a zero duration the machine then plays the target alone. Otherwise the
    /// pose crossfades for Transition::duration seconds from the outgoing pose to the target's,
    /// blended with blend_pose by `elapsed / duration`, where elapsed counts the seconds advanced
    /// since the start. The outgoing pose is the state the machine played, which keeps playing and
    /// reporting events until the crossfade ends, or, when the transition interrupts a crossfade,
    /// pose(), the pose last evaluated, frozen, so the pose stays continuous. A pose filter's result
    /// is never frozen.
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
    /// Read them in on_late_update or after Scene::update returns: during on_update this animator
    /// may not have run yet, since Scene::update leaves the order within a phase unspecified.
    [[nodiscard]] std::span<const AnimationStateEvent> events() const noexcept { return events_; }
    /// Name of the state that the machine plays, or enters during a crossfade.
    [[nodiscard]] const std::string &state() const;
    /// Normalized time of state().
    [[nodiscard]] double time() const noexcept { return runtime_.current.time; }
    /// The crossfade in progress, or empty.
    [[nodiscard]] std::optional<AnimationCrossfade> crossfade() const;
    /// The machine this animator runs.
    [[nodiscard]] const std::shared_ptr<const AnimationStateMachine> &machine() const noexcept { return machine_; }
    /// The pose that the machine evaluated for the last successful publish, before any pose filter,
    /// with local transforms.
    [[nodiscard]] const Pose &pose() const noexcept { return pose_; }
    /// The last successfully published pose: pose() as the pose filter left it, or pose() itself when
    /// that publish had no filter.
    [[nodiscard]] const Pose &published_pose() const noexcept { return filtered_ ? *filtered_ : pose_; }
    /// Sets @p filter, which every later publish (by play(), cross_fade() or update()) calls on a copy
    /// of pose() before it passes that copy to MeshRenderer::set_pose; an empty @p filter removes it.
    /// The pose already published is unchanged.
    ///
    /// The filter may replace the pose with any that set_pose accepts, including a world-only one,
    /// such as MotionRuntime::evaluate returns when it applies joint offsets and two-bone contacts to
    /// the machine's pose; that is its intended use. Crossfades blend and freeze pose(), never the
    /// filtered pose. The animator owns @p filter and calls it within the publishing call, on that
    /// call's thread; the filter must not call this animator's play(), cross_fade(), update() or
    /// set_pose_filter(). An exception from the filter, or from set_pose for the pose that it leaves,
    /// propagates from the publishing call, which then leaves the animator and its published pose
    /// unchanged.
    void set_pose_filter(std::function<void(Pose &)> filter) { filter_ = std::move(filter); }

  private:
    friend struct detail::StateMachineAnimatorAccess;
    // Index of the parameter that @p id names, checked against the machine and against the accessor's type.
    std::size_t parameter(AnimationParameterId id, AnimationStateMachine::ParameterType type) const;
    void commit(detail::AnimationStateRuntime next);
    GameObject object_;
    std::shared_ptr<const Mesh> mesh_;
    std::shared_ptr<const AnimationStateMachine> machine_;
    detail::AnimationStateRuntime runtime_;
    Pose pose_;
    // The published pose when the last publish ran a filter.
    std::optional<Pose> filtered_;
    std::function<void(Pose &)> filter_;
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
/// not report the events at that time again. Events, the pose and the pose filter are not persisted;
/// a restored animator has no filter.
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
