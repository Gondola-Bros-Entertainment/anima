#include "component_payloads.hpp"
#include <anima/input_scene.hpp>
#include <anima/prefab.hpp>
#include <anima/scene_set.hpp>
#include <iostream>
namespace i = anima::input;
using namespace anima;
namespace {
void check(bool v, const char *m) {
    if (!v)
        throw std::runtime_error(m);
}
template <class F> void rejects(F f) {
    bool caught = false;
    try {
        f();
    } catch (const std::exception &) {
        caught = true;
    }
    check(caught, "Expected input scene rejection");
}
constexpr i::Event down{i::EventType::control, {i::ControlKind::key, 4, 0}, 1};
constexpr i::Event up{i::EventType::control, {i::ControlKind::key, 4, 0}, 0};
i::Event key_event(std::uint16_t code, std::uint32_t device, float value = 1) {
    return {i::EventType::control, {i::ControlKind::key, code, device}, value};
}
i::Map chord_map(std::uint32_t device = i::any_device) {
    i::Binding binding{{i::ControlKind::key, 4, device}};
    binding.modifiers = {{i::ControlKind::key, 224, device}};
    return {{"chord", i::ActionType::button, {binding}}};
}
void chord_configuration() {
    const auto map = chord_map(7);
    const auto document = i::serialize_map(map);
    const auto restored = i::deserialize_map(document);
    check(restored[0].bindings[0].control == map[0].bindings[0].control &&
              restored[0].bindings[0].modifiers == map[0].bindings[0].modifiers &&
              i::serialize_map(restored) == document,
          "Chord configuration lost selectors or modifiers on round-trip");
    check(i::deserialize_map(R"({"version":2,"actions":[]})").empty(), "Version 2 empty input map was rejected");
    for (const auto version : {"1", "3", "2.0", "-1", "true"}) {
        const auto invalid = std::string("{\"version\":") + version + ",\"actions\":[]}";
        rejects([&] { (void)i::deserialize_map(invalid); });
    }
    for (const auto &invalid : invalid_component_payloads(document, "version"))
        rejects([&] { (void)i::deserialize_map(invalid); });

    const std::string prefix =
        R"({"version":2,"actions":[{"name":"chord","type":0,"threshold":0.5,"bindings":[{"kind":0,"code":4,"device":7,"channel":0,"scale":1,"deadzone":0)";
    const std::string suffix = "}]}]}";
    const std::string modifier = R"({"kind":0,"code":224,"device":7})";
    const auto with_modifiers = [&](std::string_view value) {
        return prefix + ",\"modifiers\":" + std::string(value) + suffix;
    };
    check(i::deserialize_map(with_modifiers("[" + modifier + "]"))[0].bindings[0].modifiers.size() == 1,
          "Explicit current chord schema was rejected");
    rejects([&] { (void)i::deserialize_map(prefix + suffix); });
    rejects([&] { (void)i::deserialize_map(with_modifiers("null")); });
    rejects([&] { (void)i::deserialize_map(with_modifiers("{}")); });
    rejects([&] { (void)i::deserialize_map(with_modifiers("[" + modifier + "," + modifier + "]")); });
    rejects([&] {
        (void)i::deserialize_map(
            with_modifiers("[" + modifier + "," + modifier + "," + modifier + "," + modifier + "," + modifier + "]"));
    });
    for (const auto &invalid : invalid_component_payloads(modifier, "kind"))
        rejects([&] { (void)i::deserialize_map(with_modifiers("[" + invalid + "]")); });
    for (const auto invalid : {R"({"kind":3,"code":0,"device":7})", R"({"kind":0,"code":224,"device":8})",
                               R"({"kind":0,"code":4,"device":7})", R"({"kind":0,"code":512,"device":7})",
                               R"({"kind":0,"code":224,"device":4294967296})"})
        rejects([&] { (void)i::deserialize_map(with_modifiers("[" + std::string(invalid) + "]")); });
    rejects([&] { (void)i::deserialize_map(std::string(1024 * 1024, ' ') + document); });

    // A valid in-memory map can exceed the wire bound once all modifiers are encoded.
    i::Binding wide{{i::ControlKind::gamepad_axis, 15, UINT32_MAX - 1}, i::Channel::x, 16, .999999F};
    wide.modifiers = {{i::ControlKind::gamepad_button, 60, UINT32_MAX - 1},
                      {i::ControlKind::gamepad_button, 61, UINT32_MAX - 1},
                      {i::ControlKind::gamepad_button, 62, UINT32_MAX - 1},
                      {i::ControlKind::gamepad_button, 63, UINT32_MAX - 1}};
    i::Map large;
    for (unsigned action = 0; action < 128; ++action)
        large.push_back({"action" + std::to_string(action), i::ActionType::axis, std::vector<i::Binding>(32, wide)});
    i::validate(large);
    rejects([&] { (void)i::serialize_map(large); });
}
void chord_persistence() {
    Scene source;
    auto object = source.create("authored chord");
    auto input = object.add_component<i::ActionInput>(chord_map(7));
    input->context().process(key_event(4, 7));
    input->context().process(key_event(224, 7));
    check(input->context().state("chord").pressed, "Source chord did not activate");
    ComponentCodecs source_codecs;
    i::add_component_codec(source_codecs);
    const auto prefab = Prefab::capture(object, source_codecs);
    check(prefab.nodes()[0].components[0].type == "anima.action-input.v2", "Action input codec did not use version 2");
    input->context().set_focused(false);
    input->context().set_enabled(false);
    const auto scene_document = serialize_scene(source, {}, source_codecs);
    object.destroy();
    ComponentCodecs destination_codecs;
    i::add_component_codec(destination_codecs);
    Scene destination;
    rejects([&] { (void)prefab.instantiate(destination, identity(), {}); });
    check(destination.size() == 0, "Missing destination input codec leaked an object");
    auto instance = Prefab::deserialize(prefab.serialize({}), {}, destination_codecs)
                        .instantiate(destination, identity(), destination_codecs);
    auto loaded = load_scene(scene_document, {}, destination_codecs);
    for (auto component : {instance.get_component<i::ActionInput>(), loaded->components<i::ActionInput>().front()}) {
        auto &context = component->context();
        check(context.enabled() && context.focused() && !context.state("chord").active &&
                  !context.state("chord").pressed && context.actions()[0].bindings[0].modifiers[0].device == 7,
              "Chord persistence restored runtime state or lost authored device selection");
        context.process(key_event(4, 8));
        context.process(key_event(224, 8));
        context.process(key_event(4, 7));
        check(!context.state("chord").active, "Restored chord accepted a foreign modifier");
        context.process(key_event(224, 7));
        check(context.state("chord").pressed, "Restored chord failed to accept fresh matching events");
    }
    auto nodes = std::vector<Prefab::Node>(prefab.nodes().begin(), prefab.nodes().end());
    nodes[0].components[0].type = "anima.action-input.v1";
    rejects([&] { (void)Prefab(nodes, destination_codecs); });
    nodes[0] = prefab.nodes()[0];
    nodes.push_back(nodes[0]);
    nodes[1].parent = 0;
    nodes[1].key = {};
    const auto before = destination.size();
    for (const auto &payload : invalid_component_payloads(nodes[0].components[0].state, "version")) {
        nodes[1].components[0].state = payload;
        rejects([&] { (void)Prefab(nodes, destination_codecs).instantiate(destination); });
        check(destination.size() == before, "Late malformed chord component leaked staged objects");
    }
}
void chord_staging() {
    SceneSet scenes;
    auto first = scenes.create("first"), second = scenes.create("second");
    auto input = first->create().add_component<i::ActionInput>(chord_map());
    auto other = second->create().add_component<i::ActionInput>(chord_map());
    input->context().process(key_event(4, 5));
    for (unsigned device = 0; device < 1024; ++device)
        other->context().process(key_event(4, device));
    i::begin_frame(scenes);
    rejects([&] { i::dispatch(scenes, key_event(224, 5)); });
    for (auto component : {input, other})
        check(!component->context().state("chord").active && !component->context().state("chord").pressed,
              "Later modifier capacity failure partially published scene-set chord state");
    other->context().process(key_event(4, 0, 0));
    i::dispatch(scenes, key_event(4, 5));
    check(!input->context().state("chord").active && !other->context().state("chord").active,
          "Rejected scene-set modifier remained in observed physical state");
    i::dispatch(scenes, key_event(224, 5));
    check(input->context().state("chord").pressed && other->context().state("chord").pressed,
          "Fresh modifier failed after releasing observed capacity");
}
template <class F> bool rejects_driver(F f) noexcept {
    try {
        f();
    } catch (const std::logic_error &) {
        return true;
    } catch (...) {
    }
    return false;
}
bool rejects_set_drivers(SceneSet &scenes) noexcept {
    bool rejected = rejects_driver([&] { i::begin_frame(scenes); });
    rejected &= rejects_driver([&] { i::dispatch(scenes, up); });
    return rejected;
}
struct BoundaryChecks {
    bool rejected = true;
    unsigned calls{};
};
struct DriverProbe {
    Scene &scene;
    SceneSet &scenes;
    BoundaryChecks &checks;
    DriverProbe(Scene &owner, SceneSet &selection, BoundaryChecks &results)
        : scene(owner), scenes(selection), checks(results) {
        attempt();
    }
    void attempt() noexcept {
        ++checks.calls;
        checks.rejected &= rejects_driver([&] { i::begin_frame(scene); });
        checks.rejected &= rejects_driver([&] { i::dispatch(scene, up); });
        checks.rejected &= rejects_set_drivers(scenes);
    }
    void on_enable() noexcept { attempt(); }
    void on_disable() noexcept { attempt(); }
    void on_update(double) { attempt(); }
    void on_fixed_update(double) { attempt(); }
    void on_late_update(double) { attempt(); }
};
void run_scene_set() {
    const i::Map map{{"activate", i::ActionType::button, {{{i::ControlKind::key, 4}}}}};
    SceneSet scenes;
    auto first = scenes.create("first"), second = scenes.create("second");
    auto object = first->create(), parent = second->create(), child = second->create();
    child.set_parent(parent);
    auto input = object.add_component<i::ActionInput>(map);
    auto other = child.add_component<i::ActionInput>(map);
    i::begin_frame(scenes);
    i::dispatch(scenes, down);
    check(input->context().state("activate").pressed && other->context().state("activate").pressed,
          "Scene set did not deliver input across scenes");
    i::begin_frame(scenes);
    for (auto component : {input, other}) {
        const auto state = component->context().state("activate");
        check(state.active && !state.pressed && !state.released && !state.canceled,
              "Scene set frame reset lost held state or retained edges");
    }
    i::dispatch(scenes, up);
    i::dispatch(scenes, down);
    check(input->context().state("activate").released && input->context().state("activate").pressed &&
              other->context().state("activate").released && other->context().state("activate").pressed,
          "Scene set did not latch ordered events across a frame");
    parent.set_active(false);
    i::begin_frame(scenes);
    check(input->context().state("activate").active && other.enabled() && other->context().state("activate").canceled &&
              !other->context().state("activate").active,
          "Scene set did not reconcile inherited activation independently");
    i::dispatch(scenes, down);
    parent.set_active(true);
    i::begin_frame(scenes);
    check(!other->context().state("activate").active && !other->context().state("activate").pressed,
          "Scene set replayed input held while inactive");
    i::dispatch(scenes, down);
    i::dispatch(scenes, {i::EventType::focus, {}, 0});
    check(input->context().state("activate").canceled && other->context().state("activate").canceled,
          "Scene set failed to cancel input on focus loss");
    i::dispatch(scenes, {i::EventType::focus, {}, 1});
    i::begin_frame(scenes);
    check(!input->context().state("activate").active && !other->context().state("activate").active,
          "Scene set replayed held input on focus gain");

    // Failure in the second scene must preserve physical state and edges in the first.
    input->context().rebind("activate", {{{i::ControlKind::gamepad_button, 0}}});
    other->context().rebind("activate", {{{i::ControlKind::gamepad_button, 0}}});
    for (unsigned device = 0; device < 1024; ++device)
        other->context().process({i::EventType::control, {i::ControlKind::gamepad_button, 0, device}, 1});
    i::begin_frame(scenes);
    rejects([&] { i::dispatch(scenes, {i::EventType::control, {i::ControlKind::gamepad_button, 0, 1024}, 1}); });
    check(!input->context().state("activate").active && !input->context().state("activate").pressed &&
              other->context().state("activate").active && !other->context().state("activate").pressed,
          "Failed event partially updated the scene set");
    input->context().process({i::EventType::control, {i::ControlKind::gamepad_button, 0, 0}, 1});
    i::begin_frame(scenes);
    input.set_enabled(false);
    rejects([&] { i::dispatch(scenes, {i::EventType::control, {i::ControlKind::gamepad_button, 0, 1024}, 1}); });
    check(input->context().enabled() && input->context().state("activate").active &&
              !input->context().state("activate").canceled,
          "Rejected later event partially published earlier enablement");
    input.set_enabled(true);
    input->context().rebind("activate", {{{i::ControlKind::key, 4}}});
    other->context().rebind("activate", {{{i::ControlKind::key, 4}}});
    i::dispatch(scenes, down);

    ComponentCodecs codecs;
    i::add_component_codec(codecs);
    const auto document = serialize_scene(second.get(), {}, codecs);
    const auto old = second;
    second = scenes.replace(second, document, {}, codecs);
    check(!old && !other, "Scene replacement retained old input handles");
    other = second->components<i::ActionInput>().front();
    i::begin_frame(scenes);
    check(input->context().state("activate").active && !other->context().state("activate").active,
          "Scene replacement replayed held input or changed another scene");
    i::dispatch(scenes, down);
    check(other->context().state("activate").pressed, "Replacement scene missed fresh input");
    scenes.unload(second);
    check(!other, "Unloading retained an input attachment");
    i::dispatch(scenes, up);
    check(input->context().state("activate").released, "Unload interrupted remaining scene input");
    scenes.clear();
    i::begin_frame(scenes);
    i::dispatch(scenes, down);
    rejects([&] { i::dispatch(scenes, {i::EventType::control, {i::ControlKind::key, 4, 0}, 2}); });
}
void run_driver_boundaries() {
    BoundaryChecks checks;
    SceneSet scenes;
    auto first = scenes.create("first");
    auto object = first->create();
    auto input =
        object.add_component<i::ActionInput>(i::Map{{"activate", i::ActionType::button, {{{i::ControlKind::key, 4}}}}});
    i::dispatch(scenes, down);
    object.add_component<DriverProbe>(first.get(), scenes, checks);
    scenes.update(.01);
    scenes.fixed_update(.01);
    first->update(.01);
    check(checks.calls == 7 && checks.rejected, "Input driver entered component construction or scheduling");
    check(input->context().state("activate").active && input->context().state("activate").pressed,
          "Rejected nested input drivers changed state");

    struct DuringLoad {};
    Scene source;
    source.create().add_component<DuringLoad>();
    ComponentCodecs codecs;
    bool load_rejected = false;
    codecs.add<DuringLoad>(
        "test.input-boundary.v1", [](const DuringLoad &, const ObjectReferences &) { return "{}"; },
        [&](GameObject target, std::string_view, const ObjectReferences &) {
            load_rejected = rejects_set_drivers(scenes);
            target.add_component<DuringLoad>();
        });
    (void)scenes.load("loaded", serialize_scene(source, {}, codecs), {}, codecs);
    check(load_rejected && input->context().state("activate").active && input->context().state("activate").pressed,
          "Input driver entered set loading or changed state");
    scenes.clear();
    check(checks.calls == 8 && checks.rejected, "Input driver entered set retirement");
    i::begin_frame(scenes);
    i::dispatch(scenes, up);
}
void run() {
    i::Map map{{"activate", i::ActionType::button, {{{i::ControlKind::key, 4}}}}};
    check(i::serialize_map(i::deserialize_map(i::serialize_map(map))) == i::serialize_map(map),
          "Map did not round-trip");
    rejects([] { (void)i::deserialize_map(R"({"version":1,"actions":[]})"); });
    rejects([] { (void)i::deserialize_map(R"({"version":2,"actions":[],"unknown":0})"); });
    rejects([] {
        (void)i::deserialize_map(
            R"({"version":2,"actions":[{"name":"a","type":0,"threshold":0.5,"bindings":[{"kind":0,"code":4,"device":4294967296,"channel":0,"scale":1,"deadzone":0,"modifiers":[]}]}]})");
    });
    Scene scene;
    auto object = scene.create();
    auto input = object.add_component<i::ActionInput>(map);
    i::begin_frame(scene);
    i::dispatch(scene, down);
    check(input->context().state("activate").pressed, "Scene input event was not delivered");
    ComponentCodecs codecs;
    i::add_component_codec(codecs);
    auto prefab = Prefab::capture(object, codecs);
    auto copy = Prefab::deserialize(prefab.serialize({}), {}, codecs).instantiate(scene);
    auto copy_input = copy.get_component<i::ActionInput>();
    check(!copy_input->context().state("activate").active, "Prefab restored live physical state");
    copy_input->context().rebind("activate", {{{i::ControlKind::key, 5}}});
    check(input->context().actions()[0].bindings[0].control.code == 4, "Prefab binding state aliased source");
    input.set_enabled(false);
    i::begin_frame(scene);
    check(input->context().state("activate").canceled, "Disabled component retained held input");
    i::dispatch(scene, down);
    input.set_enabled(true);
    i::begin_frame(scene);
    check(!input->context().state("activate").active, "Re-enabled component replayed input");
    auto parent = scene.create();
    object.set_parent(parent);
    i::dispatch(scene, down);
    parent.set_active(false);
    i::begin_frame(scene);
    check(input.enabled() && input->context().state("activate").canceled, "Inactive hierarchy retained held input");
    i::dispatch(scene, down);
    parent.set_active(true);
    i::begin_frame(scene);
    check(!input->context().state("activate").active, "Activation replayed input held while inactive");
    // A later component's capacity failure must not deliver the event to an earlier one.
    input->context().rebind("activate", {{{i::ControlKind::gamepad_button, 0}}});
    copy_input->context().rebind("activate", {{{i::ControlKind::gamepad_button, 0}}});
    for (unsigned device = 0; device < 1024; ++device)
        copy_input->context().process({i::EventType::control, {i::ControlKind::gamepad_button, 0, device}, 1});
    rejects([&] { i::dispatch(scene, {i::EventType::control, {i::ControlKind::gamepad_button, 0, 1024}, 1}); });
    check(!input->context().state("activate").active, "Failed event partially updated the scene");
    auto nodes = std::vector<Prefab::Node>(prefab.nodes().begin(), prefab.nodes().end());
    nodes.push_back(nodes[0]);
    nodes.back().key = {};
    nodes[1].parent = 0;
    const auto before = scene.size();
    for (const auto &payload : invalid_component_payloads(nodes[0].components[0].state, "version")) {
        nodes[1].components[0].state = payload;
        rejects([&] { (void)Prefab(nodes, codecs).instantiate(scene); });
        check(scene.size() == before, "Malformed input prefab leaked an object");
    }
    object.destroy();
    copy.destroy();
    check(!input && !copy_input, "Input component teardown retained handles");
    run_scene_set();
    run_driver_boundaries();
}
} // namespace
int main() {
    try {
        run();
        chord_configuration();
        chord_persistence();
        chord_staging();
        std::cout << "PASS input scene enablement, configuration, prefab and rollback\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
