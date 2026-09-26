#pragma once
#include <anima/core/math.hpp>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>
#include <vector>

/// @file
/// PCM clips, mixing buses, voices and the stereo `float` mixer.
///
/// Part of `anima::core`, which depends on no other library. The mixer renders into caller
/// buffers and opens no device; anima::AudioOutput plays it through SDL. Positions and distances
/// are right-handed and Y-up in caller-consistent units, with every coordinate finite and within
/// 1,000,000,000 of zero. Gains are linear factors in [0, 16]; a voice's gain, the gains of its
/// bus chain and the master gain multiply.
///
/// Use a mixer, its buses and its voices from one thread: mutations and render() run on the
/// caller's thread, and the mixer starts no thread, device or callback. Invalid arguments throw
/// `std::invalid_argument`, and calling a member of a moved-from Audio or of an empty AudioBus or
/// Sound throws `std::logic_error`, unless a member states otherwise. There is no streaming or
/// compressed decoding, no effect processing such as reverb, Doppler or HRTF, and no capture.

namespace anima {
/// Immutable interleaved PCM samples, shared by any number of voices.
///
/// A clip has 1 or 2 channels (stereo interleaves left, then right), a sample rate in
/// [8,000, 192,000] Hz and 1 to 33,554,432 finite samples in [-1, 1], counting every channel.
/// Factories validate the whole input before creating a clip; file IO belongs to the caller.
class AudioClip {
  public:
    /// Creates a clip from @p samples, which must hold whole frames of @p channels.
    static std::shared_ptr<const AudioClip> pcm(std::vector<float> samples, unsigned channels, unsigned sample_rate);
    /// Decodes a complete little-endian RIFF/WAVE file of at most 128 MiB into a clip.
    ///
    /// The RIFF size must equal the input length minus 8, odd-sized chunks need their pad byte,
    /// and exactly one `fmt ` and one `data` chunk must be present; other chunks are skipped. Only
    /// mono or stereo PCM16 (format tag 1) and IEEE float32 (format tag 3) with a consistent block
    /// alignment and byte rate are accepted. PCM16 decodes as `value / 32768`; float samples must
    /// already meet the clip limits.
    static std::shared_ptr<const AudioClip> wav(std::span<const std::byte> bytes);
    /// Interleaved samples.
    std::span<const float> samples() const { return samples_; }
    unsigned channels() const { return channels_; }
    /// Frames per second.
    unsigned sample_rate() const { return sample_rate_; }
    /// Length in seconds.
    double duration() const { return double(samples_.size() / channels_) / sample_rate_; }

  private:
    AudioClip(std::vector<float>, unsigned channels, unsigned sample_rate);
    std::vector<float> samples_;
    unsigned channels_, sample_rate_;
};
namespace detail {
struct AudioState;
struct AudioBusState;
struct VoiceState;
struct AudioSceneAccess;
} // namespace detail
/// Handle to a mixing bus of one Audio; copies share the bus.
///
/// An empty handle (default-constructed or moved-from) stands for the master output when passed
/// as a parent or target bus. A bus starts at gain 1, unmuted, and applies to every voice routed
/// to it or to its descendants. Voices and child buses retain their whole parent chain, so
/// dropping the application's handles changes nothing.
class AudioBus {
  public:
    AudioBus() = default;
    /// Sets the bus gain, in [0, 16].
    void volume(float gain);
    /// Silences the bus and its descendants without pausing their voices, whose cursors and fades
    /// keep advancing.
    void muted(bool value);

  private:
    friend class Audio;
    std::shared_ptr<detail::AudioBusState> state_;
};
/// Move-only owner of one mixer voice.
///
/// A new voice is stopped at the start of its clip, with gain 1, pitch 1, pan 0, attenuation
/// distances 1 to 100, looping and spatial mode off and its position at the origin. Destroying or
/// reassigning the Sound stops the voice and frees its capacity slot. The voice retains its clip,
/// bus chain and mixer state, so it never refers to a destroyed Audio.
class Sound {
  public:
    Sound() = default;
    Sound(Sound &&) noexcept = default;
    Sound &operator=(Sound &&) noexcept = default;
    Sound(const Sound &) = delete;
    Sound &operator=(const Sound &) = delete;
    /// Starts or resumes playback at the cursor; a voice at the end of its clip restarts from the
    /// beginning.
    void play();
    /// Stops playback, keeping the cursor and any fade in progress.
    void pause();
    /// Stops playback, rewinds to the start and cancels any fade at its current gain.
    void stop();
    /// Moves the cursor to @p seconds of clip time, in [0, AudioClip::duration()].
    void seek(double seconds);
    /// Whether the voice is playing. A non-looping voice stops by itself at the end of its clip,
    /// leaving the cursor at the clip duration.
    bool playing() const;
    /// Playback position in seconds of clip time; it advances by the pitch multiplier for each
    /// rendered second.
    double cursor() const;
    /// Loops playback, interpolating across the end-to-start boundary.
    void looping(bool value);
    /// Sets the voice gain, in [0, 16], and cancels any fade.
    void volume(float gain);
    /// Sets the playback-rate multiplier, in [0.01, 8], which scales both pitch and duration.
    void pitch(float rate);
    /// Sets the pan of a nonspatial voice, in [-1, 1] from left to right.
    ///
    /// Mono clips use equal-power gains, -3 dB on each channel at center. Stereo clips keep both
    /// channels unchanged at center and attenuate the opposite channel as they pan.
    void pan(float value);
    /// Places the voice relative to the listener (see Audio::listener) instead of using pan().
    ///
    /// A spatial voice downmixes to mono and pans with equal power by the cosine between the
    /// listener's right vector and the direction from the listener to the voice, centered when
    /// the two positions coincide. Its gain is full within the minimum attenuation() distance and
    /// falls linearly to silence at the maximum. Nonspatial voices ignore position() and
    /// attenuation().
    void spatial(bool value);
    /// Position of a spatial voice, in the same space as the listener.
    void position(Vec3 value);
    /// Sets the spatial distance range. @p minimum_distance must be at least 0 and less than
    /// @p maximum_distance, which must be at most 1,000,000,000.
    void attenuation(float minimum_distance, float maximum_distance);
    /// Changes the voice gain linearly from its current value to @p target_gain, in [0, 16], over
    /// @p seconds of rendered output, in [0, 86,400]; zero applies it at once.
    ///
    /// The fade advances only while the voice plays, independent of pitch. A new fade replaces it,
    /// and volume() and stop() cancel it.
    void fade(float target_gain, double seconds);

  private:
    friend class Audio;
    detail::VoiceState &state() const;
    std::shared_ptr<detail::VoiceState> state_;
};
/// The mixer: voice capacity, master gain, listener and rendering.
///
/// Moving an Audio transfers the mixer to the destination. Voices, buses, anima::AudioOutput,
/// anima::AudioSource and the codecs of anima::add_audio_component_codecs retain the mixer state,
/// so moving or destroying the wrapper never invalidates them.
class Audio {
  public:
    /// Creates a mixer rendering at @p sample_rate Hz, in [8,000, 192,000], with room for
    /// @p maximum_voices voices, in [1, 4,096]. The master gain starts at 1 and the listener at
    /// the origin, facing -Z with +Y up.
    explicit Audio(unsigned sample_rate = 48000, unsigned maximum_voices = 256);
    Audio(const Audio &) = delete;
    Audio &operator=(const Audio &) = delete;
    Audio(Audio &&) noexcept = default;
    Audio &operator=(Audio &&) noexcept = default;
    /// Creates a bus under @p parent, or under the master output when @p parent is empty. The
    /// parent must belong to this mixer and never changes; bus chains are at most 16 deep.
    AudioBus bus(const AudioBus &parent = {});
    /// Creates a stopped voice for @p clip on @p bus, or on the master output when @p bus is empty.
    /// Throws `std::invalid_argument` for a null clip or another mixer's bus, and
    /// `std::length_error` when the mixer already holds its maximum number of voices; no voice is
    /// stolen.
    Sound sound(std::shared_ptr<const AudioClip> clip, const AudioBus &bus = {});
    /// Whether @p sound holds a voice created by this mixer, including after either was moved.
    /// False for an empty Sound or a moved-from mixer.
    [[nodiscard]] bool owns(const Sound &sound) const noexcept;
    /// Whether this mixer can route to @p bus: the master output (an empty handle) or a bus it
    /// created. False for every bus of a moved-from mixer.
    [[nodiscard]] bool owns(const AudioBus &bus) const noexcept;
    /// Sets the master gain, in [0, 16].
    void volume(float gain);
    /// Places the listener that spatial voices pan and attenuate against.
    ///
    /// Right-handed, as for cameras: the default -Z forward with +Y up puts +X on the listener's
    /// right, and facing +Z swaps left and right. Only @p position and the right vector,
    /// `cross(forward, up)` normalized, affect the mix, so it does not distinguish front from back
    /// or above from below. Throws when @p forward or @p up is shorter than 0.000001 or the two
    /// are parallel.
    void listener(Vec3 position, Vec3 forward = view_forward, Vec3 up = world_up);
    /// Output rate in frames per second.
    unsigned sample_rate() const;
    /// Mixes the next `output.size() / 2` stereo frames into @p output, overwriting it.
    ///
    /// @p output holds interleaved left and right samples, so its size must be even. Voices and
    /// fades advance by the rendered frames, not wall-clock time. Clips are resampled linearly to
    /// the output rate, and the mix is hard-clipped to [-1, 1]. Parameter changes take effect at
    /// the start of the next call, without smoothing. With no changes in between, rendering the
    /// same frames as one block or as consecutive smaller blocks produces identical samples on the
    /// same platform; cross-platform bitwise equality is not promised.
    void render(std::span<float> output);

  private:
    friend class AudioOutput;
    friend struct detail::AudioSceneAccess;
    explicit Audio(std::shared_ptr<detail::AudioState> state) : state_(std::move(state)) {}
    detail::AudioState &state() const;
    std::shared_ptr<detail::AudioState> state_;
};
} // namespace anima
