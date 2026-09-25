#pragma once
#ifdef CONSUMER_ASSETS
#include <anima/audio_scene.hpp>
#include <anima/prefab.hpp>
#include <array>
#include <stdexcept>
inline void consume_audio_scene() {
    anima::Audio mixer(8000, 2);
    const auto clip = anima::AudioClip::pcm({.25F, .25F}, 1, 8000);
    anima::ComponentCodecs codecs;
    anima::add_audio_component_codecs(
        codecs, mixer, [clip](const auto &resource) { return resource == clip ? "test-tone" : ""; },
        [clip](std::string_view key) { return key == "test-tone" ? clip : nullptr; });
    anima::Scene scene;
    auto group = scene.create("sound assembly");
    group.add_component<anima::AudioListener>();
    auto object = scene.create("emitter");
    object.set_parent(group, anima::ReparentMode::keep_local);
    object.set_local_position({1, 0, 0});
    anima::AudioSourceSettings settings;
    settings.spatial = settings.looping = settings.play_on_start = true;
    object.add_component<anima::AudioSource>(mixer, clip, settings);
    const auto document = anima::Prefab::capture(group, codecs).serialize({});
    group.destroy();
    auto loaded = anima::Prefab::deserialize(document, {}, codecs).instantiate(scene);
    anima::synchronize_audio(scene, mixer);
    std::array<float, 2> samples{};
    mixer.render(samples);
    if (samples[0] != 0 || samples[1] != .25F)
        throw std::runtime_error("Independent scene audio spatial playback failed");
    auto source = loaded.children()[0].get_component<anima::AudioSource>();
    loaded.set_active(false);
    anima::synchronize_audio(scene, mixer);
    mixer.render(samples);
    if (source->playing() || !source.enabled() || samples[0] != 0 || samples[1] != 0)
        throw std::runtime_error("Inactive audio hierarchy retained playback");
    loaded.set_active(true);
    anima::synchronize_audio(scene, mixer);
    mixer.render(samples);
    if (!source->playing() || samples[1] != .25F)
        throw std::runtime_error("Reactivated audio hierarchy did not resume");
    source.set_enabled(false);
    anima::synchronize_audio(scene, mixer);
    mixer.render(samples);
    if (samples[0] != 0 || samples[1] != 0)
        throw std::runtime_error("Independent scene audio disablement failed");
    loaded.destroy();
    auto voice1 = mixer.sound(clip), voice2 = mixer.sound(clip);
    if (!mixer.owns(voice1) || !mixer.owns(voice2))
        throw std::runtime_error("Independent scene audio lifetime failed");
}
#endif
