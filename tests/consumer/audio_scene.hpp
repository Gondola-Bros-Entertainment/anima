#pragma once
#ifdef CONSUMER_ASSETS
#include <anima/audio_scene.hpp>
#include <anima/prefab.hpp>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string_view>
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
    // The last frame of a block, past the one-frame delay of a voice that starts in it.
    const auto last_frame = [&] {
        std::array<float, 2 * anima::audio_block_frames> output{};
        mixer.render(output);
        return std::array{output[output.size() - 2], output.back()};
    };
    anima::synchronize_audio(scene, mixer);
    // At the minimum distance on the listener's right: full gain there, and the 0.2 floor on the left.
    auto samples = last_frame();
    if (std::abs(samples[0] - .05F) > 1e-6F || std::abs(samples[1] - .25F) > 1e-6F)
        throw std::runtime_error("Independent scene audio spatial playback failed");
    auto source = loaded.children()[0].get_component<anima::AudioSource>();
    loaded.set_active(false);
    anima::synchronize_audio(scene, mixer);
    samples = last_frame();
    if (source->playing() || !source.enabled() || samples[0] != 0 || samples[1] != 0)
        throw std::runtime_error("Inactive audio hierarchy retained playback");
    loaded.set_active(true);
    anima::synchronize_audio(scene, mixer);
    samples = last_frame();
    if (!source->playing() || std::abs(samples[1] - .25F) > 1e-6F)
        throw std::runtime_error("Reactivated audio hierarchy did not resume");
    source.set_enabled(false);
    anima::synchronize_audio(scene, mixer);
    samples = last_frame();
    if (samples[0] != 0 || samples[1] != 0)
        throw std::runtime_error("Independent scene audio disablement failed");
    loaded.destroy();
    if (mixer.voice_count() != 0)
        throw std::runtime_error("Independent scene audio lifetime failed");
}
#endif
