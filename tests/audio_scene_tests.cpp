#include "component_payloads.hpp"
#include "near.hpp"
#include <anima/audio_scene.hpp>
#include <anima/prefab.hpp>
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace anima;
namespace {
constexpr double tolerance = 1e-4;
constexpr auto pitch_range = "Audio pitch must be in [0.01, 8]";
constexpr auto distances = "Invalid audio attenuation distances";
constexpr auto missing_or_foreign = "Missing audio clip or foreign bus";
constexpr auto duplicate_field = "Duplicate JSON document field";

auto clip() { return AudioClip::pcm(std::vector<float>(16, .25F), 1, 8000); }
// The last frame of the next block, which follows any voice started before it by more than its resampler's delay.
std::array<float, 2> frame(Audio &audio) {
    std::array<float, 2 * audio_block_frames> output{};
    audio.render(output);
    return {output[output.size() - 2], output.back()};
}
// The last frame after 0.25 s of output, once spatial gains have settled after a move.
std::array<float, 2> settle(Audio &audio) {
    std::vector<float> output(4000);
    audio.render(output);
    return {output[output.size() - 2], output.back()};
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
// invalid_component_payloads(valid, field), each with the error of a decoder whose first required field is
// @p first_required.
std::vector<std::pair<std::string, std::string>> invalid_payloads(std::string_view valid, std::string_view field,
                                                                  std::string_view first_required) {
    const auto payloads = invalid_component_payloads(valid, field);
    const std::vector<std::string> errors{"Missing JSON field: " + std::string(first_required),
                                          "Missing JSON field: " + std::string(field),
                                          "Unknown JSON field: unexpected",
                                          duplicate_field,
                                          duplicate_field,
                                          "JSON document exceeds nesting limit"};
    REQUIRE(payloads.size() == errors.size());
    std::vector<std::pair<std::string, std::string>> result;
    for (std::size_t i = 0; i < payloads.size(); ++i)
        result.emplace_back(payloads[i], errors[i]);
    return result;
}
} // namespace

TEST_CASE("Sources follow the listener's world pose, their enablement and playback requests") {
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
    settings.attenuation = {0, 4};
    auto source = emitter.add_component<AudioSource>(audio, clip(), settings);
    CHECK_FALSE(source->playing()); // Construction waits for synchronization.
    CHECK(frame(audio)[1] == 0);
    synchronize_audio(scene, audio);
    // Two units to the listener's right: half gain, all of it on the right and the 0.2 floor on the left.
    auto sample = frame(audio);
    CHECK(sample[0] == Near{.025, tolerance});
    CHECK(sample[1] == Near{.125, tolerance});
    auto rotation = identity();
    rotation[0] = rotation[10] = -2;
    rotation[5] = 3;
    listener.set_local_matrix(rotation); // World orientation, normalized scale: facing +Z.
    synchronize_audio(scene, audio);
    sample = settle(audio);
    CHECK(sample[0] == Near{.125, tolerance});
    CHECK(sample[1] == Near{.025, tolerance});
    const auto cursor = source->cursor();
    source.set_enabled(false);
    synchronize_audio(scene, audio);
    CHECK(frame(audio)[0] == 0);
    CHECK(source->cursor() == Near{cursor, 1e-9});
    synchronize_audio(scene, audio);
    source.set_enabled(true);
    synchronize_audio(scene, audio);
    CHECK(source->playing()); // Playback interrupted by disabling resumes.
    CHECK(frame(audio)[0] == Near{.125, tolerance});
    source->pause();
    synchronize_audio(scene, audio);
    CHECK_FALSE(source->playing()); // Synchronization keeps an explicit pause.
    source.set_enabled(false);
    source->play();
    synchronize_audio(scene, audio);
    CHECK_FALSE(source->playing()); // A play request waits for enablement.
    source->stop();
    source.set_enabled(true);
    synchronize_audio(scene, audio);
    CHECK_FALSE(source->playing()); // Stop cancels the deferred play.
    CHECK(source->cursor() == 0);
    source->play();
    synchronize_audio(scene, audio);
    source.set_enabled(false);
    synchronize_audio(scene, audio);
    source->pause();
    source.set_enabled(true);
    synchronize_audio(scene, audio);
    CHECK_FALSE(source->playing()); // Pausing while disabled cancels the resume.
    source->play();
    synchronize_audio(scene, audio);
    ears.set_enabled(false);
    synchronize_audio(scene, audio); // No listener resets origin; emitter is 12 units away.
    sample = settle(audio);
    CHECK(sample[0] == Near{0, tolerance});
    CHECK(sample[1] == Near{0, tolerance});
    listener.destroy();
    emitter.set_position({1, 0, 0});
    synchronize_audio(scene, audio);
    CHECK(settle(audio)[1] == Near{.1875, tolerance});
    settings.looping = false;
    source->configure(settings);
    source->seek(source->clip()->duration() - 1. / 8000);
    (void)frame(audio);
    CHECK_FALSE(source->playing()); // The one-shot finished.
    synchronize_audio(scene, audio);
    CHECK(frame(audio)[1] == 0);
    source->play();
    synchronize_audio(scene, audio);
    CHECK(source->playing()); // An explicit replay rewinds the completed clip.
    CHECK(source->cursor() == 0);
    emitter.destroy();
    CHECK_FALSE(source);
    CHECK_FALSE(ears);
    CHECK(frame(audio)[1] == 0);
    CHECK(audio.voice_count() == 0u);
}

TEST_CASE("Swapping a source's clip keeps its settings and bus and leaves it stopped") {
    Audio audio(8000);
    auto bus = audio.bus();
    Scene scene;
    AudioSourceSettings settings;
    settings.volume = .5F;
    settings.looping = true;
    auto source = scene.create().add_component<AudioSource>(audio, clip(), settings, bus);
    const auto first = source->clip();
    source->play();
    synchronize_audio(scene, audio);
    REQUIRE(source->playing());
    CHECK_THROWS_WITH_AS(source->set_clip(nullptr), missing_or_foreign, std::invalid_argument);
    CHECK(source->clip() == first); // A failed swap keeps the clip and its voice.
    CHECK(source->playing());
    CHECK(audio.voice_count() == 1u);

    const std::shared_ptr<const AudioClip> louder = AudioClip::pcm(std::vector<float>(16, .5F), 1, 8000);
    source->pause();
    source.set_enabled(false);
    source->play();
    source->set_clip(louder);
    CHECK(source->clip() == louder);
    CHECK_FALSE(source->playing());
    CHECK(source->cursor() == 0);
    CHECK(audio.voice_count() == 1u); // The old voice was released.
    CHECK(source->settings().volume == .5F);
    source.set_enabled(true);
    synchronize_audio(scene, audio);
    CHECK_FALSE(source->playing()); // The swap cancelled the pending play.

    source->play();
    synchronize_audio(scene, audio);
    REQUIRE(source->playing());
    // Gain 0.5 on a mono clip of 0.5 at center pan, -3 dB on each channel.
    const auto sample = settle(audio);
    CHECK(sample[0] == Near{.25 * std::sqrt(.5), tolerance});
    CHECK(sample[1] == Near{.25 * std::sqrt(.5), tolerance});
    bus.set_muted(true); // The new voice is routed to the source's bus.
    CHECK(settle(audio)[1] == Near{0, tolerance});
}

TEST_CASE("A source's priority decides which source the voice limit stops") {
    Audio audio(8000, 1);
    Scene scene;
    AudioSourceSettings quiet, important;
    quiet.looping = important.looping = true;
    quiet.priority = 10;
    important.priority = 200;
    auto low = scene.create().add_component<AudioSource>(audio, clip(), quiet);
    auto high = scene.create().add_component<AudioSource>(audio, clip(), important);
    low->play();
    synchronize_audio(scene, audio);
    CHECK(low->playing());
    high->play();
    synchronize_audio(scene, audio);
    CHECK(high->playing());
    CHECK_FALSE(low->playing()); // Stolen, and synchronization does not restart it.
    synchronize_audio(scene, audio);
    CHECK_FALSE(low->playing());
    low->play();
    synchronize_audio(scene, audio); // Refused: the request is consumed.
    CHECK_FALSE(low->playing());
    high->stop();
    synchronize_audio(scene, audio);
    CHECK_FALSE(low->playing());
}

TEST_CASE("Synchronization releases the engine's one-shots that stopped, including those its sources stop") {
    Audio audio(8000, 1);
    Scene scene;
    const auto tone = clip();
    audio.play_one_shot(tone);
    AudioSourceSettings important;
    important.priority = 200;
    important.play_on_start = true;
    scene.create().add_component<AudioSource>(audio, tone, important);
    CHECK(audio.voice_count() == 2u);
    synchronize_audio(scene, audio); // The source stops the one-shot, and the same call releases it.
    CHECK(audio.voice_count() == 1u);
    CHECK(tone.use_count() == 3); // Here, in the source and in its voice.
}

TEST_CASE("Synchronization validates the whole scene before publishing any change") {
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
    CHECK_THROWS_WITH_AS(synchronize_audio(scene, audio),
                         "Audio coordinates must be finite and within one billion units", std::invalid_argument);
    // The later source's pose failed, so the earlier source's disablement was not published either.
    CHECK(a->playing());
    CHECK(b->playing());
    second.set_position({0, 0, 0});
    auto extra = scene.create();
    extra.add_component<AudioListener>();
    CHECK_THROWS_WITH_AS(synchronize_audio(scene, audio), "Audio scene has multiple enabled listeners",
                         std::invalid_argument);
    CHECK(a->playing());
    extra.destroy();
    auto m = identity();
    m[8] = m[9] = m[10] = 0;
    listener.set_world_matrix(m);
    CHECK_THROWS_WITH_AS(synchronize_audio(scene, audio), "Invalid audio listener orientation", std::invalid_argument);
    CHECK(a->playing());
    listener.set_world_matrix(identity());
    auto other = scene.create();
    auto c = other.add_component<AudioSource>(foreign, clip());
    c.set_enabled(false);
    CHECK_THROWS_WITH_AS(synchronize_audio(scene, audio), "Audio source belongs to another mixer",
                         std::invalid_argument);
    CHECK(a->playing());
    other.destroy();
    synchronize_audio(scene, audio);
    CHECK_FALSE(a->playing()); // A valid snapshot publishes the disablement.
}

TEST_CASE("Invalid source settings, clips and buses are rejected without changing the source") {
    Audio audio(8000), foreign(8000);
    Scene scene;
    AudioSourceSettings settings;
    settings.spatial = settings.looping = true;
    auto source = scene.create().add_component<AudioSource>(audio, clip(), settings);
    constexpr auto not_a_number = std::numeric_limits<float>::quiet_NaN();
    const std::array<std::pair<AudioSourceSettings, const char *>, 5> invalids{{
        {{.volume = not_a_number}, "Audio gain must be in [0, 16]"},
        {{.pitch = not_a_number}, pitch_range},
        {{.pan = not_a_number}, "Audio pan must be in [-1, 1]"},
        {{.attenuation = {.minimum_distance = not_a_number}}, distances},
        {{.attenuation = {.maximum_distance = not_a_number}}, distances},
    }};
    for (std::size_t i = 0; i < invalids.size(); ++i) {
        CAPTURE(i);
        CHECK_THROWS_WITH_AS(source->configure(invalids[i].first), invalids[i].second, std::invalid_argument);
        CHECK(source->settings().volume == 1);
        CHECK(source->settings().looping);
    }
    auto invalid = settings;
    invalid.pitch = 0;
    CHECK_THROWS_WITH_AS(source->configure(invalid), pitch_range, std::invalid_argument);
    invalid = settings;
    invalid.attenuation.minimum_distance = invalid.attenuation.maximum_distance;
    CHECK_THROWS_WITH_AS(source->configure(invalid), distances, std::invalid_argument);
    invalid = settings;
    invalid.attenuation = {0, 4, AudioRolloff::inverse};
    CHECK_THROWS_WITH_AS(source->configure(invalid), distances, std::invalid_argument);
    invalid = settings;
    invalid.priority = 256;
    CHECK_THROWS_WITH_AS(source->configure(invalid), "Audio priority must be in [0, 255]", std::invalid_argument);
    CHECK(source->settings().priority == default_audio_priority);
    CHECK_THROWS_WITH_AS(source->seek(-1), "Audio seek lies outside the clip", std::invalid_argument);
    auto empty = scene.create();
    CHECK_THROWS_WITH_AS(empty.add_component<AudioSource>(audio, nullptr), missing_or_foreign, std::invalid_argument);
    CHECK_THROWS_WITH_AS(empty.add_component<AudioSource>(audio, clip(), settings, foreign.bus()), missing_or_foreign,
                         std::invalid_argument);
    CHECK_FALSE(empty.has_component<AudioSource>()); // Failed construction left nothing attached.
    CHECK(audio.voice_count() == 1u);
}

TEST_CASE("An engine keeps its voices when moved, and its moved-from wrapper cannot synchronize") {
    Audio audio(8000), foreign(8000);
    Scene scene;
    scene.create().add_component<AudioSource>(audio, clip());
    const Sound unbound;
    CHECK_FALSE(audio.owns(unbound));
    auto standalone = audio.sound(clip());
    CHECK(audio.owns(standalone));
    CHECK_FALSE(foreign.owns(standalone));
    Audio moved(std::move(audio));
    CHECK(moved.owns(standalone));
    CHECK_FALSE(audio.owns(standalone));
    CHECK_THROWS_WITH_AS(synchronize_audio(scene, audio), "Moved-from audio mixer", std::logic_error);
    CHECK_NOTHROW(synchronize_audio(scene, moved));
    CHECK(moved.voice_count() == 2u);
}

TEST_CASE("Prefabs and scenes persist source settings and enablement, not playback state") {
    Audio audio(8000, 8);
    auto tone = clip();
    auto bus = audio.bus();
    bus.set_volume(.5F);
    auto codecs = codecs_for(audio, tone, bus);
    Scene scene;
    auto root = scene.create("emitter");
    root.set_position({3, 0, 0});
    AudioSourceSettings settings{.5F, 2, -1, {2, 20, AudioRolloff::inverse}, true, false, true, 7};
    auto original = root.add_component<AudioSource>(audio, tone, settings);
    auto child = scene.create("listener");
    child.set_parent(root, ReparentMode::keep_local);
    child.add_component<AudioListener>().set_enabled(false);
    synchronize_audio(scene, audio);
    (void)frame(audio);
    (void)frame(audio);
    CHECK(original->cursor() > 0);
    original.set_enabled(false);
    const auto prefab = Prefab::capture(root, codecs);
    const auto document = prefab.serialize({});
    auto copy_object = Prefab::deserialize(document, {}, codecs).instantiate(scene);
    auto copy = copy_object.get_component<AudioSource>();
    // The copy keeps the clip and enablement, but not the original's cursor or playback.
    CHECK_FALSE(copy.enabled());
    CHECK(copy->clip() == tone);
    CHECK(copy->cursor() == 0);
    CHECK_FALSE(copy->playing());
    const auto &restored_settings = copy->settings();
    CHECK(restored_settings.volume == .5F);
    CHECK(restored_settings.pitch == 2);
    CHECK(restored_settings.pan == -1);
    CHECK(restored_settings.attenuation == AudioAttenuation{2, 20, AudioRolloff::inverse});
    CHECK(restored_settings.looping);
    CHECK_FALSE(restored_settings.spatial);
    CHECK(restored_settings.play_on_start);
    CHECK(restored_settings.priority == 7);
    REQUIRE(copy_object.children().size() == 1u);
    CHECK_FALSE(copy_object.children()[0].get_component<AudioListener>().enabled());
    synchronize_audio(scene, audio);
    CHECK(frame(audio)[0] == 0);
    copy.set_enabled(true);
    synchronize_audio(scene, audio);
    CHECK(frame(audio)[0] == Near{.0625, 1e-6}); // Explicit restored bus * voice * clip.
    const auto scene_data = serialize_scene(scene, {}, codecs);
    auto restored = load_scene(scene_data, {}, codecs);
    CHECK(restored->size() == 4u);
    CHECK(restored->components<AudioSource>().size() == 2u);
    const auto sources = restored->components<AudioSource>();
    for (std::size_t i = 0; i < sources.size(); ++i) {
        CAPTURE(i);
        CHECK_FALSE(sources[i]->playing()); // Loading starts no voice and persists no cursor.
        CHECK(sources[i]->cursor() == 0);
    }
    restored.reset();
    root.destroy();
    copy_object.destroy();
    CHECK(frame(audio)[0] == 0);
    // Codecs retain the engine, even if the original wrapper is moved/destroyed.
    Audio retained(std::move(audio));
    auto again = prefab.instantiate(scene);
    again.get_component<AudioSource>().set_enabled(true);
    synchronize_audio(scene, retained);
    CHECK(frame(retained)[0] == Near{.0625, 1e-6});
    const auto count = codecs.capture(again, {}).size();
    const auto name = [](const auto &) { return "tone"; };
    const auto resolve = [tone](auto) { return tone; };
    CHECK_THROWS_WITH_AS(add_audio_component_codecs(codecs, retained, name, resolve), "Duplicate component codec",
                         std::invalid_argument);
    CHECK(codecs.capture(again, {}).size() == count); // The failed registration changed nothing.
}

TEST_CASE("A source payload requires every field, including each field of its attenuation") {
    Audio audio(8000);
    auto tone = clip();
    auto codecs = codecs_for(audio, tone);
    Scene scene;
    auto object = scene.create();
    object.add_component<AudioSource>(audio, tone);
    const auto prefab = Prefab::capture(object, codecs);
    auto nodes = std::vector<Prefab::Node>(prefab.nodes().begin(), prefab.nodes().end());
    REQUIRE(nodes[0].components.size() == 1u);
    CHECK(nodes[0].components[0].type == "anima.audio-source.v2");
    const auto valid = nodes[0].components[0].state;
    CHECK(valid == R"({"attenuation":{"maximum_distance":100.0,"minimum_distance":1.0,"rolloff":"linear"},)"
                   R"("clip":"tone","looping":false,"pan":0.0,"pitch":1.0,"play_on_start":false,"priority":128,)"
                   R"("spatial":false,"volume":1.0})");
    // The valid payload without a field and its value. Each field name occurs once, and only the attenuation's
    // value holds a comma or a brace.
    const auto without = [&](std::string_view field) {
        auto payload = valid;
        const auto key = "\"" + std::string(field) + "\":";
        auto begin = payload.find(key);
        REQUIRE(begin != std::string::npos);
        auto end = begin + key.size();
        end = payload[end] == '{' ? payload.find('}', end) + 1 : payload.find_first_of(",}", end);
        // Also remove the comma that separates the field from the next one, or else from the previous one.
        if (payload[end] == ',')
            ++end;
        else if (payload[begin - 1] == ',')
            --begin;
        return payload.erase(begin, end - begin);
    };
    for (const auto *field : {"clip", "volume", "pitch", "pan", "attenuation", "minimum_distance", "maximum_distance",
                              "rolloff", "looping", "spatial", "play_on_start", "priority"}) {
        CAPTURE(field);
        nodes[0].components[0].state = without(field);
        const auto error = "Missing JSON field: " + std::string(field);
        CHECK_THROWS_WITH_AS(Prefab(nodes, codecs).instantiate(scene), error.c_str(), std::invalid_argument);
        CHECK(scene.size() == 1u);
        CHECK(audio.voice_count() == 1u);
    }
    // The attenuation is an object with no other fields.
    const auto replaced = [&](std::string_view from, std::string_view to) {
        auto payload = valid;
        const auto at = payload.find(from);
        REQUIRE(at != std::string::npos);
        return payload.replace(at, from.size(), to);
    };
    nodes[0].components[0].state =
        replaced(R"({"maximum_distance":100.0,"minimum_distance":1.0,"rolloff":"linear"})", R"([100.0,1.0,"linear"])");
    CHECK_THROWS_WITH_AS(Prefab(nodes, codecs).instantiate(scene), "JSON field must be an object: attenuation",
                         std::invalid_argument);
    nodes[0].components[0].state = replaced(R"("rolloff":"linear")", R"("rolloff":"linear","unexpected":0)");
    CHECK_THROWS_WITH_AS(Prefab(nodes, codecs).instantiate(scene), "Unknown JSON field: unexpected",
                         std::invalid_argument);
    nodes[0].components[0].state = replaced(R"("maximum_distance":100.0)", R"("maximum_distance":"100")");
    CHECK_THROWS_WITH_AS(Prefab(nodes, codecs).instantiate(scene), "Invalid audio source number",
                         std::invalid_argument);
    nodes[0].components[0].state = replaced(R"("minimum_distance":1.0)", R"("minimum_distance":100.0)");
    CHECK_THROWS_WITH_AS(Prefab(nodes, codecs).instantiate(scene), distances, std::invalid_argument);
    CHECK(scene.size() == 1u);
    nodes[0].components[0].state = valid;
    auto restored = Prefab(nodes, codecs).instantiate(scene).get_component<AudioSource>();
    CHECK(restored->settings().attenuation == AudioAttenuation{});
    CHECK(restored->settings().priority == default_audio_priority);
}

TEST_CASE("A failed restore rolls back its objects and voices, and teardown releases voices") {
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
    // Each invalid state of the second source, with its error.
    auto invalids = invalid_payloads(valid, "volume", "clip");
    const auto replace = [&](std::string_view from, std::string_view to, const char *error) {
        auto value = valid;
        const auto at = value.find(from);
        REQUIRE(at != std::string::npos);
        value.replace(at, from.size(), to);
        invalids.emplace_back(value, error);
    };
    replace(R"("clip":"tone")", R"("clip":"missing")", missing_or_foreign); // The resolver returns no clip.
    replace(R"("clip":"tone")", R"("clip":"")", "Invalid audio clip key");
    replace(R"("pitch":1.0)", R"("pitch":1e100)", "JSON number outside the float range");
    replace(R"("looping":false)", R"("looping":0)", "Invalid audio source flag");
    replace(R"("volume":1.0)", R"("volume":"1")", "Invalid audio source number");
    replace(R"("priority":128)", R"("priority":1.5)", "Invalid audio source priority");
    replace(R"("priority":128)", R"("priority":300)", "Audio priority must be in [0, 255]");
    replace(R"("priority":128)", R"("priority":18446744073709551615)", "Audio priority must be in [0, 255]");
    replace(R"("rolloff":"linear")", R"("rolloff":"cubic")", "Invalid audio source rolloff");
    replace(R"("rolloff":"linear")", R"("rolloff":1)", "Invalid audio source rolloff");
    invalids.emplace_back(std::string(64 * 1024 + 1, ' '), "JSON document exceeds byte limit");
    for (std::size_t i = 0; i < invalids.size(); ++i) {
        CAPTURE(i);
        nodes[1].components[0].state = invalids[i].first;
        CHECK_THROWS_WITH_AS(Prefab(nodes, codecs).instantiate(scene), invalids[i].second.c_str(),
                             std::invalid_argument);
        CHECK(scene.size() == 1u);
        CHECK(audio.voice_count() == 1u); // The first restored source released its voice.
    }
    nodes[1].components[0].state = valid;
    unsigned resolutions = 0;
    ComponentCodecs failing;
    add_audio_component_codecs(
        failing, audio, [](const auto &) { return "tone"; },
        [&](std::string_view) -> std::shared_ptr<const AudioClip> {
            if (++resolutions == 2)
                throw std::runtime_error("Synthetic resolver failure");
            return tone;
        });
    CHECK_THROWS_WITH_AS(Prefab(nodes, failing).instantiate(scene), "Synthetic resolver failure", std::runtime_error);
    CHECK(resolutions == 2u);
    CHECK(scene.size() == 1u); // The source restored before the failure was rolled back.
    CHECK(audio.voice_count() == 1u);
    auto temporary = prefab.instantiate(scene);
    CHECK(audio.voice_count() == 2u);
    CHECK(temporary.remove_component<AudioSource>());
    CHECK(audio.voice_count() == 1u);
    temporary.add_component<AudioSource>(audio, tone);
    temporary.destroy();
    existing.destroy();
    {
        Scene owned;
        owned.create().add_component<AudioSource>(audio, tone);
        owned.create().add_component<AudioSource>(audio, tone);
        CHECK(audio.voice_count() == 2u);
    }
    CHECK(audio.voice_count() == 0u);
}

TEST_CASE("Clip keys must be well-formed UTF-8 without NUL when a source is captured") {
    Audio audio(8000);
    auto tone = clip();
    std::string name;
    std::string resolved;
    ComponentCodecs codecs;
    add_audio_component_codecs(
        codecs, audio, [&](const auto &) { return name; },
        [&](std::string_view key) {
            resolved = key;
            return tone;
        });
    Scene scene;
    auto object = scene.create();
    object.add_component<AudioSource>(audio, tone);
    // A stray continuation byte, an overlong NUL, a surrogate, a code point past U+10FFFF, a truncated
    // sequence, an embedded NUL and an oversized key.
    const std::array<std::string, 7> invalids{"\xff",
                                              "\xc0\x80",
                                              "\xed\xa0\x80",
                                              "\xf4\x90\x80\x80",
                                              "\xe2\x82",
                                              std::string("a\0b", 3),
                                              std::string(4097, 'k')};
    for (std::size_t i = 0; i < invalids.size(); ++i) {
        CAPTURE(i);
        name = invalids[i];
        CHECK_THROWS_WITH_AS(Prefab::capture(object, codecs), "Invalid audio clip key", std::invalid_argument);
    }
    // Two-, three- and four-byte sequences at the edges of their ranges round-trip unchanged.
    name = "\xc2\x80\xed\x9f\xbf\xee\x80\x80\xf4\x8f\xbf\xbf" + std::string(4084, 'k');
    REQUIRE(name.size() == 4096u);
    const auto document = Prefab::capture(object, codecs).serialize({});
    (void)Prefab::deserialize(document, {}, codecs).instantiate(scene);
    CHECK(resolved == name);
}

TEST_CASE("Components and codecs can outlive their Audio") {
    const auto tone = clip();
    ComponentCodecs orphan_codecs;
    Scene orphan_scene;
    {
        Audio short_lived;
        orphan_codecs = codecs_for(short_lived, tone);
        orphan_scene.create().add_component<AudioSource>(short_lived, tone);
    }
    auto orphan = Prefab::capture(orphan_scene.roots()[0], orphan_codecs).instantiate(orphan_scene);
    CHECK(orphan.get_component<AudioSource>()->clip() == tone);
}

TEST_CASE("Invalid listener payloads are rejected without leaking objects") {
    Audio audio;
    auto codecs = codecs_for(audio, clip());
    Scene scene;
    auto listener = scene.create();
    listener.add_component<AudioListener>();
    auto nodes = std::vector<Prefab::Node>{Prefab::capture(listener, codecs).nodes()[0]};
    const std::array<std::pair<std::string_view, const char *>, 3> invalids{{
        {"[]", "JSON document requires an object"},
        {R"({"unexpected":1})", "Unknown JSON field: unexpected"},
        {R"({"a":0,"a":1})", duplicate_field},
    }};
    for (const auto &invalid : invalids) {
        CAPTURE(invalid.first);
        nodes[0].components[0].state = invalid.first;
        const auto before = scene.size();
        CHECK_THROWS_WITH_AS(Prefab(nodes, codecs).instantiate(scene), invalid.second, std::invalid_argument);
        CHECK(scene.size() == before);
    }
}

// A bus of another engine is rejected when the codecs are registered; accepting it made every later
// restore throw from Audio::sound.
TEST_CASE("Audio codecs reject a bus of another engine") {
    Audio audio(8000);
    Audio other(8000);
    const auto foreign = other.bus();
    CHECK_FALSE(audio.owns(foreign));
    CHECK(other.owns(foreign));
    CHECK(audio.owns(AudioBus{}));
    CHECK_THROWS_WITH_AS(codecs_for(audio, clip(), foreign), "Audio codecs require a bus of the same mixer",
                         std::invalid_argument);
    CHECK_NOTHROW(codecs_for(audio, clip(), audio.bus()));
}
