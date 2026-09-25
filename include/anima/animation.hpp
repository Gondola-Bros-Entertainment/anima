#pragma once
#include <anima/scene.hpp>

namespace anima {
struct ClipEvent {
    double time{};
    std::string name;
};
struct ClipMetadata {
    std::string name;
    bool loop{};
    std::vector<ClipEvent> events;
    // Optional authoring speed in asset units/second for an in-place travel clip.
    // Consumers can match cycle playback to actual travel without editing keys.
    std::optional<double> reference_speed = {};
};
class Playback {
  public:
    void select(const Animation &animation, const ClipMetadata &metadata, bool play = true);
    void restart();
    void toggle();
    void pause() noexcept { playing_ = false; }
    void resume();
    void seek(double time); // Scrubbing is silent; emits no events.
    [[nodiscard]] std::vector<ClipEvent> advance(double elapsed);
    [[nodiscard]] double time() const noexcept;
    [[nodiscard]] bool playing() const noexcept { return playing_; }
    [[nodiscard]] bool finished() const noexcept { return finished_; }
    [[nodiscard]] const Animation *animation() const noexcept { return animation_; }

  private:
    const Animation *animation_{};
    ClipMetadata metadata_;
    double elapsed_{};
    bool playing_{}, finished_{}, fresh_{};
};
// Explicit per-object playback. The source is retained so clip references cannot
// dangle. Advance with caller time; there is no hidden global update loop.
class Animator {
  public:
    Animator(GameObject object, std::shared_ptr<const Asset> source);
    void play(std::string_view clip, bool loop = true);
    void select(const ClipMetadata &clip, bool play = true);
    void bind_pose();
    void pause();
    void resume();
    void restart();
    void seek(double seconds);
    [[nodiscard]] std::vector<ClipEvent> update(double seconds);
    void on_update(double seconds) { events_ = update(seconds); }
    // Events from the most recent scene-driven update; no unbounded event queue.
    [[nodiscard]] std::span<const ClipEvent> events() const noexcept { return events_; }
    [[nodiscard]] const Playback &playback() const noexcept { return playback_; }
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
