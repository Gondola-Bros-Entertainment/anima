#pragma once
#include <anima/audio.hpp>

/// @file
/// Device playback for an anima::Audio mixer through a private SDL 3 audio stream.
///
/// Part of the optional `anima::audio_output` target (`ANIMA_BUILD_AUDIO_OUTPUT=ON`), which needs
/// no window, graphics or assets; no SDL type appears in the public API.

namespace anima {
/// Queues a mixer's output on the default playback device.
///
/// The output retains the mixer state, so moving or destroying the Audio wrapper does not
/// invalidate it. It holds its own reference to the SDL audio subsystem and never initializes
/// video, creates a window, pumps events or shuts down other SDL users; do not call `SDL_Quit`
/// while an output exists. Mixing happens only in pump(), on the caller's thread: no callback runs
/// mixer or application code. Call pump() on the thread that mutates the mixer, often enough to
/// avoid underflow, and drive each mixer from one output or render loop, since every pump renders
/// it. Voice changes affect only newly mixed audio: queued audio adds latency, and cursors
/// describe rendered audio, not the hardware playhead.
///
/// Members of a moved-from output throw `std::logic_error`; SDL failures throw
/// `std::runtime_error`.
class AudioOutput {
  public:
    /// Opens a stereo `float` stream at the mixer's sample rate on the default playback device and
    /// starts the device; SDL converts to the device format. @p buffered_seconds, in
    /// [0.005, 0.25], is the queue target. Throws `std::invalid_argument` for another target and
    /// `std::logic_error` for a moved-from mixer. A failed construction releases what it acquired.
    explicit AudioOutput(Audio &audio, double buffered_seconds = .05);
    /// Closes the stream and releases this output's SDL audio subsystem reference.
    ~AudioOutput();
    AudioOutput(AudioOutput &&) noexcept;
    AudioOutput &operator=(AudioOutput &&) noexcept;
    AudioOutput(const AudioOutput &) = delete;
    AudioOutput &operator=(const AudioOutput &) = delete;
    /// Renders and queues the frames missing from the queue target at entry; a full queue leaves
    /// the mixer untouched. When SDL rejects a block, the mixer has already advanced past it.
    void pump();
    /// Pauses or resumes the device; queued audio stays queued.
    void paused(bool value);
    /// Discards queued audio without rewinding any voice.
    void clear();
    /// Seconds of mixed audio queued in the stream.
    double queued_seconds() const;

  private:
    struct State;
    State &state() const;
    std::unique_ptr<State> state_;
};
} // namespace anima
