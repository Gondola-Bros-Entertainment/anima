#pragma once
#include <anima/audio.hpp>
#include <anima/scene.hpp>

namespace anima {
/// The enabled listener uses its object's world position, +Z forward and +Y up.
struct AudioListener {};

struct AudioSourceSettings {
    float volume = 1, pitch = 1, pan = 0;
    float minimum_distance = 1, maximum_distance = 100;
    bool looping = false, spatial = false, play_on_start = false;
};

/// Owns one voice and immutable clip. Removal/scene destruction releases the voice.
/// No device, callback or update loop is started by construction or persistence.
class AudioSource {
  public:
    AudioSource(Audio &audio, std::shared_ptr<const AudioClip> clip, AudioSourceSettings settings = {},
                const AudioBus &bus = {});
    AudioSource(const AudioSource &) = delete;
    AudioSource &operator=(const AudioSource &) = delete;
    [[nodiscard]] const AudioSourceSettings &settings() const { return settings_; }
    [[nodiscard]] const std::shared_ptr<const AudioClip> &clip() const { return clip_; }
    /// Validates the whole configuration before changing the voice. play_on_start
    /// is used only at construction; changing it affects subsequent persisted copies.
    void configure(AudioSourceSettings settings);
    /// Play requests take effect at the next enabled synchronization. Repeated
    /// synchronization never restarts a completed one-shot. Pause/stop are immediate.
    void play();
    void pause();
    void stop();
    void seek(double seconds);
    [[nodiscard]] bool playing() const { return sound_.playing(); }
    [[nodiscard]] double cursor() const { return sound_.cursor(); }

  private:
    friend struct detail::AudioSceneAccess;
    AudioSourceSettings settings_;
    std::shared_ptr<const AudioClip> clip_;
    Sound sound_;
    bool play_pending_{}, resume_{};
};

/// Call after scene/game transforms and before render/AudioOutput::pump. Does not
/// call Scene updates or advance audio time. Validates all bindings/poses first.
/// At most one enabled listener; none resets the mixer's default origin/+Z/+Y.
/// Disabled sources pause and resume on re-enable, preserving cursor and explicit
/// pause/stop. Enablement changes take effect only when this function is called.
/// Use one Scene or SceneSet driver per mixer; standalone voices may share it.
void synchronize_audio(Scene &scene, Audio &audio);
void synchronize_audio(SceneSet &scenes, Audio &audio);

using AudioClipName = std::function<std::string(const std::shared_ptr<const AudioClip> &)>;
using AudioClipResolver = std::function<std::shared_ptr<const AudioClip>(std::string_view)>;
/// Explicit v1 configuration codecs. Caller keys resolve immutable clips; each
/// copy gets a fresh stopped voice, routed to the supplied bus. The codecs retain
/// the mixer context/bus. Cursors, play/pause state and device queues are excluded.
/// Resolver callbacks must not mutate scenes/components or the mixer.
void add_audio_component_codecs(ComponentCodecs &codecs, Audio &audio, AudioClipName name, AudioClipResolver resolve,
                                const AudioBus &bus = {});
} // namespace anima
