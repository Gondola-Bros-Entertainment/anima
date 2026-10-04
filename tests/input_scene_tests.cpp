#include "component_payloads.hpp"
#include <anima/input_scene.hpp>
#include <anima/prefab.hpp>
#include <anima/scene_set.hpp>
#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace i = anima::input;
using namespace anima;
namespace {
constexpr unsigned control_capacity = 1024; // Recorded controls per Context, documented in include/anima/input.hpp.
constexpr auto over_capacity = "Input context exceeds 1024 active physical controls";
constexpr auto invalid_integer = "Invalid input configuration integer";
constexpr auto invalid_envelope = "Invalid input configuration envelope";
constexpr auto unsupported_version = "Unsupported input configuration version";
constexpr auto invalid_kind = "Invalid input control kind";
constexpr auto invalid_type = "Invalid input action type";
constexpr auto invalid_channel = "Invalid input binding channel";
constexpr auto modifier_count = "Invalid input modifier count";
constexpr auto unknown_component = "Unknown serialized component type";
constexpr auto duplicate_field = "Duplicate JSON document field";
constexpr auto busy_scene = "Scene drivers require an idle live scene";
constexpr auto member_callbacks = "A member scene is running callbacks";
constexpr auto set_updating = "Scene set is updating";
constexpr auto set_changing = "Scene set is changing membership";
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
// A gamepad GUID in the layout SDL builds (USB bus, vendor 0x045e, product 0x028e, version 0x0114), and its
// text in configuration documents.
constexpr i::DeviceIdentity authored_pad{0x03, 0x00, 0x00, 0x00, 0x5e, 0x04, 0x00, 0x00,
                                         0x8e, 0x02, 0x00, 0x00, 0x14, 0x01, 0x00, 0x00};
constexpr auto authored_pad_text = "030000005e0400008e02000014010000";
constexpr auto persisted_device_id = "Input configuration cannot persist a device ID";
constexpr auto invalid_identity = "Invalid input device identity";
// Gamepad button 0 with button 9 as its modifier, on a gamepad reporting @p pad.
i::Map pad_chord_map(const i::DeviceIdentity &pad) {
    i::Binding binding{{i::ControlKind::gamepad_button, 0, i::any_device, pad}};
    binding.modifiers = {{i::ControlKind::gamepad_button, 9, i::any_device, pad}};
    return {{"chord", i::ActionType::button, {binding}}};
}
i::Event pad_event(std::uint16_t button, std::uint32_t device, const i::DeviceIdentity &pad, float value = 1) {
    return {i::EventType::control, {i::ControlKind::gamepad_button, button, device, pad}, value};
}
i::Map key_map() { return {{"activate", i::ActionType::button, {{{i::ControlKind::key, 4}}}}}; }
// Pointer look while the right mouse button is held, Ctrl + wheel zoom, and a wheel-driven button.
i::Map pointer_map() {
    i::Binding look_x{{i::ControlKind::mouse_motion, 0}, i::Channel::x, .25F},
        look_y{{i::ControlKind::mouse_motion, 1}, i::Channel::y, -.25F}, zoom{{i::ControlKind::mouse_wheel, 1}};
    look_x.modifiers = look_y.modifiers = {{i::ControlKind::mouse_button, 3}};
    zoom.modifiers = {{i::ControlKind::key, 224}};
    return {{"look", i::ActionType::vector2, {look_x, look_y}},
            {"zoom", i::ActionType::axis, {zoom}},
            {"previous", i::ActionType::button, {{{i::ControlKind::mouse_wheel, 1}, i::Channel::x, -1}}}};
}
i::Event motion_event(std::uint16_t code, float amount) {
    return {i::EventType::control, {i::ControlKind::mouse_motion, code, 0}, amount};
}
i::Map gamepad_map() { return {{"activate", i::ActionType::button, {{{i::ControlKind::gamepad_button, 0}}}}}; }
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
    for (std::size_t index = 0; index < payloads.size(); ++index)
        result.emplace_back(payloads[index], errors[index]);
    return result;
}
} // namespace

TEST_CASE("Chord configuration round-trips by identity, and only version 4 documents load") {
    const auto map = pad_chord_map(authored_pad);
    const auto document = i::serialize_map(map);
    // Identities are stored as the text SDL_GUIDToString writes, and no identity as null.
    CHECK(document.find(std::string("\"identity\":\"") + authored_pad_text + "\"") != std::string::npos);
    CHECK(i::serialize_map(chord_map()).find("\"identity\":null") != std::string::npos);
    const auto restored = i::deserialize_map(document);
    // The round trip keeps the identity selectors and modifiers.
    CHECK(restored[0].bindings[0].control == map[0].bindings[0].control);
    CHECK(restored[0].bindings[0].modifiers == map[0].bindings[0].modifiers);
    CHECK(i::serialize_map(restored) == document);
    CHECK(i::deserialize_map(R"({"version":4,"actions":[]})").empty());
    // Any version but the integer 4 is reported as a version, before the fields are read.
    for (const auto version : {"3", "5", "4.0", "-1", "true"}) {
        CAPTURE(version);
        const auto invalid = "{\"version\":" + std::string(version) + ",\"actions\":[],\"removed\":0}";
        CHECK_THROWS_WITH_AS(i::deserialize_map(invalid), unsupported_version, std::invalid_argument);
    }
    for (const auto &invalid : invalid_payloads(document, "version", "version")) {
        CAPTURE(invalid.first);
        CHECK_THROWS_WITH_AS(i::deserialize_map(invalid.first), invalid.second.c_str(), std::invalid_argument);
    }
}

TEST_CASE("Device IDs, which name a device only while it is connected, are never persisted") {
    CHECK_THROWS_WITH_AS(i::serialize_map(chord_map(7)), persisted_device_id, std::invalid_argument);
    auto modifier_only = chord_map();
    modifier_only[0].bindings[0].modifiers[0].device = 7;
    CHECK_THROWS_WITH_AS(i::serialize_map(modifier_only), persisted_device_id, std::invalid_argument);
    // Capturing a component whose bindings select a device ID fails the same way.
    Scene source;
    auto object = source.create("session binding");
    object.add_component<i::ActionInput>(chord_map(7));
    ComponentCodecs codecs;
    i::add_component_codec(codecs);
    CHECK_THROWS_WITH_AS(Prefab::capture(object, codecs), persisted_device_id, std::invalid_argument);
    CHECK_THROWS_WITH_AS(serialize_scene(source, {}, codecs), persisted_device_id, std::invalid_argument);
    // A document cannot name one either.
    CHECK_THROWS_WITH_AS(
        i::deserialize_map(
            R"({"version":4,"actions":[{"name":"a","type":"button","threshold":0.5,"bindings":[{"kind":"key","code":4,"device":7,"channel":"x","scale":1,"deadzone":0,"modifiers":[]}]}]})"),
        "Missing JSON field: identity", std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        i::deserialize_map(
            R"({"version":4,"actions":[{"name":"a","type":"button","threshold":0.5,"bindings":[{"kind":"key","code":4,"device":7,"identity":null,"channel":"x","scale":1,"deadzone":0,"modifiers":[]}]}]})"),
        "Unknown JSON field: device", std::invalid_argument);
}

TEST_CASE("Serialized identities are null or 32 lowercase hexadecimal digits, not all zeros") {
    const auto with_identity = [](std::string_view value) {
        return R"({"version":4,"actions":[{"name":"a","type":"button","threshold":0.5,"bindings":[{"kind":"gamepad_button","code":0,"identity":)" +
               std::string(value) + R"(,"channel":"x","scale":1,"deadzone":0,"modifiers":[]}]}]})";
    };
    CHECK(i::deserialize_map(with_identity("null"))[0].bindings[0].control.identity == i::DeviceIdentity{});
    const auto parsed = i::deserialize_map(with_identity(std::string("\"") + authored_pad_text + "\""));
    CHECK(parsed[0].bindings[0].control.identity == authored_pad);
    CHECK(parsed[0].bindings[0].control.device == i::any_device);
    const std::array<std::string_view, 9> invalids{
        R"("030000005E0400008E02000014010000")",  // Uppercase digits.
        R"("030000005e0400008e0200001401000")",   // 31 digits.
        R"("030000005e0400008e020000140100000")", // 33 digits.
        R"("030000005e0400008e0200001401000g")",  // Not hexadecimal.
        R"("00000000000000000000000000000000")",  // All zeros, which means none.
        R"("")",
        "7",
        "true",
        R"(["030000005e0400008e02000014010000"])",
    };
    for (const auto invalid : invalids) {
        CAPTURE(invalid);
        CHECK_THROWS_WITH_AS(i::deserialize_map(with_identity(invalid)), invalid_identity, std::invalid_argument);
    }
}

TEST_CASE("Serialized modifiers must be a list of up to four valid, distinct and compatible controls") {
    const std::string prefix =
        R"({"version":4,"actions":[{"name":"chord","type":"button","threshold":0.5,"bindings":[{"kind":"key","code":4,"identity":")" +
        std::string(authored_pad_text) + R"(","channel":"x","scale":1,"deadzone":0)";
    const std::string suffix = "}]}]}";
    const std::string modifier = R"({"kind":"key","code":224,"identity":null})";
    const auto with_modifiers = [&](std::string_view value) {
        return prefix + ",\"modifiers\":" + std::string(value) + suffix;
    };
    CHECK(i::deserialize_map(with_modifiers("[" + modifier + "]"))[0].bindings[0].modifiers.size() == 1u);
    CHECK_THROWS_WITH_AS(i::deserialize_map(prefix + suffix), "Missing JSON field: modifiers", std::invalid_argument);
    CHECK_THROWS_WITH_AS(i::deserialize_map(with_modifiers("null")), modifier_count, std::invalid_argument);
    CHECK_THROWS_WITH_AS(i::deserialize_map(with_modifiers("{}")), modifier_count, std::invalid_argument);
    CHECK_THROWS_WITH_AS(i::deserialize_map(with_modifiers("[" + modifier + "," + modifier + "]")),
                         "Repeated input chord control", std::invalid_argument);
    CHECK_THROWS_WITH_AS(i::deserialize_map(with_modifiers("[" + modifier + "," + modifier + "," + modifier + "," +
                                                           modifier + "," + modifier + "]")),
                         modifier_count, std::invalid_argument);
    for (const auto &invalid : invalid_payloads(modifier, "kind", "kind")) {
        CAPTURE(invalid.first);
        CHECK_THROWS_WITH_AS(i::deserialize_map(with_modifiers("[" + invalid.first + "]")), invalid.second.c_str(),
                             std::invalid_argument);
    }
    const std::array<std::pair<std::string_view, const char *>, 6> modifiers{{
        {R"({"kind":"gamepad_axis","code":0,"identity":null})", "Input chord modifiers must be digital"},
        {R"({"kind":"key","code":224,"identity":"0300000000000000ffff000000000000"})",
         "Input chord device selectors conflict"},
        {R"({"kind":"key","code":4,"identity":null})", "Repeated input chord control"},
        {R"({"kind":"key","code":512,"identity":null})", invalid_integer},
        {R"({"kind":"key","code":224,"identity":7})", invalid_identity},
        {R"({"kind":"key","code":224,"device":7,"identity":null})", "Unknown JSON field: device"},
    }};
    for (const auto &invalid : modifiers) {
        CAPTURE(invalid.first);
        CHECK_THROWS_WITH_AS(i::deserialize_map(with_modifiers("[" + std::string(invalid.first) + "]")), invalid.second,
                             std::invalid_argument);
    }
}

TEST_CASE("Configuration beyond 1 MiB is rejected in both directions") {
    CHECK_THROWS_WITH_AS(
        i::deserialize_map(std::string(1024 * 1024, ' ') + i::serialize_map(pad_chord_map(authored_pad))),
        "JSON document exceeds byte limit", std::invalid_argument);
    // A valid in-memory map can exceed the wire bound once all modifiers are encoded.
    i::DeviceIdentity widest{};
    widest.fill(0xff);
    i::Binding wide{{i::ControlKind::gamepad_axis, 15, i::any_device, widest}, i::Channel::x, 16, .999999F};
    wide.modifiers = {{i::ControlKind::gamepad_button, 60, i::any_device, widest},
                      {i::ControlKind::gamepad_button, 61, i::any_device, widest},
                      {i::ControlKind::gamepad_button, 62, i::any_device, widest},
                      {i::ControlKind::gamepad_button, 63, i::any_device, widest}};
    i::Map large;
    for (unsigned action = 0; action < 128; ++action)
        large.push_back({"action" + std::to_string(action), i::ActionType::axis, std::vector<i::Binding>(32, wide)});
    CHECK_NOTHROW(i::validate(large));
    CHECK_THROWS_WITH_AS(i::serialize_map(large), "Input configuration exceeds byte limit", std::invalid_argument);
}

TEST_CASE("Persisted chords keep their identity and follow it to each connected device, restoring no runtime state") {
    constexpr i::DeviceIdentity other_pad{0x05, 0x00, 0x00, 0x00, 0x4c, 0x05, 0x00, 0x00,
                                          0xe6, 0x0c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};
    Scene source;
    auto object = source.create("authored chord");
    auto input = object.add_component<i::ActionInput>(pad_chord_map(authored_pad));
    input->context().process(pad_event(0, 7, authored_pad));
    input->context().process(pad_event(9, 7, authored_pad));
    CHECK(input->context().state("chord").pressed);
    ComponentCodecs source_codecs;
    i::add_component_codec(source_codecs);
    const auto prefab = Prefab::capture(object, source_codecs);
    CHECK(prefab.nodes()[0].components[0].type == "anima.action-input.v4");
    input->context().set_focused(false);
    input->context().set_enabled(false);
    const auto scene_document = serialize_scene(source, {}, source_codecs);
    object.destroy();
    ComponentCodecs destination_codecs;
    i::add_component_codec(destination_codecs);
    Scene destination;
    CHECK_THROWS_WITH_AS(prefab.instantiate(destination, identity(), {}), unknown_component, std::invalid_argument);
    CHECK(destination.size() == 0u); // The missing destination codec leaked no object.
    auto instance = Prefab::deserialize(prefab.serialize({}), {}, destination_codecs)
                        .instantiate(destination, identity(), destination_codecs);
    auto loaded = load_scene(scene_document, {}, destination_codecs);
    // The prefab instance, then the loaded scene's component: each restores the configuration, including the
    // authored identity, into an enabled and focused context without recorded input.
    std::array restored{instance.get_component<i::ActionInput>(), loaded->components<i::ActionInput>().front()};
    for (std::size_t index = 0; index < restored.size(); ++index) {
        CAPTURE(index);
        auto &context = restored[index]->context();
        CHECK(context.enabled());
        CHECK(context.focused());
        CHECK_FALSE(context.state("chord").active);
        CHECK_FALSE(context.state("chord").pressed);
        CHECK(context.actions()[0].bindings[0].modifiers[0].identity == authored_pad);
        CHECK(context.actions()[0].bindings[0].modifiers[0].device == i::any_device);
        context.process(pad_event(0, 3, other_pad));
        context.process(pad_event(9, 3, other_pad));
        CHECK_FALSE(context.state("chord").active); // A gamepad reporting another identity does not match.
        // The authored gamepad matches under the ID it has in this session, and again under a new ID after
        // reconnecting.
        context.process(pad_event(0, 12, authored_pad));
        context.process(pad_event(9, 12, authored_pad));
        CHECK(context.state("chord").pressed);
        context.process({i::EventType::disconnect, {i::ControlKind::gamepad_button, 0, 12}});
        CHECK_FALSE(context.state("chord").active);
        context.process(pad_event(0, 13, authored_pad));
        context.process(pad_event(9, 13, authored_pad));
        CHECK(context.state("chord").active);
    }
}

TEST_CASE("Input components of an old type or with a malformed payload are rejected without leaking objects") {
    Scene source;
    auto object = source.create();
    object.add_component<i::ActionInput>(pad_chord_map(authored_pad));
    ComponentCodecs codecs;
    i::add_component_codec(codecs);
    const auto prefab = Prefab::capture(object, codecs);
    auto nodes = std::vector<Prefab::Node>(prefab.nodes().begin(), prefab.nodes().end());
    nodes[0].components[0].type = "anima.action-input.v3";
    CHECK_THROWS_WITH_AS(Prefab(nodes, codecs), unknown_component, std::invalid_argument);
    nodes[0] = prefab.nodes()[0];
    nodes.push_back(nodes[0]);
    nodes[1].parent = 0;
    nodes[1].key = {};
    Scene destination;
    for (const auto &invalid : invalid_payloads(nodes[0].components[0].state, "version", "version")) {
        CAPTURE(invalid.first);
        nodes[1].components[0].state = invalid.first;
        CHECK_THROWS_WITH_AS(Prefab(nodes, codecs).instantiate(destination), invalid.second.c_str(),
                             std::invalid_argument);
        CHECK(destination.size() == 0u); // The late malformed component leaked no staged object.
    }
}

TEST_CASE("A later scene's capacity failure publishes no chord state in any scene") {
    SceneSet scenes;
    auto first = scenes.create("first"), second = scenes.create("second");
    auto input = first->create().add_component<i::ActionInput>(chord_map());
    auto other = second->create().add_component<i::ActionInput>(chord_map());
    input->context().process(key_event(4, 5));
    for (unsigned device = 0; device < control_capacity; ++device)
        other->context().process(key_event(4, device));
    i::begin_frame(scenes);
    CHECK_THROWS_WITH_AS(i::dispatch(scenes, key_event(224, 5)), over_capacity, std::length_error);
    CHECK_FALSE(input->context().state("chord").active);
    CHECK_FALSE(input->context().state("chord").pressed);
    CHECK_FALSE(other->context().state("chord").active);
    CHECK_FALSE(other->context().state("chord").pressed);
    other->context().process(key_event(4, 0, 0));
    i::dispatch(scenes, key_event(4, 5));
    // The rejected modifier was recorded in neither scene.
    CHECK_FALSE(input->context().state("chord").active);
    CHECK_FALSE(other->context().state("chord").active);
    i::dispatch(scenes, key_event(224, 5));
    // Once a recorded control is released, the fresh modifier fits.
    CHECK(input->context().state("chord").pressed);
    CHECK(other->context().state("chord").pressed);
}

namespace {
// The input driver calls made from inside the scenes, and what the set drivers must report in the current phase.
struct BoundaryChecks {
    unsigned calls{};
    const char *set_error = member_callbacks;
};
// Calls every input driver from component construction and scheduling, where each must be rejected. The hooks
// are noexcept, so they use only CHECK assertions, which report a failure without throwing.
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
        CAPTURE(checks.calls);
        CHECK_THROWS_WITH_AS(i::begin_frame(scene), busy_scene, std::logic_error);
        CHECK_THROWS_WITH_AS(i::dispatch(scene, up), busy_scene, std::logic_error);
        CHECK_THROWS_WITH_AS(i::begin_frame(scenes), checks.set_error, std::logic_error);
        CHECK_THROWS_WITH_AS(i::dispatch(scenes, up), checks.set_error, std::logic_error);
    }
    void on_enable() noexcept { attempt(); }
    void on_disable() noexcept { attempt(); }
    void on_update(double) { attempt(); }
    void on_fixed_update(double) { attempt(); }
    void on_late_update(double) { attempt(); }
};
} // namespace

TEST_CASE("A scene set delivers input across its scenes and reconciles each component's activation") {
    SceneSet scenes;
    auto first = scenes.create("first"), second = scenes.create("second");
    auto object = first->create(), parent = second->create(), child = second->create();
    child.set_parent(parent);
    auto input = object.add_component<i::ActionInput>(key_map());
    auto other = child.add_component<i::ActionInput>(key_map());
    i::begin_frame(scenes);
    i::dispatch(scenes, down);
    CHECK(input->context().state("activate").pressed);
    CHECK(other->context().state("activate").pressed);
    i::begin_frame(scenes);
    // A new frame keeps held state and clears the edges, in every scene.
    const std::array components{input, other};
    for (std::size_t index = 0; index < components.size(); ++index) {
        CAPTURE(index);
        const auto state = components[index]->context().state("activate");
        CHECK(state.active);
        CHECK_FALSE(state.pressed);
        CHECK_FALSE(state.released);
        CHECK_FALSE(state.canceled);
    }
    i::dispatch(scenes, up);
    i::dispatch(scenes, down);
    // Both events of the frame stay latched, in every scene.
    CHECK(input->context().state("activate").released);
    CHECK(input->context().state("activate").pressed);
    CHECK(other->context().state("activate").released);
    CHECK(other->context().state("activate").pressed);
    parent.set_active(false);
    i::begin_frame(scenes);
    // The inactive parent cancels only its own component, which stays enabled.
    CHECK(input->context().state("activate").active);
    CHECK(other.enabled());
    CHECK(other->context().state("activate").canceled);
    CHECK_FALSE(other->context().state("activate").active);
    i::dispatch(scenes, down);
    parent.set_active(true);
    i::begin_frame(scenes);
    CHECK_FALSE(other->context().state("activate").active); // Input held while inactive is not replayed.
    CHECK_FALSE(other->context().state("activate").pressed);
    i::dispatch(scenes, down);
    i::dispatch(scenes, {i::EventType::focus, {}, 0});
    CHECK(input->context().state("activate").canceled); // Losing focus cancels every scene's input.
    CHECK(other->context().state("activate").canceled);
    i::dispatch(scenes, {i::EventType::focus, {}, 1});
    i::begin_frame(scenes);
    CHECK_FALSE(input->context().state("activate").active); // Regaining focus replays none.
    CHECK_FALSE(other->context().state("activate").active);
}

TEST_CASE("A scene set event rejected in one scene changes no scene") {
    SceneSet scenes;
    auto first = scenes.create("first"), second = scenes.create("second");
    auto input = first->create().add_component<i::ActionInput>(gamepad_map());
    auto other = second->create().add_component<i::ActionInput>(gamepad_map());
    for (unsigned device = 0; device < control_capacity; ++device)
        other->context().process({i::EventType::control, {i::ControlKind::gamepad_button, 0, device}, 1});
    i::begin_frame(scenes);
    const i::Event beyond{i::EventType::control, {i::ControlKind::gamepad_button, 0, control_capacity}, 1};
    CHECK_THROWS_WITH_AS(i::dispatch(scenes, beyond), over_capacity, std::length_error);
    // Failure in the second scene must preserve physical state and edges in the first.
    CHECK_FALSE(input->context().state("activate").active);
    CHECK_FALSE(input->context().state("activate").pressed);
    CHECK(other->context().state("activate").active);
    CHECK_FALSE(other->context().state("activate").pressed);
    input->context().process({i::EventType::control, {i::ControlKind::gamepad_button, 0, 0}, 1});
    i::begin_frame(scenes);
    input.set_enabled(false);
    CHECK_THROWS_WITH_AS(i::dispatch(scenes, beyond), over_capacity, std::length_error);
    // Nor does it publish the first component's disablement.
    CHECK(input->context().enabled());
    CHECK(input->context().state("activate").active);
    CHECK_FALSE(input->context().state("activate").canceled);
    i::begin_frame(scenes);
    CHECK_FALSE(input->context().enabled());
    input.set_enabled(true);
    CHECK_THROWS_WITH_AS(i::dispatch(scenes, beyond), over_capacity, std::length_error);
    // Or its enablement.
    CHECK_FALSE(input->context().enabled());
}

TEST_CASE("Replacing, unloading and clearing scenes detaches their input without disturbing the rest") {
    SceneSet scenes;
    auto first = scenes.create("first"), second = scenes.create("second");
    auto input = first->create().add_component<i::ActionInput>(key_map());
    auto other = second->create().add_component<i::ActionInput>(key_map());
    i::dispatch(scenes, down);
    ComponentCodecs codecs;
    i::add_component_codec(codecs);
    const auto document = serialize_scene(second.get(), {}, codecs);
    const auto old = second;
    second = scenes.replace(second, document, {}, codecs);
    CHECK_FALSE(old); // Replacement invalidates the old scene and its input handles.
    CHECK_FALSE(other);
    other = second->components<i::ActionInput>().front();
    i::begin_frame(scenes);
    CHECK(input->context().state("activate").active);       // The other scene keeps its held input.
    CHECK_FALSE(other->context().state("activate").active); // The replacement replays none.
    i::dispatch(scenes, down);
    CHECK(other->context().state("activate").pressed);
    scenes.unload(second);
    CHECK_FALSE(other);
    i::dispatch(scenes, up);
    CHECK(input->context().state("activate").released); // Unloading did not interrupt the remaining scene.
    scenes.clear();
    i::begin_frame(scenes);
    i::dispatch(scenes, down);
    CHECK_THROWS_WITH_AS(i::dispatch(scenes, {i::EventType::control, {i::ControlKind::key, 4, 0}, 2}),
                         "Invalid input control value", std::invalid_argument);
}

TEST_CASE("Input drivers are rejected during component construction, scheduling, set loading and retirement") {
    BoundaryChecks checks;
    SceneSet scenes;
    auto first = scenes.create("first");
    auto object = first->create();
    auto input = object.add_component<i::ActionInput>(key_map());
    i::dispatch(scenes, down);
    checks.set_error = member_callbacks; // Construction outside a set update leaves only the member busy.
    object.add_component<DriverProbe>(first.get(), scenes, checks);
    checks.set_error = set_updating;
    scenes.update(.01);
    scenes.fixed_update(.01);
    checks.set_error = member_callbacks; // So does updating one scene directly.
    first->update(.01);
    CHECK(checks.calls == 7u);
    // The rejected drivers changed no state.
    CHECK(input->context().state("activate").active);
    CHECK(input->context().state("activate").pressed);

    struct DuringLoad {};
    Scene source;
    source.create().add_component<DuringLoad>();
    ComponentCodecs codecs;
    unsigned loads = 0;
    codecs.add<DuringLoad>(
        "test.input-boundary.v1", [](const DuringLoad &, const ObjectReferences &) { return "{}"; },
        [&](GameObject target, std::string_view, const ObjectReferences &) {
            ++loads;
            CHECK_THROWS_WITH_AS(i::begin_frame(scenes), set_changing, std::logic_error);
            CHECK_THROWS_WITH_AS(i::dispatch(scenes, up), set_changing, std::logic_error);
            target.add_component<DuringLoad>();
        });
    (void)scenes.load("loaded", serialize_scene(source, {}, codecs), {}, codecs);
    CHECK(loads == 1u);
    CHECK(input->context().state("activate").active);
    CHECK(input->context().state("activate").pressed);
    checks.set_error = set_changing;
    scenes.clear();
    CHECK(checks.calls == 8u); // Retirement disabled the probe.
    i::begin_frame(scenes);
    i::dispatch(scenes, up);
}

TEST_CASE("Maps round-trip, and other versions, unknown fields and out-of-range codes are rejected") {
    const auto map = key_map();
    CHECK(i::serialize_map(i::deserialize_map(i::serialize_map(map))) == i::serialize_map(map));
    CHECK_THROWS_WITH_AS(i::deserialize_map(R"({"version":1,"actions":[]})"), unsupported_version,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(i::deserialize_map(R"({"version":4,"actions":{}})"), invalid_envelope, std::invalid_argument);
    CHECK_THROWS_WITH_AS(i::deserialize_map(R"({"version":4,"actions":[],"unknown":0})"), "Unknown JSON field: unknown",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        i::deserialize_map(
            R"({"version":4,"actions":[{"name":"a","type":"button","threshold":0.5,"bindings":[{"kind":"key","code":512,"identity":null,"channel":"x","scale":1,"deadzone":0,"modifiers":[]}]}]})"),
        invalid_integer, std::invalid_argument);
}

TEST_CASE("Enumerators are stored by name, and integers, other types and unknown names are rejected") {
    // Every action type, control kind and channel.
    const i::Map map{
        {"move",
         i::ActionType::vector2,
         {{{i::ControlKind::gamepad_axis, 0}, i::Channel::x}, {{i::ControlKind::gamepad_axis, 1}, i::Channel::y}}},
        {"jump",
         i::ActionType::button,
         {{{i::ControlKind::key, 44}}, {{i::ControlKind::mouse_button, 1}}, {{i::ControlKind::gamepad_button, 0}}}},
        {"turn", i::ActionType::axis, {{{i::ControlKind::mouse_motion, 0}}, {{i::ControlKind::mouse_wheel, 1}}}}};
    const auto document = i::serialize_map(map);
    for (const auto name :
         {R"("type":"vector2")", R"("type":"button")", R"("type":"axis")", R"("kind":"gamepad_axis")",
          R"("kind":"key")", R"("kind":"mouse_button")", R"("kind":"gamepad_button")", R"("kind":"mouse_motion")",
          R"("kind":"mouse_wheel")", R"("channel":"x")", R"("channel":"y")"}) {
        CAPTURE(name);
        CHECK(document.find(name) != std::string::npos);
    }
    const auto restored = i::deserialize_map(document);
    REQUIRE(restored.size() == map.size());
    for (std::size_t action = 0; action < map.size(); ++action) {
        CAPTURE(action);
        CHECK(restored[action].type == map[action].type);
        REQUIRE(restored[action].bindings.size() == map[action].bindings.size());
        for (std::size_t binding = 0; binding < map[action].bindings.size(); ++binding) {
            CAPTURE(binding);
            CHECK(restored[action].bindings[binding].control == map[action].bindings[binding].control);
            CHECK(restored[action].bindings[binding].channel == map[action].bindings[binding].channel);
        }
    }
    CHECK(i::serialize_map(restored) == document);
    // A version 3 document, which stored enumerator values, is another version.
    CHECK_THROWS_WITH_AS(
        i::deserialize_map(
            R"({"version":3,"actions":[{"name":"a","type":0,"threshold":0.5,"bindings":[{"kind":0,"code":4,"identity":null,"channel":0,"scale":1,"deadzone":0,"modifiers":[]}]}]})"),
        unsupported_version, std::invalid_argument);
    const std::string valid =
        R"({"version":4,"actions":[{"name":"a","type":"button","threshold":0.5,"bindings":[{"kind":"key","code":4,"identity":null,"channel":"x","scale":1,"deadzone":0,"modifiers":[{"kind":"key","code":224,"identity":null}]}]}]})";
    REQUIRE(i::deserialize_map(valid)[0].bindings[0].modifiers.size() == 1u);
    const auto changed = [&](std::string_view from, std::string_view to) {
        auto result = valid;
        const auto at = result.find(from);
        REQUIRE(at != std::string::npos);
        return result.replace(at, from.size(), to);
    };
    constexpr std::string_view binding_kind = R"("kind":"key","code":4)", modifier_kind = R"("kind":"key","code":224)";
    const std::array<std::pair<std::string, const char *>, 17> invalids{{
        {changed(R"("type":"button")", R"("type":0)"), invalid_type},
        {changed(R"("type":"button")", R"("type":"Button")"), invalid_type},
        {changed(R"("type":"button")", R"("type":"trigger")"), invalid_type},
        {changed(R"("type":"button")", R"("type":null)"), invalid_type},
        {changed(R"("type":"button")", R"("type":["button"])"), invalid_type},
        {changed(binding_kind, R"("kind":0,"code":4)"), invalid_kind},
        {changed(binding_kind, R"("kind":"keyboard","code":4)"), invalid_kind},
        {changed(binding_kind, R"("kind":"KEY","code":4)"), invalid_kind},
        {changed(binding_kind, R"("kind":"key\u0000","code":4)"), invalid_kind},
        {changed(binding_kind, R"("kind":"","code":4)"), invalid_kind},
        {changed(modifier_kind, R"("kind":0,"code":224)"), invalid_kind},
        {changed(modifier_kind, R"("kind":"ctrl","code":224)"), invalid_kind},
        {changed(R"("channel":"x")", R"("channel":0)"), invalid_channel},
        {changed(R"("channel":"x")", R"("channel":"X")"), invalid_channel},
        {changed(R"("channel":"x")", R"("channel":"z")"), invalid_channel},
        {changed(R"("channel":"x")", R"("channel":true)"), invalid_channel},
        // A known name still passes validate(), which allows the second channel only in vector2 actions.
        {changed(R"("channel":"x")", R"("channel":"y")"), "Invalid input binding channel/scale/deadzone"},
    }};
    for (const auto &[invalid, error] : invalids) {
        CAPTURE(invalid);
        CHECK_THROWS_WITH_AS(i::deserialize_map(invalid), error, std::invalid_argument);
    }
}

TEST_CASE("Scene input follows enablement and activation, and prefab copies restore only configuration") {
    Scene scene;
    auto object = scene.create();
    auto input = object.add_component<i::ActionInput>(key_map());
    i::begin_frame(scene);
    i::dispatch(scene, down);
    CHECK(input->context().state("activate").pressed);
    ComponentCodecs codecs;
    i::add_component_codec(codecs);
    auto prefab = Prefab::capture(object, codecs);
    auto copy = Prefab::deserialize(prefab.serialize({}), {}, codecs).instantiate(scene);
    auto copy_input = copy.get_component<i::ActionInput>();
    CHECK_FALSE(copy_input->context().state("activate").active); // The copy restores no physical state.
    copy_input->context().rebind("activate", {{{i::ControlKind::key, 5}}});
    CHECK(input->context().actions()[0].bindings[0].control.code == 4); // Nor does it share bindings.
    input.set_enabled(false);
    i::begin_frame(scene);
    CHECK(input->context().state("activate").canceled); // Disabling the component cancels held input.
    i::dispatch(scene, down);
    input.set_enabled(true);
    i::begin_frame(scene);
    CHECK_FALSE(input->context().state("activate").active); // Enabling it again replays nothing.
    auto parent = scene.create();
    object.set_parent(parent);
    i::dispatch(scene, down);
    parent.set_active(false);
    i::begin_frame(scene);
    // An inactive parent cancels held input, though the component stays enabled.
    CHECK(input.enabled());
    CHECK(input->context().state("activate").canceled);
    i::dispatch(scene, down);
    parent.set_active(true);
    i::begin_frame(scene);
    CHECK_FALSE(input->context().state("activate").active); // Activation replays nothing held while inactive.
    object.destroy();
    copy.destroy();
    CHECK_FALSE(input); // Teardown invalidates the component handles.
    CHECK_FALSE(copy_input);
}

TEST_CASE("A scene event rejected by a later component reaches no earlier one") {
    Scene scene;
    auto input = scene.create().add_component<i::ActionInput>(gamepad_map());
    auto full = scene.create().add_component<i::ActionInput>(gamepad_map());
    for (unsigned device = 0; device < control_capacity; ++device)
        full->context().process({i::EventType::control, {i::ControlKind::gamepad_button, 0, device}, 1});
    CHECK_THROWS_WITH_AS(
        i::dispatch(scene, {i::EventType::control, {i::ControlKind::gamepad_button, 0, control_capacity}, 1}),
        over_capacity, std::length_error);
    CHECK_FALSE(input->context().state("activate").active);
}

TEST_CASE("A malformed input payload fails its prefab without leaking objects") {
    Scene scene;
    auto object = scene.create();
    object.add_component<i::ActionInput>(key_map());
    ComponentCodecs codecs;
    i::add_component_codec(codecs);
    const auto prefab = Prefab::capture(object, codecs);
    auto nodes = std::vector<Prefab::Node>(prefab.nodes().begin(), prefab.nodes().end());
    nodes.push_back(nodes[0]);
    nodes.back().key = {};
    nodes[1].parent = 0;
    const auto before = scene.size();
    for (const auto &invalid : invalid_payloads(nodes[0].components[0].state, "version", "version")) {
        CAPTURE(invalid.first);
        nodes[1].components[0].state = invalid.first;
        CHECK_THROWS_WITH_AS(Prefab(nodes, codecs).instantiate(scene), invalid.second.c_str(), std::invalid_argument);
        CHECK(scene.size() == before);
    }
}

TEST_CASE("Delta bindings round-trip with their modifiers, and invalid delta documents are rejected") {
    const auto map = pointer_map();
    const auto document = i::serialize_map(map);
    // The delta kinds are stored by name.
    CHECK(document.find(R"("version":4)") != std::string::npos);
    CHECK(document.find(R"("kind":"mouse_motion")") != std::string::npos);
    CHECK(document.find(R"("kind":"mouse_wheel")") != std::string::npos);
    const auto restored = i::deserialize_map(document);
    REQUIRE(restored.size() == map.size());
    for (std::size_t action = 0; action < map.size(); ++action) {
        CAPTURE(action);
        CHECK(restored[action].name == map[action].name);
        CHECK(restored[action].type == map[action].type);
        REQUIRE(restored[action].bindings.size() == map[action].bindings.size());
        for (std::size_t binding = 0; binding < map[action].bindings.size(); ++binding) {
            CAPTURE(binding);
            const auto &expected = map[action].bindings[binding];
            const auto &actual = restored[action].bindings[binding];
            CHECK(actual.control == expected.control);
            CHECK(actual.channel == expected.channel);
            CHECK(actual.scale == expected.scale);
            CHECK(actual.deadzone == expected.deadzone);
            CHECK(actual.modifiers == expected.modifiers);
        }
    }
    CHECK(i::serialize_map(restored) == document);
    // The component codec's key names the document version.
    Scene scene;
    auto object = scene.create();
    object.add_component<i::ActionInput>(map);
    ComponentCodecs codecs;
    i::add_component_codec(codecs);
    CHECK(Prefab::capture(object, codecs).nodes()[0].components[0].type == "anima.action-input.v4");
    const auto with_bindings = [](std::string_view type, std::string_view bindings) {
        return R"({"version":4,"actions":[{"name":"a","type":")" + std::string(type) +
               R"(","threshold":0.5,"bindings":[)" + std::string(bindings) + "]}]}";
    };
    constexpr auto wheel_binding =
        R"({"kind":"mouse_wheel","code":1,"identity":null,"channel":"x","scale":1,"deadzone":0)";
    const std::array<std::pair<std::string, const char *>, 5> invalids{{
        {with_bindings(
             "axis",
             R"({"kind":"mouse_motion","code":2,"identity":null,"channel":"x","scale":1,"deadzone":0,"modifiers":[]})"),
         "Input control code outside supported range"},
        {with_bindings(
             "axis",
             R"({"kind":"joystick","code":0,"identity":null,"channel":"x","scale":1,"deadzone":0,"modifiers":[]})"),
         invalid_kind},
        {with_bindings("axis", std::string(wheel_binding) +
                                   R"(,"modifiers":[{"kind":"mouse_motion","code":0,"identity":null}]})"),
         "Input chord modifiers must be digital"},
        {with_bindings(
             "axis",
             R"({"kind":"mouse_wheel","code":1,"identity":null,"channel":"x","scale":1,"deadzone":0.25,"modifiers":[]})"),
         "Input delta bindings require a zero deadzone"},
        {with_bindings(
             "axis", R"({"kind":"key","code":4,"identity":null,"channel":"x","scale":1,"deadzone":0,"modifiers":[]},)" +
                         std::string(wheel_binding) + R"(,"modifiers":[]})"),
         "Input axis and vector2 actions cannot mix held and delta controls"},
    }};
    for (const auto &[invalid, error] : invalids) {
        CAPTURE(invalid);
        CHECK_THROWS_WITH_AS(i::deserialize_map(invalid), error, std::invalid_argument);
    }
    // A button may mix them.
    CHECK(
        i::deserialize_map(with_bindings("button", R"({"kind":"key","code":4,"identity":null,"channel":"x","scale":1,)"
                                                   R"("deadzone":0,"modifiers":[]},)" +
                                                       std::string(wheel_binding) + R"(,"modifiers":[]})"))
            .size() == 1u);
}

TEST_CASE("A scene set's frame of increments reaches every scene, and the next frame starts at zero") {
    SceneSet scenes;
    auto first = scenes.create("first"), second = scenes.create("second");
    auto input = first->create().add_component<i::ActionInput>(pointer_map());
    auto other = second->create().add_component<i::ActionInput>(pointer_map());
    const std::array components{input, other};
    i::begin_frame(scenes);
    i::dispatch(scenes, motion_event(0, 40)); // Before the right button, so no look.
    i::dispatch(scenes, {i::EventType::control, {i::ControlKind::mouse_button, 3, 0}, 1});
    i::dispatch(scenes, motion_event(0, 8));
    i::dispatch(scenes, motion_event(1, 4));
    i::dispatch(scenes, {i::EventType::control, {i::ControlKind::mouse_wheel, 1, 0}, -1});
    for (std::size_t index = 0; index < components.size(); ++index) {
        CAPTURE(index);
        const auto &context = components[index]->context();
        CHECK(context.state("look").value.x == 2);
        CHECK(context.state("look").value.y == -1);
        CHECK(context.state("previous").pressed);
        CHECK(context.state("zoom").value.x == 0); // Ctrl is not held.
    }
    i::begin_frame(scenes);
    i::dispatch(scenes, motion_event(1, 8));
    for (std::size_t index = 0; index < components.size(); ++index) {
        CAPTURE(index);
        const auto &context = components[index]->context();
        CHECK(context.state("look").value.x == 0);
        CHECK(context.state("look").value.y == -2); // The held button still gates the new frame's motion.
        CHECK(context.state("previous").released);
        CHECK_FALSE(context.state("previous").active);
    }
}

TEST_CASE("An increment that would overflow a sum in one scene changes no scene") {
    constexpr float largest = std::numeric_limits<float>::max();
    const i::Map map{{"look", i::ActionType::axis, {{{i::ControlKind::mouse_motion, 0}}}}};
    SceneSet scenes;
    auto first = scenes.create("first"), second = scenes.create("second");
    auto input = first->create().add_component<i::ActionInput>(map);
    auto other = second->create().add_component<i::ActionInput>(map);
    i::begin_frame(scenes);
    other->context().process(motion_event(0, largest));
    CHECK_THROWS_WITH_AS(i::dispatch(scenes, motion_event(0, largest)),
                         "Input increment would overflow a delta binding's sum", std::overflow_error);
    CHECK(input->context().state("look").value.x == 0);
    CHECK_FALSE(input->context().state("look").pressed);
    CHECK(other->context().state("look").value.x == largest);
    i::dispatch(scenes, motion_event(0, -largest));
    CHECK(input->context().state("look").value.x == -largest);
    CHECK(other->context().state("look").value.x == 0);
}
