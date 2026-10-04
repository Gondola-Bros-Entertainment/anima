#pragma once
#include <anima/scene.hpp>

/// @file
/// Clip playback: a Playback clock and the Animator component that poses a GameObject's mesh.
///
/// Part of the `anima::assets` target. Times are in seconds, supplied by the caller; there is no
/// hidden global update loop.

namespace anima {
/// A named marker in a clip.
struct ClipEvent {
    /// Seconds from the clip start, in [0, Animation::duration].
    double time{};
    std::string name;
};
/// Playback policy for one clip.
struct ClipMetadata {
    /// Name of the Animation it describes.
    std::string name;
    /// Whether playback wraps at the end instead of stopping.
    bool loop{};
    /// Events that Playback::advance reports when crossed.
    std::vector<ClipEvent> events;
    /// Optional authored travel speed of an in-place travel clip, in asset units per second;
    /// positive and finite when set. Anima validates but does not apply it: to match actual
    /// travel without editing keys, scale playback by `actual_speed / reference_speed`, with
    /// Animator::set_speed or, in a state machine, AnimationStateMachine::State::speed_parameter.
    std::optional<double> reference_speed = {};
};
/// Whether a newly selected clip plays or waits at its start.
enum class PlaybackStart {
    playing, ///< Advances from the next step.
    paused   ///< Holds time 0 until resumed or restarted.
};
/// Playback clock for one clip: time, looping, pausing and event crossing, without posing.
///
/// Borrows the selected Animation, which must outlive its use here. Not synchronized: use each
/// Playback from one thread at a time.
class Playback {
  public:
    /// Selects @p animation with @p metadata and rewinds to 0, playing or paused as @p start says.
    ///
    /// A clip of zero duration, as load_asset imports one whose keys all sit at time 0, plays as a
    /// pose: its time stays 0, and advance() describes its events. Throws `std::invalid_argument`
    /// when @p metadata names another clip, the duration is negative or not finite, or an event
    /// lies outside [0, duration].
    void select(const Animation &animation, const ClipMetadata &metadata, PlaybackStart start = PlaybackStart::playing);
    /// Rewinds to 0 and plays; events at 0 are reported again. Does nothing before select().
    void restart();
    /// Pauses when playing, otherwise resumes.
    void toggle();
    void pause() noexcept { playing_ = false; }
    /// Plays again, restarting a finished clip. Does nothing before select().
    void resume();
    /// Moves to @p time seconds without reporting events: looping clips wrap, others clamp and
    /// finish at the end. A clip of zero duration stays at 0, its end, so one that does not loop
    /// finishes. Throws `std::invalid_argument` before select() or for a negative or nonfinite time.
    void seek(double time);
    /// Advances by @p elapsed seconds while playing and returns the events crossed, in time order.
    ///
    /// An event is crossed when its time lies after the previous position and at or before the
    /// new one, once per loop crossed; events at 0 are reported by the first nonzero advance while
    /// playing after select() or restart(). A non-looping clip stops and finishes at its end. A
    /// clip of zero duration does not wrap: its events, all at 0, are reported once, by that first
    /// advance, which also finishes it unless it loops; a looping one keeps playing at 0 and
    /// reports nothing more. Paused playback, no clip or a zero step reports nothing. Throws
    /// `std::invalid_argument` for a negative or nonfinite step, and for a step longer than 10,000
    /// clip durations on a looping clip of positive duration.
    [[nodiscard]] std::vector<ClipEvent> advance(double elapsed);
    /// Clip time in seconds; 0 before select().
    [[nodiscard]] double time() const noexcept;
    [[nodiscard]] bool playing() const noexcept { return playing_; }
    /// Whether a non-looping clip reached its end.
    [[nodiscard]] bool finished() const noexcept { return finished_; }
    /// The selected clip, or null.
    [[nodiscard]] const Animation *animation() const noexcept { return animation_; }

  private:
    const Animation *animation_{};
    ClipMetadata metadata_;
    double elapsed_{};
    bool playing_{}, finished_{}, fresh_{};
};
/// Component that plays clips of a shared Asset on its GameObject's mesh.
///
/// Retains the source asset, so clip references cannot dangle, and publishes each pose through
/// MeshRenderer::set_pose. Attached as a component, on_update advances it during Scene::update; a
/// standalone Animator advances through update(). Use one driver per Animator. Replacing the
/// object's mesh requires a new Animator: every call that publishes a pose then throws
/// `std::logic_error`. Not synchronized: use it only on the thread that uses its object's scene,
/// as scene.hpp requires of the scene's handles.
class Animator {
  public:
    /// Binds @p source to @p object's mesh and publishes the rest pose. Throws
    /// `std::invalid_argument` for a null source or one whose node names, parents or rest pose
    /// (within `1e-5`) differ from the mesh's; fails as GameObject::renderer does when @p object
    /// has no mesh. The Animator holds @p source, and with it the images of its textures, until it
    /// is destroyed; TexelRetention describes how to let them go.
    Animator(GameObject object, std::shared_ptr<const Asset> source);
    /// Selects @p clip with no events and plays it; see select().
    void play(std::string_view clip, bool loop = true);
    /// Selects the source clip that @p clip names, publishes its first pose and plays or pauses as
    /// @p start says, leaving the bind pose. Throws `std::out_of_range` when no source clip has
    /// that name, `std::invalid_argument` when several do, and as Playback::select does; the state
    /// is unchanged on failure.
    void select(const ClipMetadata &clip, PlaybackStart start = PlaybackStart::playing);
    /// Publishes the rest pose and pauses; update() then does nothing until a clip is selected,
    /// resumed, restarted or sought.
    void bind_pose();
    void pause();
    /// Resumes playback, restarting a finished clip, and publishes the pose.
    void resume();
    /// Restarts the clip from 0 and publishes the pose.
    void restart();
    /// Seeks without events and publishes the pose; see Playback::seek.
    void seek(double seconds);
    /// Sets the playback rate, a multiplier of each update() step: 1 (the default) plays in real
    /// time, 2 twice as fast, and 0 holds the pose. It scales update() only, not seek(), and
    /// select(), play() and the other controls keep it.
    /// Throws `std::invalid_argument` with "Animator speed must be finite and nonnegative" unless
    /// @p multiplier is finite and at least 0, leaving the rate unchanged.
    void set_speed(double multiplier);
    /// The playback rate that set_speed() last set, 1 by default.
    [[nodiscard]] double speed() const noexcept { return speed_; }
    /// Advances playback by @p seconds times speed(), publishes the new pose and returns the
    /// events crossed.
    ///
    /// Returns nothing in the bind pose or while paused. Throws `std::invalid_argument` with
    /// "Invalid Animator time step" for a negative or nonfinite @p seconds, with "Animator time step
    /// times its speed is not finite" when the scaled step overflows, and as Playback::advance
    /// does; throws `std::logic_error` once the mesh was replaced, even while paused.
    [[nodiscard]] std::vector<ClipEvent> update(double seconds);
    /// Component hook: runs update() and keeps its events for events().
    void on_update(double seconds) { events_ = update(seconds); }
    /// Events from the most recent on_update; each call replaces them, so nothing accumulates.
    /// Read them in on_late_update or after Scene::update returns: during on_update this animator
    /// may not have run yet, since Scene::update leaves the order within a phase unspecified.
    [[nodiscard]] std::span<const ClipEvent> events() const noexcept { return events_; }
    [[nodiscard]] const Playback &playback() const noexcept { return playback_; }
    /// The last successfully published pose.
    [[nodiscard]] const Pose &pose() const noexcept { return pose_; }

  private:
    void publish(const Playback &playback, bool bind = false);
    GameObject object_;
    std::shared_ptr<const Mesh> mesh_;
    std::shared_ptr<const Asset> source_;
    Playback playback_;
    Pose pose_;
    std::vector<ClipEvent> events_;
    double speed_ = 1;
    bool bind_ = true;
};
} // namespace anima
