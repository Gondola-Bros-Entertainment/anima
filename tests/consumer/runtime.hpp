#pragma once
#include <anima/audio_scene.hpp>
#include <anima/core/fixed_step.hpp>
#include <anima/input_scene.hpp>
#include <anima/navigation_scene.hpp>
#include <anima/physics2d_scene.hpp>
#include <anima/physics_scene.hpp>
#include <anima/scene_set.hpp>
#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

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
        rejects([&] { input::begin_frame(*scenes); });
        rejects([&] { input::dispatch(*scenes, {input::EventType::control, {input::ControlKind::key, 44, 0}, 1}); });
        rejects([&] { navigation::update_agents(*scenes, seconds); });
        rejects([&] { physics::step(*scenes, *volume, seconds); });
        rejects([&] { physics2d::step(*scenes, *plane, seconds); });
        rejects([&] { synchronize_audio(*scenes, *audio); });
    }
};
struct FrameCounts {
    bool pending_press{};
    unsigned presses{}, fixed{}, frame{}, late{};
};
struct FrameProbe {
    ComponentRef<navigation::Agent> agent;
    GameObject emitter;
    FrameCounts *counts;
    void on_fixed_update(double) {
        check(agent->follower().next() == 1 && agent->desired_velocity().x == 2,
              "Fixed hook ran before navigation intent was available");
        if (std::exchange(counts->pending_press, false))
            ++counts->presses;
        ++counts->fixed;
    }
    void on_update(double) {
        ++counts->frame;
        emitter.set_position({1, 0, 0});
    }
    void on_late_update(double) {
        check(counts->frame == counts->late + 1, "Late hook did not follow the frame hook");
        ++counts->late;
        emitter.set_position({2, 0, 0});
    }
};
inline void frame_cadence() {
    using namespace std::chrono_literals;
    physics::World volume, reference_volume;
    physics2d::World plane, reference_plane;
    Audio audio(8000);
    FrameCounts counts;
    SceneSet scenes;
    auto persistent = scenes.create("persistent"), level = scenes.create("level");
    auto listener = persistent->create(), steering = persistent->create(), emitter = level->create();
    listener.add_component<AudioListener>();
    auto actions = steering.add_component<input::ActionInput>(
        input::Map{{"advance", input::ActionType::button, {{{input::ControlKind::key, 44}}}}});
    auto agent = steering.add_component<navigation::Agent>(std::vector<Vec3>{{0, 0, 0}, {10, 0, 0}}, 2.F, 0.F);
    steering.add_component<FrameProbe>(agent, emitter, &counts);
    physics::BodySettings settings3;
    settings3.motion = physics::Motion::dynamic;
    settings3.pose.position = {0, 3, 0};
    physics2d::BodySettings settings2;
    settings2.motion = physics2d::Motion::dynamic;
    settings2.pose.position = {0, 3};
    auto object3 = persistent->create(), object2 = level->create();
    object3.set_position({0, 3, 0});
    object2.set_position({0, 3, 0});
    object3.add_component<physics::RigidBody>(volume, settings3);
    object2.add_component<physics2d::RigidBody>(plane, settings2);
    auto reference3 = reference_volume.create(settings3);
    auto reference2 = reference_plane.create(settings2);
    AudioSourceSettings sound;
    sound.spatial = sound.looping = sound.play_on_start = true;
    sound.minimum_distance = 0;
    sound.maximum_distance = 4;
    auto source =
        emitter.add_component<AudioSource>(audio, AudioClip::pcm(std::vector<float>(16, .25F), 1, 8000), sound);
    FixedStepClock clock(10ms, 4);
    const auto advance = [&](std::chrono::nanoseconds elapsed, std::span<const input::Event> events) {
        input::begin_frame(scenes);
        for (const auto &event : events)
            input::dispatch(scenes, event);
        // Application policy: retain a press across zero-tick frames and consume
        // it once at the first fixed tick, even when a frame catches up several.
        counts.pending_press = counts.pending_press || actions->context().state("advance").pressed;
        const auto batch = clock.advance(elapsed);
        const double fixed_seconds = std::chrono::duration<double>(clock.step()).count();
        for (std::uint32_t step = 0; step < batch.steps; ++step) {
            navigation::update_agents(scenes, fixed_seconds);
            scenes.fixed_update(fixed_seconds);
            physics::step(scenes, volume, fixed_seconds);
            physics2d::step(scenes, plane, fixed_seconds);
            reference_volume.step(fixed_seconds);
            reference_plane.step(fixed_seconds);
        }
        scenes.update(std::chrono::duration<double>(elapsed).count());
        const auto cursor = source->cursor();
        synchronize_audio(scenes, audio);
        check(source->cursor() == cursor, "Audio synchronization advanced mixer time");
        std::array<float, 2> samples{};
        audio.render(samples);
        check(std::abs(samples[0]) < 1e-6F && std::abs(samples[1] - .125F) < 1e-6F,
              "Audio synchronization missed the late-update emitter pose");
        check(std::abs(object3.position().y - reference3.pose().position.y) < 1e-6F &&
                  std::abs(object2.position().y - reference2.pose().position.y) < 1e-6F,
              "An integrated physics world did not advance exactly once per fixed tick");
        return batch.steps;
    };
    const input::Event press{input::EventType::control, {input::ControlKind::key, 44, 0}, 1};
    const input::Event release{input::EventType::control, {input::ControlKind::key, 44, 0}, 0};
    const std::array pressed{press};
    const std::array tapped{release, press};
    check(advance(5ms, pressed) == 0 && counts.pending_press && counts.presses == 0 && counts.fixed == 0 &&
              counts.frame == 1 && counts.late == 1,
          "Zero-tick frame consumed input or skipped frame presentation");
    check(advance(25ms, {}) == 3 && counts.presses == 1 && counts.fixed == 3 && !counts.pending_press &&
              !actions->context().state("advance").pressed && actions->context().state("advance").active,
          "Catch-up ticks lost a queued press or consumed one edge repeatedly");
    check(advance(20ms, tapped) == 2 && counts.presses == 2 && counts.fixed == 5 && counts.frame == 3 &&
              counts.late == 3 && steering.position().x == 0,
          "Later input edge, fixed cadence or application-owned navigation movement failed");
}
struct PrefabFinish {};
inline void prefab_destinations() {
    physics::World destination_volume;
    physics2d::World destination_plane;
    Audio destination_audio(8000, 1);
    const auto clip = AudioClip::pcm(std::vector<float>(16, .25F), 1, 8000);
    const auto make_codecs = [&](physics::World &volume, physics2d::World &plane, Audio &audio, bool fail = false) {
        ComponentCodecs codecs;
        physics::add_component_codec(codecs, volume);
        physics2d::add_component_codec(codecs, plane);
        add_audio_component_codecs(
            codecs, audio, [clip](const auto &resource) { return resource == clip ? "tone" : ""; },
            [clip](std::string_view name) { return name == "tone" ? clip : nullptr; });
        codecs.add<PrefabFinish>(
            "test.prefab-finish.v1", [](const PrefabFinish &, const ObjectReferences &) { return "{}"; },
            [fail](GameObject object, std::string_view, const ObjectReferences &) {
                object.add_component<PrefabFinish>();
                if (fail)
                    throw std::runtime_error("Destination prefab decoder failed");
            });
        return codecs;
    };
    const auto destination_codecs = make_codecs(destination_volume, destination_plane, destination_audio);
    const auto prefab = [&] {
        physics::World source_volume;
        physics2d::World source_plane;
        Audio source_audio(8000);
        Scene authored, captured, transferred;
        auto root = authored.create(), child = authored.create();
        child.set_parent(root);
        root.add_component<physics::RigidBody>(source_volume);
        child.add_component<physics2d::RigidBody>(source_plane);
        AudioSourceSettings sound;
        sound.looping = sound.play_on_start = true;
        child.add_component<AudioSource>(source_audio, clip, sound);
        child.add_component<PrefabFinish>();
        const auto asset = Prefab::capture(root, make_codecs(source_volume, source_plane, source_audio));
        const auto original = asset.instantiate(captured);
        check(source_volume.owns(original.get_component<physics::RigidBody>()->body()) &&
                  source_plane.owns(original.children().front().get_component<physics2d::RigidBody>()->body()),
              "Default prefab instantiation lost captured physics bindings");
        synchronize_audio(captured, source_audio);
        const auto relocated = asset.instantiate(transferred, identity(), destination_codecs);
        check(destination_volume.owns(relocated.get_component<physics::RigidBody>()->body()) &&
                  destination_plane.owns(relocated.children().front().get_component<physics2d::RigidBody>()->body()) &&
                  source_volume.size() == 2 && source_plane.size() == 2,
              "Destination prefab instance used the original physics worlds");
        synchronize_audio(transferred, destination_audio);
        check(relocated.children().front().get_component<AudioSource>()->playing(),
              "Destination prefab voice did not bind to the selected mixer");
        rejects([&] { synchronize_audio(transferred, source_audio); });
        const auto retained = asset.instantiate(captured);
        check(source_volume.owns(retained.get_component<physics::RigidBody>()->body()) && source_volume.size() == 3 &&
                  source_plane.size() == 3,
              "Destination override replaced the prefab's retained registry");
        synchronize_audio(captured, source_audio);
        return asset;
    }();
    check(destination_volume.size() == 0 && destination_plane.size() == 0,
          "Temporary destination instances leaked physics resources");
    Scene destination;
    const auto existing = destination.create();
    rejects([&] { (void)prefab.instantiate(destination); });
    check(destination.size() == 1, "Expired captured physics bindings leaked a staged instance");
    auto restored = prefab.instantiate(destination, identity(), destination_codecs);
    synchronize_audio(destination, destination_audio);
    check(destination_volume.size() == 1 && destination_plane.size() == 1 &&
              restored.children().front().get_component<AudioSource>()->clip() == clip,
          "Destination override depended on the expired original session");
    restored.destroy();
    const auto failing = make_codecs(destination_volume, destination_plane, destination_audio, true);
    rejects([&] { (void)prefab.instantiate(destination, identity(), failing); });
    std::array<float, 2> samples{};
    destination_audio.render(samples);
    const auto available_voice = destination_audio.sound(clip);
    check(destination.size() == 1 && existing.valid() && destination_volume.size() == 0 &&
              destination_plane.size() == 0 && samples[0] == 0 && samples[1] == 0 &&
              destination_audio.owns(available_voice),
          "Failed destination decoder leaked bodies, voices or staged objects");
}
inline void run() {
    frame_cadence();
    prefab_destinations();
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
