#pragma once
#include <anima/audio.hpp>

namespace anima {
// Optional anima::audio_output: queued SDL device output without graphics/assets.
// Retains the mixer's context. Call pump regularly on the same thread as mixer
// mutations. No callback invokes application code. Owns one SDL audio subsystem
// reference; it neither initializes video nor shuts down the caller's SDL owners.
class AudioOutput {
  public:
    explicit AudioOutput(Audio &audio, double buffered_seconds = .05);
    ~AudioOutput();
    AudioOutput(AudioOutput &&) noexcept;
    AudioOutput &operator=(AudioOutput &&) noexcept;
    AudioOutput(const AudioOutput &) = delete;
    AudioOutput &operator=(const AudioOutput &) = delete;
    void pump();
    void paused(bool value);
    void clear();
    double queued_seconds() const;

  private:
    struct State;
    State &state() const;
    std::unique_ptr<State> state_;
};
} // namespace anima
