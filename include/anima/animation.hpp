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
    /// travel without editing keys, scale playback by `actual_speed / reference_speed`.
    std::optional<double> reference_speed = {};
};
/// Playback clock for one clip: time, looping, pausing and event crossing, without posing.
///
/// Borrows the selected Animation, which must outlive its use here.
class Playback {
  public:
    /// Selects @p animation with @p metadata and rewinds to 0, playing when @p play is true.
    /// Throws `std::invalid_argument` when the names differ, the duration is not positive and
    /// finite, or an event lies outside [0, duration].
    void select(const Animation &animation, const ClipMetadata &metadata, bool play = true);
    /// Rewinds to 0 and plays; events at 0 are reported again. Does nothing before select().
    void restart();
    /// Pauses when playing, otherwise resumes.
    void toggle();
    void pause() noexcept { playing_ = false; }
    /// Plays again, restarting a finished clip. Does nothing before select().
    void resume();
    /// Moves to @p time seconds without reporting events: looping clips wrap, others clamp and
    /// finish at the end. Throws `std::invalid_argument` before select() or for a negative or
    /// nonfinite time.
    void seek(double time);
    /// Advances by @p elapsed seconds while playing and returns the events crossed, in time order.
    ///
    /// An event is crossed when its time lies after the previous position and at or before the
    /// new one, once per loop crossed; events at 0 are reported by the first nonzero advance while
    /// playing after select() or restart(). A non-looping clip stops and finishes at its end.
    /// Paused playback, no clip or a zero step reports nothing. Throws `std::invalid_argument` for
    /// a negative or nonfinite step and `std::runtime_error` for a step longer than 10,000 clip
    /// durations.
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
/// `std::logic_error`.
class Animator {
  public:
    /// Binds @p source to @p object's mesh and publishes the rest pose. Throws
    /// `std::invalid_argument` for a null source or one whose node names, parents or rest pose
    /// (within `1e-5`) differ from the mesh's; fails as GameObject::renderer does when @p object
    /// has no mesh.
    Animator(GameObject object, std::shared_ptr<const Asset> source);
    /// Selects @p clip with no events and plays it; see select().
    void play(std::string_view clip, bool loop = true);
    /// Selects the source clip that @p clip names, publishes its first pose and plays when @p play
    /// is true, leaving the bind pose. Throws `std::runtime_error` for a missing or ambiguous clip
    /// name, and as Playback::select does; the state is unchanged on failure.
    void select(const ClipMetadata &clip, bool play = true);
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
    /// Advances playback by @p seconds, publishes the new pose and returns the events crossed.
    ///
    /// Returns nothing in the bind pose or while paused. Throws `std::invalid_argument` for a
    /// negative or nonfinite step, and `std::logic_error` once the mesh was replaced, even while
    /// paused.
    [[nodiscard]] std::vector<ClipEvent> update(double seconds);
    /// Component hook: runs update() and keeps its events for events().
    void on_update(double seconds) { events_ = update(seconds); }
    /// Events from the most recent on_update; each call replaces them, so nothing accumulates.
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
    bool bind_ = true;
};
} // namespace anima
