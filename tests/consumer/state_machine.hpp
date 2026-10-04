#pragma once
// An application's own authored state machine, run on its own synthetic model through the public API.
#include "rejection.hpp"
#include <anima/animation_state_machine.hpp>
#include <anima/prefab.hpp>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace state_machine_consumer {
using rejection::rejects;
inline void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
inline bool close_to(float actual, float expected) { return std::abs(actual - expected) < 1e-5F; }
// A base node and a tip above it. Rest holds still, sway moves the base 1 unit along +X over 1 s, dash 3 units over
// 0.5 s, and leap raises it 4 units along +Y for 0.5 s.
inline std::shared_ptr<const anima::Asset> puppet() {
    auto asset = std::make_shared<anima::Asset>();
    asset->nodes.resize(2);
    asset->nodes[0].name = "base";
    asset->nodes[1].name = "tip";
    asset->nodes[1].parent = 0;
    asset->nodes[1].rest.translation = {0, 1, 0};
    anima::Material surface;
    surface.name = "surface";
    asset->materials.push_back(surface);
    anima::SourcePrimitive primitive;
    primitive.node = 1;
    primitive.material = 0;
    for (const auto corner : {anima::Vec3{0, 0, 0}, anima::Vec3{1, 0, 0}, anima::Vec3{0, 1, 0}}) {
        anima::SourceVertex vertex;
        vertex.position = corner;
        vertex.normal = {0, 0, 1};
        primitive.vertices.push_back(vertex);
    }
    asset->primitives.push_back(primitive);
    const auto add = [&](const char *name, double duration, anima::Vec3 from, anima::Vec3 to) {
        anima::Animation clip;
        clip.name = name;
        clip.duration = duration;
        clip.channels.push_back({0,
                                 anima::ChannelPath::translation,
                                 anima::Interpolation::linear,
                                 {0, duration},
                                 {{from.x, from.y, from.z, 0}, {to.x, to.y, to.z, 0}}});
        asset->animations.push_back(std::move(clip));
    };
    add("rest", 1, {}, {});
    add("sway", 1, {}, {1, 0, 0});
    add("dash", .5, {}, {3, 0, 0});
    add("leap", .5, {0, 4, 0}, {0, 4, 0});
    return asset;
}
inline constexpr std::string_view puppet_machine = R"({
  "version": 1,
  "kind": "anima.animation-state-machine",
  "parameters": [{"name": "pace", "type": "float"}, {"name": "hop", "type": "trigger"}],
  "states": [
    {"name": "ground", "blend": {"parameter": "pace", "clips": [
      {"clip": "rest", "threshold": 0}, {"clip": "sway", "threshold": 1}, {"clip": "dash", "threshold": 2}]}},
    {"name": "air", "clip": "leap"}
  ],
  "transitions": [
    {"from": "ground", "to": "air", "conditions": [{"parameter": "hop", "mode": "is_true"}], "duration": 0.25},
    {"from": "air", "to": "ground", "exit_time": 1, "duration": 0.25, "interruption": "destination"}
  ]
})";

inline void run() {
    using namespace anima;
    const auto asset = puppet();
    const std::vector<ClipMetadata> clips{{"rest", true, {}, {}},
                                          {"sway", true, {{.75, "plant"}}, {}},
                                          {"dash", true, {}, {}},
                                          {"leap", false, {{.25, "apex"}}, {}}};
    const auto machine =
        std::make_shared<const AnimationStateMachine>(AnimationStateMachine::deserialize(asset, clips, puppet_machine));
    require(machine->definition().states.size() == 2 && machine->source() == asset,
            "State machine document lost its states or source");

    Scene scene;
    auto puppet_object = scene.create("puppet", Mesh::compile(*asset));
    auto animator = puppet_object.add_component<StateMachineAnimator>(machine);
    const auto base = [&] {
        const auto &world = scene.instance(puppet_object.id()).palette.at(0);
        return Vec3{world[12], world[13], world[14]};
    };
    animator->set_float("pace", 1);
    scene.update(.25);
    require(animator->state() == "ground" && close_to(base().x, .25F),
            "Blend state did not play its clip at its threshold");

    // A trigger starts a crossfade that blends the grounded and airborne poses.
    animator->set_trigger("hop");
    scene.update(.125);
    const auto fade = animator->crossfade();
    require(animator->state() == "air" && fade && fade->source == "ground" && fade->weight == .5F &&
                !animator->get_bool("hop"),
            "Trigger did not start a crossfade");
    require(close_to(base().x, .1875F) && close_to(base().y, 2), "Crossfade did not blend the two poses");
    scene.update(.125);
    require(!animator->crossfade() && close_to(base().y, 4), "Crossfade did not end at its duration");
    require(animator->events().size() == 1 && animator->events()[0].event.name == "apex" &&
                animator->events()[0].offset == .125 && animator->events()[0].weight == 1,
            "State machine did not report its clip event");

    // The exit time at the end of the leap starts the way back in the next update.
    scene.update(.25);
    require(animator->state() == "air", "Exit time started before it was crossed");
    scene.update(.125);
    require(animator->state() == "ground" && animator->crossfade() && animator->crossfade()->source == "air" &&
                close_to(base().y, 2) && close_to(base().x, .0625F),
            "Exit time did not start its crossfade");

    // A scene document keeps the machine key, the parameters and the state entered.
    ComponentCodecs codecs;
    add_state_machine_animator_codec(
        codecs,
        [&](const std::shared_ptr<const AnimationStateMachine> &named) {
            return named == machine ? std::string("puppet.states") : std::string();
        },
        [&](std::string_view key) { return key == "puppet.states" ? machine : nullptr; });
    const auto mesh = puppet_object.renderer().mesh();
    const auto document =
        serialize_scene(scene, [](const std::shared_ptr<const Mesh> &) { return std::string("puppet.mesh"); }, codecs);
    const auto loaded = load_scene(document, [&](std::string_view) { return mesh; }, codecs);
    const auto restored = loaded->components<StateMachineAnimator>();
    require(restored.size() == 1 && restored[0]->machine() == machine && restored[0]->state() == "ground" &&
                restored[0]->time() == animator->time() && restored[0]->get_float("pace") == 1 &&
                !restored[0]->crossfade(),
            "State machine animator did not persist its key, parameters and state");

    rejects<std::invalid_argument>(
        [&] {
            (void)AnimationStateMachine::deserialize(asset, clips,
                                                     R"({"version": 2, "kind": "anima.animation-state-machine"})");
        },
        "Unsupported animation state machine document version");
    AnimationStateMachine::Definition unknown_clip = machine->definition();
    unknown_clip.states[1].clip = "glide";
    rejects<std::invalid_argument>([&] { (void)AnimationStateMachine(asset, clips, unknown_clip); },
                                   "Animation clip has no metadata: glide");
    rejects<std::out_of_range>([&] { animator->set_float("speed", 1); }, "Unknown animation parameter: speed");
    rejects<std::invalid_argument>([&] { animator->set_bool("pace", true); },
                                   "Animation parameter has another type: pace");
    rejects<std::out_of_range>([&] { animator->play("sky"); }, "Unknown animation state: sky");
    rejects<std::invalid_argument>([&] { (void)animator->update(-1); }, "Invalid StateMachineAnimator time step");
    rejects<std::invalid_argument>(
        [&] { (void)StateMachineAnimator(ComponentOwner{scene.create("other", mesh)}, nullptr); },
        "StateMachineAnimator requires a state machine");
}
} // namespace state_machine_consumer
