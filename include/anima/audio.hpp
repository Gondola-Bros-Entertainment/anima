#pragma once
#include <anima/core/math.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

/// @file
/// Audio clips, mixing buses, voices and the Audio engine that mixes them.
///
/// Part of `anima::core`. Decoding, mixing and device output run on a private copy of miniaudio 0.11.25 that
/// `anima::core` compiles itself; no miniaudio type appears in this header. Positions and distances are
/// right-handed and Y-up in caller-consistent units, with every coordinate finite and within 1,000,000,000 of
/// zero. Gains are linear factors in [0, 16]; a voice's gain, the gains of its bus chain and the master gain
/// multiply, and the mix is limited to [-1, 1], with non-finite values replaced by 0.
///
/// Use an Audio, its buses and its voices from one thread at a time. An engine made by Audio::open_device also
/// mixes on miniaudio's device thread: every member of Audio, AudioBus and Sound locks the engine while it runs,
/// and the device thread holds the same lock while it mixes each block of at most anima::audio_block_frames frames.
/// Each call is therefore safe while the device plays, may wait for the block being mixed, and takes effect from the
/// next block; getters of playback, such as Sound::playing and Sound::cursor, report the state after the latest mixed
/// block, and getters of settings the value last set. An engine without a device mixes only in Audio::render, on the
/// calling thread.
///
/// Invalid arguments throw `std::invalid_argument`, and calling a member of a moved-from Audio or of an empty
/// AudioBus or Sound throws `std::logic_error`, unless a member states otherwise. A failure that miniaudio reports
/// throws `std::runtime_error`, or `std::bad_alloc` when it runs out of memory. There is no effect processing such
/// as reverb, Doppler or HRTF, and no capture.

namespace anima {
/// Seconds over which a voice's gain, pan and pitch, a bus gain and the master gain move linearly to a new value.
///
/// A change ramps only when a playing voice's audio passed through the voice, bus or master in the latest mixed
/// block, so that a listener hears it; otherwise, and for a stopped or paused voice, it applies at once. A voice's
/// ramps advance only while it plays, and bus and master ramps with output time.
inline constexpr double audio_smoothing_seconds = .02;
/// Most frames mixed at once. Mixing proceeds in blocks that never cross a multiple of this many output frames,
/// counted from the engine's creation; the end of a render() call or a device period can end one early. Pitch ramps
/// step at the start of each block that begins at such a multiple.
inline constexpr unsigned audio_block_frames = 64;

/// How AudioClip::decode holds a clip's audio.
enum class AudioLoadMode {
    /// Decodes every sample when the clip is created, as Unity's Decompress On Load does; playing decodes
    /// nothing.
    decompress,
    /// Keeps a copy of the encoded bytes, which each voice decodes while it plays, as Unity's Compressed In Memory
    /// does. The clip never holds its decoded samples.
    stream,
};

/// How a spatial voice's gain falls with its distance d from the listener, between the minimum and maximum
/// distances of its AudioAttenuation. Nearer than the minimum distance, a voice plays at full gain.
enum class AudioRolloff {
    /// `1 - (d - minimum) / (maximum - minimum)`, silent from the maximum distance on, like Unity's Linear
    /// Rolloff and Unreal's linear attenuation.
    linear,
    /// `minimum / d`, held at `minimum / maximum` beyond the maximum distance: OpenAL's inverse distance clamped
    /// model with a rolloff factor of 1. Requires a positive minimum distance.
    inverse,
};

/// A spatial voice's distance range and how its gain falls within it, as Sound::set_attenuation applies it.
/// Distances are in the units of positions. The defaults are a new voice's.
struct AudioAttenuation {
    /// Distance up to which a spatial voice plays at full gain: at least 0, and positive for AudioRolloff::inverse.
    float minimum_distance = 1;
    /// Distance from which the gain stops falling, silent with linear rolloff: greater than #minimum_distance and at
    /// most 1,000,000,000.
    float maximum_distance = 100;
    /// How the gain falls between the two distances.
    AudioRolloff rolloff = AudioRolloff::linear;
    /// Compares every field, the distances with `float` `==`.
    bool operator==(const AudioAttenuation &) const = default;
};

/// Priority of a new voice, in the [0, 255] range of Sound::set_priority.
inline constexpr int default_audio_priority = 128;

/// Where Audio::open_device sends its output.
enum class AudioBackend {
    /// The default playback device of the first miniaudio backend that opens, in miniaudio's order: WASAPI,
    /// DirectSound, then WinMM on Windows, Core Audio on macOS, and PulseAudio, ALSA, then JACK on Linux. The null
    /// backend never stands in for a missing device.
    platform,
    /// miniaudio's null backend: a device thread that consumes output at the device rate and discards it, for
    /// tests and headless runs.
    null,
};

namespace detail {
struct AudioState;
struct AudioBusState;
struct VoiceState;
struct AudioClipAccess;
struct AudioSceneAccess;
} // namespace detail

/// Immutable audio, shared by any number of voices of any number of engines.
///
/// A clip has 1 or 2 channels (stereo interleaves left, then right), a sample rate in [8,000, 192,000] Hz and at
/// least one frame; a decompressed clip holds at most 33,554,432 samples, counting every channel. Factories
/// validate their input before creating a clip; file IO belongs to the caller.
///
/// pcm() and decode() read only their arguments and may run concurrently on any thread, such as an application's
/// loader threads. A clip never changes after its factory returns, so any number of threads and engines, including
/// their device threads, may share it.
class AudioClip {
  public:
    /// Creates a decompressed clip from interleaved @p samples, which must hold whole frames of @p channels and be
    /// finite and in [-1, 1]. Throws `std::invalid_argument` with "Audio sample rate must be between 8000 and 192000
    /// Hz", "Invalid PCM channels or sample count" or "PCM samples must be finite and normalized to [-1, 1]".
    [[nodiscard]] static std::shared_ptr<const AudioClip> pcm(std::vector<float> samples, unsigned channels,
                                                              unsigned sample_rate);
    /// Creates a clip from a complete WAV, FLAC, MP3 or Ogg Vorbis file of at most 128 MiB.
    ///
    /// The leading bytes choose the decoder: `RIFF`, `RIFX`, `RF64` or `riff` (Wave64) for WAV, `fLaC` for FLAC
    /// and `OggS` for Ogg Vorbis; anything else is decoded as MP3. miniaudio decodes WAV, FLAC and MP3, and
    /// stb_vorbis decodes Vorbis, to 32-bit float at the file's own channel count and rate. Throws
    /// `std::invalid_argument` with "Invalid audio load mode", "Encoded audio exceeds 128 MiB", "Unsupported or
    /// malformed audio data" when the decoder cannot open the input, "Audio clips must have 1 or 2 channels", the
    /// sample rate message of pcm(), and "Audio data holds no samples".
    ///
    /// AudioLoadMode::decompress decodes the whole input now, and also throws "Decoded audio exceeds 33554432
    /// samples" and "Decoded audio samples must be finite". Decoded samples may exceed 1 in magnitude, as lossy
    /// decoders produce. AudioLoadMode::stream opens the input only to read its format and length, then copies it.
    /// Its length is the one the decoder reports from the file's header, or for MP3 from its Xing or Info tag or else
    /// its frame headers; an input without one is decoded once to count its frames. A voice that meets malformed
    /// data later ends there, and non-finite samples play as silence.
    ///
    /// Decoding, of the whole input or to count its frames, throws "Malformed audio data" when the decoder reports
    /// an error before the end. The decoders report none for truncated data but end at the cut, so a truncated input
    /// decodes, or counts, to the frames before it, and throws "Audio data holds no samples" when there are none.
    [[nodiscard]] static std::shared_ptr<const AudioClip> decode(std::span<const std::byte> encoded,
                                                                 AudioLoadMode mode = AudioLoadMode::decompress);
    /// Channels per frame: 1 or 2.
    [[nodiscard]] unsigned channels() const noexcept { return channels_; }
    /// Frames per second.
    [[nodiscard]] unsigned sample_rate() const noexcept { return sample_rate_; }
    /// Length in frames.
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
    /// Length in seconds.
    [[nodiscard]] double duration() const noexcept { return double(frames_) / sample_rate_; }
    /// How the clip holds its audio; clips from pcm() are decompressed.
    [[nodiscard]] AudioLoadMode load_mode() const noexcept { return mode_; }
    /// Bytes of audio the clip holds: 4 per decoded sample, or the size of a streamed clip's encoded copy.
    [[nodiscard]] std::size_t memory_bytes() const noexcept {
        return samples_.size() * sizeof(float) + encoded_.size();
    }

  private:
    friend struct detail::AudioClipAccess;
    AudioClip() = default;
    std::vector<float> samples_;
    std::vector<std::byte> encoded_;
    std::uint64_t frames_{};
    unsigned channels_{}, sample_rate_{};
    int encoding_{}; // The decoder a streamed clip's voices open.
    AudioLoadMode mode_ = AudioLoadMode::decompress;
};

/// Handle to a mixing group of one Audio that mixes into its parent, as Unity's mixer groups and Unreal's sound
/// classes do; copies share the bus.
///
/// An empty handle (default-constructed or moved-from) stands for the master output when passed as a parent or
/// target bus. A bus starts at gain 1, unmuted, and applies to every voice routed to it or to its descendants.
/// Voices, one-shots and child buses retain their whole parent chain, so dropping the application's handles changes
/// nothing.
class AudioBus {
  public:
    /// Empty handle, which stands for the master output.
    AudioBus() = default;
    /// Shares the bus of @p other, or is empty when @p other is.
    AudioBus(const AudioBus &other) = default;
    /// Takes the bus of @p other, which becomes empty.
    AudioBus(AudioBus &&other) noexcept = default;
    /// Makes this handle share @p other's bus, or empty when @p other is.
    AudioBus &operator=(AudioBus other) noexcept {
        // `other` takes the previous bus and engine, and releases the bus first, in the order of the members.
        audio_.swap(other.audio_);
        state_.swap(other.state_);
        return *this;
    }
    /// Releases this handle's share of the bus, which lives on while other handles, voices, one-shots or child buses
    /// retain it.
    ~AudioBus() = default;
    /// Sets the bus gain, in [0, 16], reached as #audio_smoothing_seconds describes. Throws
    /// `std::invalid_argument` with "Audio gain must be in [0, 16]" otherwise.
    void set_volume(float gain);
    /// The gain of the latest set_volume(), or 1 for a new bus, even while the bus is muted or still ramps to it.
    [[nodiscard]] float volume() const;
    /// Silences the bus and its descendants, as a gain of 0 would, without pausing their voices, whose cursors and
    /// fades keep advancing. Unmuting restores the gain of set_volume().
    void set_muted(bool value);
    /// Whether the latest set_muted() muted the bus; false for a new bus.
    [[nodiscard]] bool muted() const;

  private:
    friend class Audio;
    // A bus refers to its engine without owning it, so that a one-shot holding its bus chain never keeps the engine
    // open; its handles retain the engine instead, declared first so that the bus is released before it.
    std::shared_ptr<detail::AudioState> audio_;
    std::shared_ptr<detail::AudioBusState> state_;
};

/// Move-only owner of one voice: one playing instance of a clip, like a Unity AudioSource's playback or an Unreal
/// active sound.
///
/// A new voice is stopped at the start of its clip, with gain 1, pitch 1, pan 0, priority #default_audio_priority,
/// the attenuation of a default AudioAttenuation, looping and spatial mode off and its position at the origin. Each
/// getter of these settings reports the value last set, which a playing voice may still be ramping to. Only a
/// playing voice counts against the engine's voice limit. Destroying or reassigning the Sound stops and releases
/// the voice. The voice retains its clip, bus chain and engine, so it never refers to a destroyed Audio.
class Sound {
  public:
    /// Empty Sound, with no voice; Audio::sound() creates one that has a voice.
    Sound() = default;
    /// Takes the voice of @p other, which becomes empty.
    Sound(Sound &&other) noexcept = default;
    /// Stops and releases this Sound's voice, if any, then takes the voice of @p other, which becomes empty.
    Sound &operator=(Sound &&other) noexcept = default;
    Sound(const Sound &) = delete;
    Sound &operator=(const Sound &) = delete;
    /// Starts or resumes playback at the cursor and returns whether the voice plays. A voice at the end of its
    /// clip restarts from the beginning.
    ///
    /// When the engine already plays Audio::maximum_voices other voices, the one with the lowest priority stops as
    /// stop() stops it; among equal priorities, the one admitted by play() or Audio::play_one_shot longest ago
    /// stops. When that voice's priority is higher than this one's, nothing stops, this voice stays stopped and
    /// play() returns false. This is Unreal's "stop lowest priority" concurrency rule; a stolen voice reports
    /// playing() as false.
    bool play() &;
    /// Deleted: a temporary Sound releases its voice when the full expression ends, so `audio.sound(clip).play()`
    /// would play nothing. Audio::play_one_shot plays a voice that the engine owns instead.
    bool play() && = delete;
    /// Stops playback, keeping the cursor, the gain and any fade in progress.
    void pause();
    /// Stops playback, rewinds to the start and cancels any fade at its current gain; a gain, pan or pitch ramp
    /// still in progress completes at once.
    void stop();
    /// Moves the cursor to @p seconds of clip time, in [0, AudioClip::duration()], at the nearest frame; a playing
    /// voice continues from there. For a streamed clip, a new decoder seeks on the calling thread before the engine
    /// is locked to swap it in, so a playing device never waits for it; for MP3 that decodes the clip from its start
    /// to the new position.
    void seek(double seconds);
    /// Whether the voice is playing. A non-looping voice stops by itself at the end of its clip, leaving the cursor
    /// at the clip duration.
    [[nodiscard]] bool playing() const;
    /// Playback position in seconds of clip time: the clip frames the voice has taken for mixing, which run up to two
    /// clip frames ahead of its latest mixed output, held by its linear resampler.
    [[nodiscard]] double cursor() const;
    /// Loops playback, continuing from the start at the end of the clip.
    void set_looping(bool value);
    /// Whether the voice loops; false for a new voice.
    [[nodiscard]] bool looping() const;
    /// Sets the voice gain, in [0, 16], and replaces any fade; a playing voice reaches it as
    /// #audio_smoothing_seconds describes. Throws `std::invalid_argument` with "Audio gain must be in [0, 16]"
    /// otherwise.
    void set_volume(float gain);
    /// The gain the voice has or ramps to: that of the latest set_volume() or fade(), or the gain at which stop()
    /// cancelled a fade; 1 for a new voice.
    [[nodiscard]] float volume() const;
    /// Sets the playback-rate multiplier, in [0.01, 8], which scales both pitch and duration; a playing voice
    /// reaches it as #audio_smoothing_seconds and #audio_block_frames describe. Clips are resampled linearly to the
    /// output rate, without low-pass filtering. Throws `std::invalid_argument` with "Audio pitch must be in [0.01,
    /// 8]" otherwise.
    void set_pitch(float rate);
    /// The playback-rate multiplier of the latest set_pitch(); 1 for a new voice.
    [[nodiscard]] float pitch() const;
    /// Sets the pan of a nonspatial voice, in [-1, 1] from left to right, reached like set_volume(). Throws
    /// `std::invalid_argument` with "Audio pan must be in [-1, 1]" otherwise.
    ///
    /// Mono clips use equal-power gains, -3 dB on each channel at center: `sqrt((1 - pan) / 2)` on the left and
    /// `sqrt((1 + pan) / 2)` on the right. Stereo clips keep both channels unchanged at center and attenuate the
    /// opposite channel by `sqrt(1 - |pan|)` as they pan. A spatial voice keeps its pan for when it stops being
    /// spatial.
    void set_pan(float value);
    /// The pan of the latest set_pan(), also while the voice is spatial; 0 for a new voice.
    [[nodiscard]] float pan() const;
    /// Sets the priority that play() compares when the voice limit is reached, in [0, 255]; higher values are more
    /// important, as in Unreal (Unity's AudioSource.priority runs the other way). Throws `std::invalid_argument`
    /// with "Audio priority must be in [0, 255]" otherwise.
    void set_priority(int value);
    /// The priority of the latest set_priority(); #default_audio_priority for a new voice.
    [[nodiscard]] int priority() const;
    /// Places the voice relative to the listener (see Audio::set_listener) instead of panning it; switching mode
    /// applies at once.
    ///
    /// A spatial voice's gain falls with distance as set_attenuation() sets, and its output channels are weighted
    /// by direction: with c the cosine between the listener's right vector and the direction from the listener to
    /// the voice, the left channel takes `max(0.2, (1 - c) / 2)` and the right `max(0.2, (1 + c) / 2)`, so a voice
    /// straight ahead plays at half gain on both sides. Within 0.001 of the listener, neither is weighted. A mono
    /// clip feeds both channels and a stereo clip keeps its own. While the voice plays, these gains follow
    /// position, attenuation and listener changes through a linear ramp across one block of
    /// anima::audio_block_frames frames, reaching the new values by the end of the first whole block after the
    /// change; they take their current values at once when the voice starts, resumes or becomes spatial.
    /// Nonspatial voices ignore their position and attenuation.
    void set_spatial(bool value);
    /// Whether the voice is spatial; false for a new voice.
    [[nodiscard]] bool spatial() const;
    /// Sets the position of a spatial voice, in the same space as the listener. Throws `std::invalid_argument`
    /// with "Audio coordinates must be finite and within one billion units" otherwise.
    void set_position(Vec3 value);
    /// The position of the latest set_position(); the origin for a new voice.
    [[nodiscard]] Vec3 position() const;
    /// Sets the spatial distance range and how gain falls within it. Throws `std::invalid_argument` with "Invalid
    /// audio rolloff" for a rolloff that is not an AudioRolloff enumerator, then "Invalid audio attenuation
    /// distances" for a distance that is not finite or outside the ranges AudioAttenuation states.
    void set_attenuation(const AudioAttenuation &value);
    /// The attenuation of the latest set_attenuation(); a default AudioAttenuation for a new voice.
    [[nodiscard]] AudioAttenuation attenuation() const;
    /// Changes the voice gain linearly from its current value to @p target_gain, in [0, 16], over @p seconds of
    /// mixed output, in [0, 86,400]; zero applies it at once. Throws `std::invalid_argument` with "Audio gain must
    /// be in [0, 16]" or "Invalid audio fade duration" otherwise.
    ///
    /// The fade advances only while the voice plays, independent of pitch. A new fade replaces it, set_volume()
    /// replaces it with its own ramp, and stop() cancels it.
    void fade(float target_gain, double seconds);

  private:
    friend class Audio;
    detail::VoiceState &state() const;
    std::shared_ptr<detail::VoiceState> state_;
};

/// The voice settings of Audio::play_one_shot, with the ranges of the Sound setters. The defaults are a new Sound's.
struct AudioOneShot {
    /// Voice gain, in [0, 16]; see Sound::set_volume.
    float volume = 1;
    /// Playback-rate multiplier, in [0.01, 8]; see Sound::set_pitch.
    float pitch = 1;
    /// Pan of a nonspatial one-shot, in [-1, 1]; see Sound::set_pan.
    float pan = 0;
    /// Plays at #position relative to the listener instead of panning; see Sound::set_spatial.
    bool spatial = false;
    /// Position of a spatial one-shot, with every coordinate finite and within 1,000,000,000 of zero.
    Vec3 position{};
    /// How a spatial one-shot's gain falls with distance; see Sound::set_attenuation.
    AudioAttenuation attenuation{};
    /// Priority when the voice limit is reached, in [0, 255]; see Sound::play.
    int priority = default_audio_priority;
};

/// An audio engine: a miniaudio engine with its buses, voices, listener and output, in the role of Unity's audio
/// settings and listener or Unreal's audio device.
///
/// Moving an Audio transfers the engine to the destination. Voices, buses, anima::AudioSource and the codecs of
/// anima::add_audio_component_codecs retain the engine, so moving or destroying the wrapper never invalidates
/// them; the engine and its device close when the last of them is destroyed. One-shots do not retain it: those
/// still playing then stop with it.
class Audio {
  public:
    /// Creates an engine without a device, which mixes only in render(), for tests and offline or headless
    /// rendering. It mixes at @p sample_rate Hz, in [8,000, 192,000], and plays at most @p maximum_voices voices
    /// at once, in [1, 4,096]. The master gain starts at 1 and the listener at the origin, facing -Z with +Y up.
    /// Throws `std::invalid_argument` with "Audio sample rate must be between 8000 and 192000 Hz" or "Audio voice
    /// limit must be in [1, 4096]".
    explicit Audio(unsigned sample_rate = 48000, unsigned maximum_voices = 64);
    /// Creates an engine that plays through a device of @p backend at the device's own sample rate, as the
    /// constructor describes otherwise. The device thread starts mixing before this returns; see the file comment.
    /// Throws `std::invalid_argument` with the voice limit message of the constructor or "Invalid audio backend",
    /// and `std::runtime_error` when no device of @p backend opens.
    [[nodiscard]] static Audio open_device(AudioBackend backend = AudioBackend::platform, unsigned maximum_voices = 64);
    Audio(const Audio &) = delete;
    Audio &operator=(const Audio &) = delete;
    /// Takes the engine of @p other, which is then moved-from.
    Audio(Audio &&other) noexcept = default;
    /// Releases this object's share of its engine, which closes once nothing else retains it, as the class comment
    /// describes, then takes the engine of @p other, which is then moved-from.
    Audio &operator=(Audio &&other) noexcept = default;
    /// Creates a bus under @p parent, or under the master output when @p parent is empty. The parent must belong
    /// to this engine and never changes; bus chains are at most 16 deep.
    [[nodiscard]] AudioBus bus(const AudioBus &parent = {});
    /// Creates a stopped voice for @p clip on @p bus, or on the master output when @p bus is empty. Throws
    /// `std::invalid_argument` with "Missing audio clip or foreign bus" for a null clip or another engine's bus.
    /// The voice of a streamed clip opens its own decoder over the clip's bytes.
    [[nodiscard]] Sound sound(std::shared_ptr<const AudioClip> clip, const AudioBus &bus = {});
    /// Plays @p clip once on @p bus, or on the master output when @p bus is empty, in a voice that the engine owns:
    /// fire and forget, like Unity's AudioSource.PlayOneShot or Unreal's UGameplayStatics::PlaySound2D.
    ///
    /// The voice starts as a new Sound given @p settings through its setters would start on Sound::play(), under
    /// the same voice limit: when every playing voice outranks it, nothing plays, and a later voice can stop it as
    /// it stops a Sound. It plays the clip once, without looping, and ends there or when the voice limit stops it;
    /// nothing can pause, change or query it. It retains its clip and bus chain but not the engine, so one still
    /// playing when the engine closes stops with it.
    ///
    /// The engine releases a one-shot's voice in its first play_one_shot(), render() or anima::synchronize_audio()
    /// after the voice stops, on the thread making that call and never on the device thread, or when the engine
    /// closes; until then voice_count() counts it.
    ///
    /// Throws `std::invalid_argument`, before creating a voice, with the message of the Sound setter for the first
    /// setting outside its range in the order of AudioOneShot's members, then with that of sound() for a null clip
    /// or another engine's bus.
    void play_one_shot(std::shared_ptr<const AudioClip> clip, const AudioOneShot &settings = {},
                       const AudioBus &bus = {});
    /// Whether @p sound holds a voice created by this engine, including after either was moved. False for an
    /// empty Sound or a moved-from engine.
    [[nodiscard]] bool owns(const Sound &sound) const noexcept;
    /// Whether this engine can route to @p bus: the master output (an empty handle) or a bus it created. False
    /// for every bus of a moved-from engine.
    [[nodiscard]] bool owns(const AudioBus &bus) const noexcept;
    /// Sets the master gain, in [0, 16], reached as #audio_smoothing_seconds describes. Throws
    /// `std::invalid_argument` with "Audio gain must be in [0, 16]" otherwise.
    void set_volume(float gain);
    /// The master gain of the latest set_volume(); 1 for a new engine.
    [[nodiscard]] float volume() const;
    /// Places the listener that spatial voices pan and attenuate against.
    ///
    /// Right-handed, as for cameras: the default -Z forward with +Y up puts +X on the listener's right, and facing
    /// +Z swaps left and right. Only @p position and the right vector, `cross(forward, up)` normalized, affect the
    /// mix, so it does not distinguish front from back or above from below. Throws `std::invalid_argument` with
    /// "Audio coordinates must be finite and within one billion units" for a coordinate of any argument that is
    /// not, "Invalid audio listener orientation" when @p forward or @p up is shorter than 0.000001, and "Parallel
    /// audio listener orientation vectors" when the two are parallel.
    void set_listener(Vec3 position, Vec3 forward = view_forward, Vec3 up = world_up);
    /// Output rate in frames per second: the constructor's, or the device's.
    [[nodiscard]] unsigned sample_rate() const;
    /// How many voices, one-shots included, may play at once; see Sound::play.
    [[nodiscard]] unsigned maximum_voices() const;
    /// Voices of this engine that exist, playing or not: every Sound it created that has not been destroyed,
    /// including those of AudioSource components, and every one-shot it has not yet released (see play_one_shot()).
    [[nodiscard]] std::size_t voice_count() const;
    /// Mixes the next `output.size() / 2` stereo frames into @p output, overwriting it. An engine with a device
    /// mixes on its device thread instead, and throws `std::logic_error` here.
    ///
    /// @p output holds interleaved left and right samples, so its size must be even. Voices, fades and ramps
    /// advance by the mixed frames, not wall-clock time, and changes made before the call apply from its first
    /// frame. With no changes in between, rendering the same frames in one call or in several produces identical
    /// samples on the same platform, except while spatial gains ramp (see Sound::set_spatial); cross-platform bitwise
    /// equality is not promised.
    void render(std::span<float> output);

  private:
    friend struct detail::AudioSceneAccess;
    explicit Audio(std::shared_ptr<detail::AudioState> state) : state_(std::move(state)) {}
    detail::AudioState &state() const;
    // Releases the voices of one-shots that stopped, as play_one_shot() describes.
    void release_one_shots();
    std::shared_ptr<detail::AudioState> state_;
};
} // namespace anima
