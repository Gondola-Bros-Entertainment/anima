#include "../detail/audio.hpp"
#include "../detail/json.hpp"
#include "../detail/scene_driver.hpp"
#include <anima/audio_scene.hpp>

namespace anima {
namespace detail {
struct AudioSceneAccess {
    template <class Scenes> static void synchronize(Scenes &scenes, Audio &audio);
    static Audio retain(Audio &audio) {
        (void)audio.state();
        return Audio(audio.state_);
    }
};
} // namespace detail
namespace {
constexpr std::size_t maximum_clip_key_bytes = 4096;
constexpr std::size_t maximum_component_bytes = 64 * 1024;
void validate(const AudioSourceSettings &s) {
    detail::audio_gain(s.volume);
    detail::audio_pitch(s.pitch);
    detail::audio_pan(s.pan);
    detail::audio_attenuation(s.minimum_distance, s.maximum_distance);
}
void key(std::string_view value) {
    if (value.empty() || value.size() > maximum_clip_key_bytes || value.find('\0') != std::string_view::npos)
        throw std::invalid_argument("Invalid audio clip key");
}
} // namespace
AudioSource::AudioSource(Audio &audio, std::shared_ptr<const AudioClip> clip, AudioSourceSettings settings,
                         const AudioBus &bus)
    : clip_(std::move(clip)) {
    validate(settings);
    sound_ = audio.sound(clip_, bus);
    configure(settings);
    play_pending_ = settings.play_on_start;
}
void AudioSource::configure(AudioSourceSettings settings) {
    validate(settings);
    sound_.volume(settings.volume);
    sound_.pitch(settings.pitch);
    sound_.pan(settings.pan);
    sound_.attenuation(settings.minimum_distance, settings.maximum_distance);
    sound_.looping(settings.looping);
    sound_.spatial(settings.spatial);
    settings_ = settings;
}
void AudioSource::play() { play_pending_ = true; }
void AudioSource::pause() {
    sound_.pause();
    play_pending_ = resume_ = false;
}
void AudioSource::stop() {
    sound_.stop();
    play_pending_ = resume_ = false;
}
void AudioSource::seek(double seconds) { sound_.seek(seconds); }

template <class Scenes> void detail::AudioSceneAccess::synchronize(Scenes &scenes, Audio &audio) {
    SceneDriver::check(scenes);
    (void)audio.sample_rate(); // Reject a moved-from mixer even for an empty scene.
    Vec3 position{}, forward = view_forward, up = world_up;
    bool have_listener = false;
    for (auto listener : scenes.template components<AudioListener>()) {
        if (!listener.active())
            continue;
        if (have_listener)
            throw std::invalid_argument("Audio scene has multiple enabled listeners");
        have_listener = true;
        const auto m = listener.object().world_matrix();
        position = translation_of(m);
        forward = -axis_z(m); // Listeners face local -Z, as cameras do.
        up = axis_y(m);
        detail::audio_location(position);
        (void)detail::audio_right(forward, up);
    }
    struct Pending {
        ComponentRef<AudioSource> component;
        Vec3 position;
    };
    std::vector<Pending> pending;
    for (auto source : scenes.template components<AudioSource>()) {
        if (!audio.owns(source->sound_))
            throw std::invalid_argument("Audio source belongs to another mixer");
        const auto p = source.active() && source->settings_.spatial ? source.object().position() : Vec3{};
        detail::audio_location(p);
        pending.push_back({source, p});
    }
    // No callbacks, allocation or remaining validation failures during publication.
    audio.listener(position, forward, up);
    for (auto &update : pending) {
        auto &source = update.component.get();
        if (!update.component.active()) {
            source.resume_ = source.resume_ || source.sound_.playing();
            source.sound_.pause();
            continue;
        }
        source.sound_.position(update.position);
        if (source.play_pending_ || source.resume_) {
            source.sound_.play();
            source.play_pending_ = source.resume_ = false;
        }
    }
}

void synchronize_audio(Scene &scene, Audio &audio) { detail::AudioSceneAccess::synchronize(scene, audio); }
void synchronize_audio(SceneSet &scenes, Audio &audio) { detail::AudioSceneAccess::synchronize(scenes, audio); }

void add_audio_component_codecs(ComponentCodecs &codecs, Audio &audio, AudioClipName name, AudioClipResolver resolve,
                                const AudioBus &bus) {
    if (!name || !resolve)
        throw std::invalid_argument("Audio codecs require clip naming and resolution callbacks");
    auto mixer = std::make_shared<Audio>(detail::AudioSceneAccess::retain(audio));
    // Keep registration atomic if either type/key is already registered.
    auto pending = codecs;
    using Json = nlohmann::json;
    pending.add<AudioListener>(
        "anima.audio-listener.v1", [](const AudioListener &, const ObjectReferences &) { return "{}"; },
        [](GameObject object, std::string_view data, const ObjectReferences &) {
            const auto j = detail::parse_json(data, maximum_component_bytes);
            detail::json_fields(j, {});
            object.add_component<AudioListener>();
        });
    pending.add<AudioSource>(
        "anima.audio-source.v1",
        [name = std::move(name)](const AudioSource &source, const ObjectReferences &) {
            const auto clip = name(source.clip());
            key(clip);
            const auto &s = source.settings();
            return Json{{"clip", clip},
                        {"volume", s.volume},
                        {"pitch", s.pitch},
                        {"pan", s.pan},
                        {"minimum_distance", s.minimum_distance},
                        {"maximum_distance", s.maximum_distance},
                        {"looping", s.looping},
                        {"spatial", s.spatial},
                        {"play_on_start", s.play_on_start}}
                .dump();
        },
        [mixer, bus, resolve = std::move(resolve)](GameObject object, std::string_view data, const ObjectReferences &) {
            const auto j = detail::parse_json(data, maximum_component_bytes);
            detail::json_fields(j, {"clip", "volume", "pitch", "pan", "minimum_distance", "maximum_distance", "looping",
                                    "spatial", "play_on_start"});
            if (!j.at("clip").is_string())
                throw std::invalid_argument("Invalid audio source fields");
            const auto number = [&](const char *field) {
                if (!j.at(field).is_number())
                    throw std::invalid_argument("Invalid audio source number");
                return j.at(field).get<float>();
            };
            const auto flag = [&](const char *field) {
                if (!j.at(field).is_boolean())
                    throw std::invalid_argument("Invalid audio source flag");
                return j.at(field).get<bool>();
            };
            AudioSourceSettings s;
            s.volume = number("volume");
            s.pitch = number("pitch");
            s.pan = number("pan");
            s.minimum_distance = number("minimum_distance");
            s.maximum_distance = number("maximum_distance");
            s.looping = flag("looping");
            s.spatial = flag("spatial");
            s.play_on_start = flag("play_on_start");
            validate(s);
            const auto clip_key = j.at("clip").get<std::string>();
            key(clip_key);
            auto clip = resolve(clip_key);
            object.add_component<AudioSource>(*mixer, std::move(clip), s, bus);
        });
    codecs = std::move(pending);
}
} // namespace anima
