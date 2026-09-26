#include "component_payloads.hpp"
#include <anima/audio_scene.hpp>
#include <anima/prefab.hpp>
#include <array>
#include <iostream>
#include <limits>

using namespace anima;
namespace {
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void near(double a, double b) { check(std::abs(a - b) < 1e-6, "Audio scene sample/state mismatch"); }
template <class F> void rejects(F f) {
    bool caught = false;
    try {
        f();
    } catch (const std::exception &) {
        caught = true;
    }
    check(caught, "Expected audio scene rejection");
}
auto clip() { return AudioClip::pcm(std::vector<float>(16, .25F), 1, 8000); }
std::array<float, 2> frame(Audio &audio) {
    std::array<float, 2> result{};
    audio.render(result);
    return result;
}
ComponentCodecs codecs_for(Audio &audio, std::shared_ptr<const AudioClip> sound, const AudioBus &bus = {}) {
    ComponentCodecs codecs;
    add_audio_component_codecs(
        codecs, audio,
        [sound](const auto &value) {
            if (value != sound)
                throw std::invalid_argument("Unregistered clip");
            return "tone";
        },
        [sound](std::string_view key) { return key == "tone" ? sound : nullptr; }, bus);
    return codecs;
}
void playback() {
    Audio audio(8000);
    Scene scene;
    auto group = scene.create();
    group.set_position({10, 0, 0});
    auto listener = scene.create();
    listener.set_parent(group, ReparentMode::keep_local);
    auto ears = listener.add_component<AudioListener>();
    auto emitter = scene.create();
    emitter.set_parent(group, ReparentMode::keep_local);
    emitter.set_local_position({2, 0, 0});
    AudioSourceSettings settings;
    settings.spatial = settings.looping = settings.play_on_start = true;
    settings.minimum_distance = 0;
    settings.maximum_distance = 4;
    auto source = emitter.add_component<AudioSource>(audio, clip(), settings);
    check(!source->playing(), "Construction started playback before synchronization");
    near(frame(audio)[1], 0);
    synchronize_audio(scene, audio);
    auto sample = frame(audio);
    near(sample[0], 0);
    near(sample[1], .125);
    auto rotation = identity();
    rotation[0] = rotation[10] = -2;
    rotation[5] = 3;
    listener.set_local_matrix(rotation); // World orientation, normalized scale.
    synchronize_audio(scene, audio);
    sample = frame(audio);
    near(sample[0], .125);
    near(sample[1], 0);
    const auto cursor = source->cursor();
    source.set_enabled(false);
    synchronize_audio(scene, audio);
    near(frame(audio)[0], 0);
    near(source->cursor(), cursor);
    synchronize_audio(scene, audio);
    source.set_enabled(true);
    synchronize_audio(scene, audio);
    check(source->playing(), "Disabled playback did not resume");
    near(frame(audio)[0], .125);
    source->pause();
    synchronize_audio(scene, audio);
    check(!source->playing(), "Synchronization overrode explicit pause");
    source.set_enabled(false);
    source->play();
    synchronize_audio(scene, audio);
    check(!source->playing(), "Play request bypassed enablement");
    source->stop();
    source.set_enabled(true);
    synchronize_audio(scene, audio);
    check(!source->playing() && source->cursor() == 0, "Stop failed to cancel deferred/resumed play");
    source->play();
    synchronize_audio(scene, audio);
    source.set_enabled(false);
    synchronize_audio(scene, audio);
    source->pause();
    source.set_enabled(true);
    synchronize_audio(scene, audio);
    check(!source->playing(), "Pause while disabled did not cancel resume");
    source->play();
    synchronize_audio(scene, audio);
    ears.set_enabled(false);
    synchronize_audio(scene, audio); // No listener resets origin; emitter is 12 units away.
    near(frame(audio)[0], 0);
    near(frame(audio)[1], 0);
    listener.destroy();
    emitter.set_position({1, 0, 0});
    synchronize_audio(scene, audio);
    near(frame(audio)[1], .1875);
    settings.looping = false;
    source->configure(settings);
    source->seek(source->clip()->duration() - 1. / 8000);
    (void)frame(audio);
    check(!source->playing(), "One-shot failed to finish");
    synchronize_audio(scene, audio);
    near(frame(audio)[1], 0);
    source->play();
    synchronize_audio(scene, audio);
    check(source->playing() && source->cursor() == 0, "Explicit replay did not rewind completed clip");
    emitter.destroy();
    check(!source && !ears, "Destroyed audio component handles remain valid");
    near(frame(audio)[1], 0);
}
void validation() {
    Audio audio(8000), foreign(8000);
    Scene scene;
    auto listener = scene.create();
    listener.add_component<AudioListener>();
    auto first = scene.create();
    auto second = scene.create();
    AudioSourceSettings settings;
    settings.spatial = settings.looping = true;
    auto a = first.add_component<AudioSource>(audio, clip(), settings);
    auto b = second.add_component<AudioSource>(audio, clip(), settings);
    a->play();
    b->play();
    synchronize_audio(scene, audio);
    a.set_enabled(false);
    second.set_position({2e9F, 0, 0});
    rejects([&] { synchronize_audio(scene, audio); });
    check(a->playing() && b->playing(), "Bad later pose partially paused snapshot");
    second.set_position({0, 0, 0});
    auto extra = scene.create();
    extra.add_component<AudioListener>();
    rejects([&] { synchronize_audio(scene, audio); });
    check(a->playing(), "Ambiguous listener partially applied source state");
    extra.destroy();
    auto m = identity();
    m[8] = m[9] = m[10] = 0;
    listener.set_world_matrix(m);
    rejects([&] { synchronize_audio(scene, audio); });
    check(a->playing(), "Invalid listener partially applied source state");
    listener.set_world_matrix(identity());
    auto other = scene.create();
    auto c = other.add_component<AudioSource>(foreign, clip());
    c.set_enabled(false);
    rejects([&] { synchronize_audio(scene, audio); });
    check(a->playing(), "Foreign mixer partially applied snapshot");
    other.destroy();
    synchronize_audio(scene, audio);
    check(!a->playing(), "Valid snapshot did not publish disablement");
    for (unsigned field = 0; field < 5; ++field) {
        auto invalid = settings;
        float *values[] = {&invalid.volume, &invalid.pitch, &invalid.pan, &invalid.minimum_distance,
                           &invalid.maximum_distance};
        *values[field] = std::numeric_limits<float>::quiet_NaN();
        rejects([&] { b->configure(invalid); });
        check(b->settings().volume == 1 && b->settings().looping, "Rejected configuration changed settings");
    }
    auto invalid = settings;
    invalid.pitch = 0;
    rejects([&] { b->configure(invalid); });
    invalid = settings;
    invalid.minimum_distance = invalid.maximum_distance;
    rejects([&] { b->configure(invalid); });
    rejects([&] { b->seek(-1); });
    auto empty = scene.create();
    rejects([&] { empty.add_component<AudioSource>(audio, nullptr); });
    rejects([&] { empty.add_component<AudioSource>(audio, clip(), settings, foreign.bus()); });
    check(!empty.has_component<AudioSource>(), "Failed construction left component attached");
    Sound unbound;
    check(!audio.owns(unbound), "Mixer owns an empty voice");
    auto standalone = audio.sound(clip());
    check(audio.owns(standalone) && !foreign.owns(standalone), "Mixer identity check failed");
    Audio moved(std::move(audio));
    check(moved.owns(standalone) && !audio.owns(standalone), "Mixer move lost voice identity");
    rejects([&] { synchronize_audio(scene, audio); });
    synchronize_audio(scene, moved);
}
void persistence() {
    Audio audio(8000, 8);
    auto tone = clip();
    auto bus = audio.bus();
    bus.volume(.5F);
    auto codecs = codecs_for(audio, tone, bus);
    Scene scene;
    auto root = scene.create("emitter");
    root.set_position({3, 0, 0});
    AudioSourceSettings settings{.5F, 2, -1, 2, 20, true, false, true};
    auto original = root.add_component<AudioSource>(audio, tone, settings);
    auto child = scene.create("listener");
    child.set_parent(root, ReparentMode::keep_local);
    child.add_component<AudioListener>().set_enabled(false);
    synchronize_audio(scene, audio);
    (void)frame(audio);
    (void)frame(audio);
    check(original->cursor() > 0, "Original source did not advance");
    original.set_enabled(false);
    const auto prefab = Prefab::capture(root, codecs);
    const auto document = prefab.serialize({});
    auto copy_object = Prefab::deserialize(document, {}, codecs).instantiate(scene);
    auto copy = copy_object.get_component<AudioSource>();
    check(!copy.enabled() && copy->clip() == tone && copy->cursor() == 0 && !copy->playing(),
          "Prefab copied live state or lost clip/enablement");
    check(copy->settings().volume == .5F && copy->settings().pitch == 2 && copy->settings().pan == -1 &&
              copy->settings().minimum_distance == 2 && copy->settings().maximum_distance == 20 &&
              copy->settings().looping && !copy->settings().spatial && copy->settings().play_on_start,
          "Source settings round trip failed");
    check(!copy_object.children()[0].get_component<AudioListener>().enabled(), "Listener enablement lost");
    synchronize_audio(scene, audio);
    near(frame(audio)[0], 0);
    copy.set_enabled(true);
    synchronize_audio(scene, audio);
    near(frame(audio)[0], .0625); // Explicit restored bus * voice * clip.
    check(original->cursor() != copy->cursor(), "Prefab voices shared cursor state");
    const auto scene_data = serialize_scene(scene, {}, codecs);
    auto restored = load_scene(scene_data, {}, codecs);
    check(restored->size() == 4 && restored->components<AudioSource>().size() == 2, "Scene audio round trip failed");
    for (auto source : restored->components<AudioSource>())
        check(!source->playing() && source->cursor() == 0, "Load started voices or persisted cursor");
    restored.reset();
    root.destroy();
    copy_object.destroy();
    near(frame(audio)[0], 0);
    // Codecs retain the mixer context, even if the original wrapper is moved/destroyed.
    Audio retained(std::move(audio));
    auto again = prefab.instantiate(scene);
    again.get_component<AudioSource>().set_enabled(true);
    synchronize_audio(scene, retained);
    near(frame(retained)[0], .0625);
    auto count = codecs.capture(again, {}).size();
    rejects([&] {
        add_audio_component_codecs(
            codecs, retained, [](const auto &) { return "tone"; }, [tone](auto) { return tone; });
    });
    check(codecs.capture(again, {}).size() == count, "Failed registration changed codecs");
}
void rollback_and_lifetime() {
    Audio audio(8000, 2);
    auto tone = clip();
    auto codecs = codecs_for(audio, tone);
    Scene scene;
    auto existing = scene.create();
    existing.add_component<AudioSource>(audio, tone);
    auto prefab = Prefab::capture(existing, codecs);
    auto nodes = std::vector<Prefab::Node>(prefab.nodes().begin(), prefab.nodes().end());
    nodes.push_back(nodes[0]);
    nodes.back().key = {};
    nodes[1].parent = 0;
    const auto valid = nodes[1].components[0].state;
    auto invalids = invalid_component_payloads(valid, "volume");
    auto replace = [&](std::string from, std::string to) {
        auto value = valid;
        const auto at = value.find(from);
        check(at != std::string::npos, "Test mutation missing field");
        value.replace(at, from.size(), to);
        invalids.push_back(value);
    };
    replace("\"clip\":\"tone\"", "\"clip\":\"missing\"");
    replace("\"clip\":\"tone\"", "\"clip\":\"\"");
    replace("\"pitch\":1.0", "\"pitch\":1e100");
    replace("\"looping\":false", "\"looping\":0");
    replace("\"volume\":1.0", "\"volume\":\"1\"");
    invalids.push_back(std::string(64 * 1024 + 1, ' '));
    for (const auto &invalid : invalids) {
        nodes[1].components[0].state = invalid;
        rejects([&] { (void)Prefab(nodes, codecs).instantiate(scene); });
        check(scene.size() == 1, "Failed prefab leaked objects");
        auto probe = audio.sound(tone); // Earlier decoded source must have released capacity.
        check(audio.owns(probe), "Rollback leaked a voice");
    }
    nodes[1].components[0].state = valid;
    rejects([&] { (void)Prefab(nodes, codecs).instantiate(scene); }); // Capacity failure after first voice.
    check(scene.size() == 1, "Capacity failure leaked prefab");
    unsigned resolutions = 0;
    ComponentCodecs failing;
    add_audio_component_codecs(
        failing, audio, [](const auto &) { return "tone"; },
        [&](std::string_view) -> std::shared_ptr<const AudioClip> {
            if (++resolutions == 2)
                throw std::runtime_error("Synthetic resolver failure");
            return tone;
        });
    rejects([&] { (void)Prefab(nodes, failing).instantiate(scene); });
    check(resolutions == 2 && scene.size() == 1, "Resolver failure did not roll back prior source");
    auto temporary = prefab.instantiate(scene);
    check(temporary.remove_component<AudioSource>(), "Source removal failed");
    temporary.add_component<AudioSource>(audio, tone);
    temporary.destroy();
    existing.destroy();
    {
        Scene owned;
        owned.create().add_component<AudioSource>(audio, tone);
        owned.create().add_component<AudioSource>(audio, tone);
    }
    auto one = audio.sound(tone);
    auto two = audio.sound(tone);
    check(audio.owns(one) && audio.owns(two), "Scene teardown leaked voices");
    // Safe destruction in either order: components and codecs can outlive Audio.
    ComponentCodecs orphan_codecs;
    Scene orphan_scene;
    {
        Audio short_lived;
        orphan_codecs = codecs_for(short_lived, tone);
        orphan_scene.create().add_component<AudioSource>(short_lived, tone);
    }
    auto orphan = Prefab::capture(orphan_scene.roots()[0], orphan_codecs).instantiate(orphan_scene);
    check(orphan.get_component<AudioSource>()->clip() == tone, "Retained codec mixer expired");
    auto listener = orphan_scene.create();
    listener.add_component<AudioListener>();
    auto listener_nodes = std::vector<Prefab::Node>{Prefab::capture(listener, orphan_codecs).nodes()[0]};
    for (const auto &payload : {"[]", "{\"unexpected\":1}", "{\"a\":0,\"a\":1}"}) {
        listener_nodes[0].components[0].state = payload;
        auto before = orphan_scene.size();
        rejects([&] { (void)Prefab(listener_nodes, orphan_codecs).instantiate(orphan_scene); });
        check(orphan_scene.size() == before, "Invalid listener leaked prefab");
    }
}
// A bus of another mixer is rejected when the codecs are registered; accepting it made every later
// restore throw from Audio::sound.
void foreign_bus() {
    Audio audio(8000);
    Audio other(8000);
    const auto foreign = other.bus();
    check(!audio.owns(foreign) && other.owns(foreign) && audio.owns(AudioBus{}), "Bus ownership misreported");
    bool rejected = false;
    try {
        (void)codecs_for(audio, clip(), foreign);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    check(rejected, "Audio codecs accepted another mixer's bus");
    (void)codecs_for(audio, clip(), audio.bus());
}
} // namespace
int main() {
    try {
        playback();
        validation();
        persistence();
        rollback_and_lifetime();
        foreign_bus();
        std::cout << "PASS audio scene transforms, playback, ownership, persistence, rollback and teardown\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
