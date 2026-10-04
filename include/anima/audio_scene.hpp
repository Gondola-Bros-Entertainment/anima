#pragma once
#include <anima/audio.hpp>
#include <anima/scene.hpp>

/// @file
/// Scene integration for anima::Audio: a listener marker, a voice-owning source component, a
/// synchronization driver and persistence codecs.
///
/// Part of `anima::assets`; it needs no device, SDL or graphics. Use it from the scenes' thread; the
/// engine may mix on its device thread meanwhile (see anima::Audio). Invalid arguments throw
/// `std::invalid_argument` unless a member states otherwise.

namespace anima {
/// Marks the object whose world pose the engine's listener follows.
///
/// At synchronize_audio(), an active listener takes its object's world position, faces the local
/// -Z axis and has the local +Y axis up, like a Camera on that object. The axes are normalized, so
/// nonuniform scale is allowed, but zero or parallel axes are rejected.
struct AudioListener {};

/// Voice configuration of an AudioSource, with the ranges of the Sound setters. The defaults are a
/// new Sound's.
struct AudioSourceSettings {
    /// Voice gain, in [0, 16].
    float volume = 1;
    /// Playback-rate multiplier, in [0.01, 8].
    float pitch = 1;
    /// Pan of a nonspatial source, in [-1, 1].
    float pan = 0;
    /// How a spatial source's gain falls with distance. Its distances are in world units; object
    /// scale does not change them.
    AudioAttenuation attenuation{};
    bool looping = false;
    /// Plays at the object's world position; see Sound::set_spatial.
    bool spatial = false;
    /// Requests playback once after construction or loading; see AudioSource::play.
    bool play_on_start = false;
    /// Priority when the engine's voice limit is reached, in [0, 255]; see Sound::play.
    int priority = default_audio_priority;
};

/// Component that owns one voice for an immutable clip, like Unity's AudioSource or Unreal's audio
/// component.
///
/// A source starts stopped. Playback requests take effect at the next synchronize_audio() in which
/// the component is active. An inactive source pauses there, keeping its cursor, and resumes when
/// active again unless it was paused or stopped meanwhile. The source retains its clip and engine;
/// removing the component or destroying its object or scene releases the voice. Neither
/// construction nor persistence starts a device, callback or update loop.
class AudioSource {
  public:
    /// Creates a stopped voice for @p clip in @p audio, routed to @p bus (the master output when
    /// empty) and configured by @p settings. Throws `std::invalid_argument` for invalid settings, a
    /// null clip or another engine's bus, and `std::logic_error` for a moved-from engine.
    AudioSource(Audio &audio, std::shared_ptr<const AudioClip> clip, AudioSourceSettings settings = {},
                const AudioBus &bus = {});
    AudioSource(const AudioSource &) = delete;
    AudioSource &operator=(const AudioSource &) = delete;
    /// The applied configuration.
    [[nodiscard]] const AudioSourceSettings &settings() const { return settings_; }
    /// The clip, fixed at construction.
    [[nodiscard]] const std::shared_ptr<const AudioClip> &clip() const { return clip_; }
    /// Validates all of @p settings, then applies them to the voice at once.
    ///
    /// AudioSourceSettings::play_on_start affects only copies persisted afterwards; use play() for
    /// this source. A source switched to spatial mode takes its object's position at the next
    /// synchronization, so synchronize before rendering.
    void configure(AudioSourceSettings settings);
    /// Requests playback at the next synchronization in which the source is active, resuming a
    /// paused voice or restarting a finished one. Synchronizing again never restarts a voice that
    /// finished or that the engine's voice limit stopped or refused (see Sound::play).
    void play();
    /// Pauses at once, keeping the cursor, and cancels pending or interrupted playback, even while
    /// inactive.
    void pause();
    /// Stops and rewinds at once, and cancels pending or interrupted playback, even while inactive.
    void stop();
    /// Moves the cursor; see Sound::seek.
    void seek(double seconds);
    /// Whether the voice is playing now; a pending play() request does not count.
    [[nodiscard]] bool playing() const { return sound_.playing(); }
    /// Voice cursor in seconds of clip time; see Sound::cursor.
    [[nodiscard]] double cursor() const { return sound_.cursor(); }

  private:
    friend struct detail::AudioSceneAccess;
    AudioSourceSettings settings_;
    std::shared_ptr<const AudioClip> clip_;
    Sound sound_;
    bool play_pending_{}, resume_{};
};

/// Publishes the listener and sources of @p scene to @p audio.
///
/// Everything is validated first, including inactive sources, and `std::invalid_argument` is
/// thrown before any change when a source belongs to another engine, more than one AudioListener
/// is active, the listener's axes are zero or parallel, or a published position has a coordinate
/// that is not finite or exceeds 1,000,000,000 in magnitude. Activity is ComponentRef::active():
/// component enablement and inherited object activation.
///
/// The active listener then places the engine's listener (see Audio::set_listener); without one, the
/// listener returns to the origin facing -Z with +Y up. Active sources take their object's world
/// position when spatial, and play when play() or AudioSourceSettings::play_on_start requested it or
/// when they were playing before becoming inactive; each such request is consumed, whether or not
/// the voice limit admits the voice. Inactive sources pause, keeping their cursor. Enablement
/// changes take effect only here.
///
/// Call it after the application's transform changes. An engine with a device mixes what it
/// publishes from its next block, and one without in its next Audio::render(). It runs no
/// component hooks and does not advance audio time. Last, it releases the engine's one-shots that
/// stopped (see Audio::play_one_shot). Use one Scene or SceneSet driver per engine;
/// standalone voices may share the engine. Throws `std::logic_error` while the scene is updating,
/// under construction or destroyed, and for a moved-from engine.
void synchronize_audio(Scene &scene, Audio &audio);
/// Synchronizes every scene of @p scenes as synchronize_audio(Scene &, Audio &) does, validating
/// all of them before any changes; at most one listener may be active across the set. Also throws
/// `std::logic_error` while the set is busy, as SceneSet describes.
void synchronize_audio(SceneSet &scenes, Audio &audio);

/// Returns the persistent key of a clip when a source is captured.
using AudioClipName = std::function<std::string(const std::shared_ptr<const AudioClip> &)>;
/// Returns the clip for a persistent key when a source is restored.
using AudioClipResolver = std::function<std::shared_ptr<const AudioClip>(std::string_view)>;
/// Registers the `anima.audio-listener.v1` and `anima.audio-source.v2` component codecs.
///
/// The codecs retain the engine of @p audio and @p bus, which must belong to that engine: with a
/// foreign bus, every source restore throws. Each restored source gets its own stopped voice on
/// @p bus (the master output when empty) and the clip @p resolve returns, and its play_on_start
/// request waits for synchronize_audio().
///
/// The listener payload is `{}`. The source payload is a JSON object of at most 64 KiB with `clip`
/// (the key), `volume`, `pitch`, `pan`, `attenuation`, `looping`, `spatial`, `play_on_start` and
/// `priority` (an integer), as in AudioSourceSettings; `attenuation` is an object with
/// `minimum_distance`, `maximum_distance` and `rolloff` (`"linear"` or `"inverse"`), as in
/// AudioAttenuation. Every field is required: a missing one throws `std::invalid_argument` with
/// "Missing JSON field: " followed by its name. Unknown or duplicate fields, wrong types and invalid
/// values are rejected too, as is a null clip from @p resolve. Keys must be nonempty, at most 4,096
/// bytes and well-formed UTF-8 without NUL (no overlong forms or surrogates), both when captured
/// and when restored; another key throws `std::invalid_argument` with "Invalid audio clip key",
/// on restore before @p resolve runs. The scene or prefab stores transforms and component
/// enablement; cursors, playback state, pending requests and bus settings are not persisted.
///
/// Neither callback may mutate scenes, components or the engine. Throws `std::invalid_argument` for
/// an empty callback, a @p bus of another engine, or when @p codecs already has a codec for either
/// component type or key, leaving @p codecs unchanged, and `std::logic_error` for a moved-from
/// engine.
void add_audio_component_codecs(ComponentCodecs &codecs, Audio &audio, AudioClipName name, AudioClipResolver resolve,
                                const AudioBus &bus = {});
} // namespace anima
