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
    rejects([] { (void)i::deserialize_map(R"({"version":2,"actions":[]})"); });
    rejects([] { (void)i::deserialize_map(R"({"version":1,"actions":[],"unknown":0})"); });
    rejects([] {
        (void)i::deserialize_map(
            R"({"version":1,"actions":[{"name":"a","type":0,"threshold":0.5,"bindings":[{"kind":0,"code":4,"device":4294967296,"channel":0,"scale":1,"deadzone":0}]}]})");
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
        std::cout << "PASS input scene enablement, configuration, prefab and rollback\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
