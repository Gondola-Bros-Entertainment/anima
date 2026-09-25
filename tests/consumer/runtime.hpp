#pragma once
#include <anima/audio_scene.hpp>
#include <anima/physics2d_scene.hpp>
#include <anima/physics_scene.hpp>
#include <anima/scene_set.hpp>
#include <array>
#include <cmath>
#include <stdexcept>

namespace runtime_consumer {
using namespace anima;
inline void check(bool value, const char *reason) {
    if (!value)
        throw std::runtime_error(reason);
}
template <class F> void rejects(F call) {
    try {
        call();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error("Invalid multi-scene operation accepted");
}
struct Tick {
    int *ticks;
    void on_fixed_update(double) { ++*ticks; }
};
struct Reentry {
    SceneSet *scenes;
    physics::World *volume;
    physics2d::World *plane;
    Audio *audio;
    void on_fixed_update(double seconds) {
        rejects([&] { physics::step(*scenes, *volume, seconds); });
        rejects([&] { physics2d::step(*scenes, *plane, seconds); });
        rejects([&] { synchronize_audio(*scenes, *audio); });
    }
};
inline void run() {
    constexpr double tick = 1. / 60;
    constexpr float sample_value = .25F;
    physics::World volume, volume_reference;
    physics2d::World plane, plane_reference;
    Audio audio(8000);
    SceneSet scenes;
    auto persistent = scenes.create("persistent"), level = scenes.create("level");
    int ticks = 0;
    auto listener = persistent->create("listener");
    listener.add_component<AudioListener>();
    listener.add_component<Tick>(&ticks);
    listener.add_component<Reentry>(&scenes, &volume, &plane, &audio);
    auto marker = level->create("marker");
    marker.add_component<Tick>(&ticks);
    auto ground3 = persistent->create("floor3");
    auto ground2 = persistent->create("floor2");
    ground3.set_position({0, -1, 0});
    ground2.set_position({0, -1, 0});
    auto stationary3 = ground3.add_component<physics::RigidBody>(volume)->body();
    auto stationary2 = ground2.add_component<physics2d::RigidBody>(plane)->body();
    physics::BodySettings settings3;
    settings3.motion = physics::Motion::dynamic;
    settings3.pose.position = {0, 3, 0};
    physics2d::BodySettings settings2;
    settings2.motion = physics2d::Motion::dynamic;
    settings2.pose.position = {0, 3};
    auto object3 = level->create("falling3"), object2 = level->create("falling2");
    object3.set_position({0, 3, 0});
    object2.set_position({0, 3, 7});
    auto body3 = object3.add_component<physics::RigidBody>(volume, settings3)->body();
    auto body2 = object2.add_component<physics2d::RigidBody>(plane, settings2)->body();
    const auto reference3 = volume_reference.create(settings3);
    const auto reference2 = plane_reference.create(settings2);
    scenes.fixed_update(tick);
    physics::step(scenes, volume, tick);
    physics2d::step(scenes, plane, tick);
    volume_reference.step(tick);
    plane_reference.step(tick);
    check(ticks == 2, "Physics driver repeated component ticks");
    check(std::abs(object3.position().y - reference3.pose().position.y) < 1e-6F &&
              std::abs(object2.position().y - reference2.pose().position.y) < 1e-6F && object2.position().z == 7,
          "A shared world advanced more than once or failed to publish poses");

    // A bad later scene must not move/disable earlier bodies or advance the world.
    const auto before3 = body3.pose();
    const auto before2 = body2.pose();
    ground3.set_position({10, -1, 0});
    ground2.set_position({10, -1, 0});
    object3.set_position({2e6F, 3, 0});
    object2.set_position({2e6F, 3, 7});
    rejects([&] { physics::step(scenes, volume, tick); });
    rejects([&] { physics2d::step(scenes, plane, tick); });
    check(stationary3.pose().position.x == 0 && stationary2.pose().position.x == 0 &&
              body3.pose().position.y == before3.position.y && body2.pose().position.y == before2.position.y,
          "Invalid later scene partially changed a shared world");
    object3.set_position(before3.position);
    object2.set_position({before2.position.x, before2.position.y, 7});
    ground3.set_position({0, -1, 0});
    ground2.set_position({0, -1, 0});

    auto emitter = level->create("sound");
    emitter.set_position({2, 0, 0});
    auto clip = AudioClip::pcm(std::vector<float>(16, sample_value), 1, 8000);
    AudioSourceSettings sound;
    sound.spatial = sound.looping = sound.play_on_start = true;
    sound.minimum_distance = 0;
    sound.maximum_distance = 4;
    auto source = emitter.add_component<AudioSource>(audio, clip, sound);
    synchronize_audio(scenes, audio);
    check(source->playing() && source->cursor() == 0, "Audio synchronization advanced time or missed playback");
    std::array<float, 2> samples{};
    audio.render(samples);
    check(std::abs(samples[0]) < 1e-6F && std::abs(samples[1] - sample_value / 2) < 1e-6F,
          "Cross-scene listener/source spatialization failed");
    auto duplicate = marker.add_component<AudioListener>();
    listener.set_position({100, 0, 0});
    emitter.set_active(false);
    rejects([&] { synchronize_audio(scenes, audio); });
    check(source->playing(), "Invalid listener selection partially paused sources");
    audio.render(samples);
    check(std::abs(samples[1] - sample_value / 2) < 1e-6F, "Rejected audio snapshot changed listener");
    duplicate.set_enabled(false);
    listener.set_position({0, 0, 0});
    synchronize_audio(scenes, audio);
    check(!source->playing(), "Inactive source in another scene did not pause");
    emitter.set_active(true);
    synchronize_audio(scenes, audio);
    check(source->playing(), "Source did not resume after cross-scene synchronization");

    // Foreign bindings reject even when disabled, before earlier state changes.
    auto foreign3 = marker.add_component<physics::RigidBody>(volume_reference);
    auto foreign2 = marker.add_component<physics2d::RigidBody>(plane_reference);
    foreign3.set_enabled(false);
    foreign2.set_enabled(false);
    ground3.set_active(false);
    ground2.set_active(false);
    rejects([&] { physics::step(scenes, volume, tick); });
    rejects([&] { physics2d::step(scenes, plane, tick); });
    check(stationary3.enabled() && stationary2.enabled(), "Foreign later binding partially disabled earlier bodies");
    marker.remove_component<physics::RigidBody>();
    marker.remove_component<physics2d::RigidBody>();
    ground3.set_active(true);
    ground2.set_active(true);
    Audio other_audio(8000);
    auto foreign_source = marker.add_component<AudioSource>(other_audio, clip);
    foreign_source.set_enabled(false);
    emitter.set_active(false);
    rejects([&] { synchronize_audio(scenes, audio); });
    check(source->playing(), "Foreign later source partially changed an earlier source");
    marker.remove_component<AudioSource>();
    emitter.set_active(true);

    // Cross-scene collision and teardown use the same production driver.
    for (int i = 0; i < 180; ++i) {
        scenes.fixed_update(tick);
        physics::step(scenes, volume, tick);
        physics2d::step(scenes, plane, tick);
    }
    check(std::abs(object3.position().y) < .04F && std::abs(object2.position().y) < .04F,
          "Bodies did not collide with the other scene's floor");
    const auto views = scenes.render_scenes();
    rejects([&] { (void)scenes.replace(level, "{}", {}); });
    check(level && body3.valid() && body2.valid() && source->playing(), "Failed transition changed live systems");
    ComponentCodecs codecs;
    physics::add_component_codec(codecs, volume);
    physics2d::add_component_codec(codecs, plane);
    add_audio_component_codecs(
        codecs, audio, [](const auto &) { return "tone"; },
        [&](std::string_view name) { return name == "tone" ? clip : nullptr; });
    codecs.add<Tick>(
        "test.tick.v1", [](const Tick &, const ObjectReferences &) { return "{}"; },
        [&](GameObject object, std::string_view state, const ObjectReferences &) {
            check(state == "{}", "Invalid consumer tick state");
            object.add_component<Tick>(&ticks);
        });
    const auto document = serialize_scene(level.get(), {}, codecs);
    for (int transition = 0; transition < 3; ++transition) {
        auto old = level;
        level = scenes.replace(level, document, {}, codecs);
        scenes.fixed_update(tick);
        physics::step(scenes, volume, tick);
        physics2d::step(scenes, plane, tick);
        synchronize_audio(scenes, audio);
        check(!old && volume.size() == 2 && plane.size() == 2 && level->components<AudioSource>().front()->playing(),
              "Repeated replacement lost system bindings or leaked resources");
    }
    check(!body3.valid() && !body2.valid() && !source && views[1]->size() == 0 && views[0]->size() != 0,
          "Replacement rebound stale handles or affected the persistent scene");
    scenes.unload(level);
    check(volume.size() == 1 && plane.size() == 1, "Unloading a scene retained bodies");
    synchronize_audio(scenes, audio);
    audio.render(samples);
    check(samples[0] == 0 && samples[1] == 0, "Unloaded scene still produced audio");
    scenes.clear();
    physics::step(scenes, volume, tick);
    physics2d::step(scenes, plane, tick);
    synchronize_audio(scenes, audio);
    check(volume.size() == 0 && plane.size() == 0, "Empty set retained bodies");
}
} // namespace runtime_consumer
