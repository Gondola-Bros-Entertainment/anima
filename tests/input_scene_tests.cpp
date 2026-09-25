#include "component_payloads.hpp"
#include <anima/input_scene.hpp>
#include <anima/prefab.hpp>
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
    const i::Event down{i::EventType::control, {i::ControlKind::key, 4, 0}, 1};
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
