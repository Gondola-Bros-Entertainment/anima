#include "../detail/audio.hpp"
#include "../detail/json.hpp"
#include "../detail/resource_key.hpp"
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
constexpr std::size_t maximum_component_bytes = 64 * 1024;
void validate(const AudioSourceSettings &s) {
    detail::audio_gain(s.volume);
    detail::audio_pitch(s.pitch);
    detail::audio_pan(s.pan);
    detail::audio_attenuation(s.attenuation);
    detail::audio_priority(s.priority);
}
void apply(Sound &sound, const AudioSourceSettings &s) {
    sound.set_volume(s.volume);
    sound.set_pitch(s.pitch);
    sound.set_pan(s.pan);
    sound.set_attenuation(s.attenuation);
    sound.set_looping(s.looping);
    sound.set_spatial(s.spatial);
    sound.set_priority(s.priority);
}
constexpr std::string_view linear_rolloff = "linear", inverse_rolloff = "inverse";
void key(std::string_view value) { detail::validate_resource_key(value, "Invalid audio clip key"); }
} // namespace
AudioSource::AudioSource(Audio &audio, std::shared_ptr<const AudioClip> clip, AudioSourceSettings settings,
                         const AudioBus &bus)
    : audio_(detail::AudioSceneAccess::retain(audio)), bus_(bus), clip_(std::move(clip)) {
    validate(settings);
    sound_ = audio_.sound(clip_, bus_);
    configure(settings);
    play_pending_ = settings.play_on_start;
}
void AudioSource::configure(AudioSourceSettings settings) {
    validate(settings);
    apply(sound_, settings);
    settings_ = settings;
}
void AudioSource::set_clip(std::shared_ptr<const AudioClip> clip) {
    auto sound = audio_.sound(clip, bus_);
    apply(sound, settings_);
    // Nothing below throws: the old voice is released only once the new one is ready.
    clip_ = std::move(clip);
    sound_ = std::move(sound);
    play_pending_ = resume_ = false;
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
    audio.set_listener(position, forward, up);
    for (auto &update : pending) {
        auto &source = update.component.get();
        if (!update.component.active()) {
            source.resume_ = source.resume_ || source.sound_.playing();
            source.sound_.pause();
            continue;
        }
        source.sound_.set_position(update.position);
        if (source.play_pending_ || source.resume_) {
            source.sound_.play();
            source.play_pending_ = source.resume_ = false;
        }
    }
    audio.release_one_shots();
}

void synchronize_audio(Scene &scene, Audio &audio) { detail::AudioSceneAccess::synchronize(scene, audio); }
void synchronize_audio(SceneSet &scenes, Audio &audio) { detail::AudioSceneAccess::synchronize(scenes, audio); }

void add_audio_component_codecs(ComponentCodecs &codecs, Audio &audio, AudioClipName name, AudioClipResolver resolve,
                                const AudioBus &bus) {
    if (!name || !resolve)
        throw std::invalid_argument("Audio codecs require clip naming and resolution callbacks");
    auto mixer = std::make_shared<Audio>(detail::AudioSceneAccess::retain(audio));
    // Every restore creates its voice on this bus, so a foreign one would make each of them throw.
    if (!audio.owns(bus))
        throw std::invalid_argument("Audio codecs require a bus of the same mixer");
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
        "anima.audio-source.v2",
        [name = std::move(name)](const AudioSource &source, const ObjectReferences &) {
            const auto clip = name(source.clip());
            key(clip);
            const auto &s = source.settings();
            const auto &a = s.attenuation;
            return Json{{"clip", clip},
                        {"volume", s.volume},
                        {"pitch", s.pitch},
                        {"pan", s.pan},
                        {"attenuation",
                         {{"minimum_distance", a.minimum_distance},
                          {"maximum_distance", a.maximum_distance},
                          {"rolloff", a.rolloff == AudioRolloff::inverse ? inverse_rolloff : linear_rolloff}}},
                        {"looping", s.looping},
                        {"spatial", s.spatial},
                        {"play_on_start", s.play_on_start},
                        {"priority", s.priority}}
                .dump();
        },
        [mixer, bus, resolve = std::move(resolve)](GameObject object, std::string_view data, const ObjectReferences &) {
            const auto j = detail::parse_json(data, maximum_component_bytes);
            detail::json_fields(j, {"clip", "volume", "pitch", "pan", "attenuation", "looping", "spatial",
                                    "play_on_start", "priority"});
            if (!j.at("clip").is_string())
                throw std::invalid_argument("Invalid audio source fields");
            const auto &attenuation = detail::json_object(j, "attenuation");
            detail::json_fields(attenuation, {"minimum_distance", "maximum_distance", "rolloff"});
            const auto number = [](const Json &parent, const char *field) {
                if (!parent.at(field).is_number())
                    throw std::invalid_argument("Invalid audio source number");
                return detail::json_float(parent.at(field));
            };
            const auto flag = [&](const char *field) {
                if (!j.at(field).is_boolean())
                    throw std::invalid_argument("Invalid audio source flag");
                return j.at(field).get<bool>();
            };
            AudioSourceSettings s;
            s.volume = number(j, "volume");
            s.pitch = number(j, "pitch");
            s.pan = number(j, "pan");
            s.attenuation.minimum_distance = number(attenuation, "minimum_distance");
            s.attenuation.maximum_distance = number(attenuation, "maximum_distance");
            // Compared as a std::string: comparing the JSON value with a string_view is ambiguous on MSVC.
            const auto *rolloff = attenuation.at("rolloff").get_ptr<const std::string *>();
            if (rolloff && *rolloff == linear_rolloff)
                s.attenuation.rolloff = AudioRolloff::linear;
            else if (rolloff && *rolloff == inverse_rolloff)
                s.attenuation.rolloff = AudioRolloff::inverse;
            else
                throw std::invalid_argument("Invalid audio source rolloff");
            s.looping = flag("looping");
            s.spatial = flag("spatial");
            s.play_on_start = flag("play_on_start");
            const auto &priority = j.at("priority");
            if (!priority.is_number_integer())
                throw std::invalid_argument("Invalid audio source priority");
            // An unsigned JSON integer above INT64_MAX reads as negative, which the range check rejects too.
            const auto value = priority.get<std::int64_t>();
            detail::audio_priority(value);
            s.priority = static_cast<int>(value);
            validate(s);
            const auto clip_key = j.at("clip").get<std::string>();
            key(clip_key);
            auto clip = resolve(clip_key);
            object.add_component<AudioSource>(*mixer, std::move(clip), s, bus);
        });
    codecs = std::move(pending);
}
} // namespace anima
