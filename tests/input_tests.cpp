#include "near.hpp"
#include <anima/input.hpp>
#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace i = anima::input;
namespace {
constexpr float tolerance = 1e-6F;
constexpr unsigned control_capacity = 1024; // Recorded controls per Context, documented in include/anima/input.hpp.
constexpr auto code_range = "Input control code outside supported range";
constexpr auto repeated = "Repeated input chord control";
constexpr auto conflict = "Input chord device selectors conflict";
constexpr auto binding_error = "Invalid input binding channel/scale/deadzone";
constexpr auto over_capacity = "Input context exceeds 1024 active physical controls";
constexpr auto digital_modifiers = "Input chord modifiers must be digital";
constexpr auto delta_deadzone = "Input delta bindings require a zero deadzone";
constexpr auto mixed_controls = "Input axis and vector2 actions cannot mix held and delta controls";
constexpr auto invalid_value = "Invalid input control value";
constexpr auto sum_overflow = "Input increment would overflow a delta binding's sum";
constexpr auto over_sum_capacity = "Input context exceeds 1024 delta sums";
constexpr unsigned sum_capacity = 1024; // Delta sums per Context, documented in include/anima/input.hpp.
constexpr std::uint16_t x_code = 0, y_code = 1, right_button = 3, left_ctrl = 224;

i::Event key(unsigned code, bool down, unsigned device = 0) {
    return {i::EventType::control, {i::ControlKind::key, static_cast<std::uint16_t>(code), device}, down ? 1.F : 0.F};
}
i::Binding chord(i::Control primary, std::vector<i::Control> modifiers) {
    i::Binding result;
    result.control = primary;
    result.modifiers = std::move(modifiers);
    return result;
}
i::Event event(i::ControlKind kind, unsigned code, unsigned device, float value = 1) {
    return {i::EventType::control, {kind, static_cast<std::uint16_t>(code), device}, value};
}
// A device identity whose first byte is @p model and whose other bytes are zero.
i::DeviceIdentity model_identity(std::uint8_t model) {
    i::DeviceIdentity result{};
    result[0] = model;
    return result;
}
i::Event reported(i::ControlKind kind, unsigned code, unsigned device, const i::DeviceIdentity &identity,
                  float value = 1) {
    return {i::EventType::control, {kind, static_cast<std::uint16_t>(code), device, identity}, value};
}
// An increment of @p amount on axis @p code of pointer motion or the wheel.
i::Event motion(std::uint16_t code, float amount, unsigned device = 0) {
    return event(i::ControlKind::mouse_motion, code, device, amount);
}
i::Event wheel(std::uint16_t code, float amount, unsigned device = 0) {
    return event(i::ControlKind::mouse_wheel, code, device, amount);
}
i::Binding delta(i::ControlKind kind, std::uint16_t code, i::Channel channel = i::Channel::x, float scale = 1) {
    return {{kind, code}, channel, scale};
}
// Every edge latch of @p state is clear.
bool unlatched(const i::State &state) { return !state.pressed && !state.released && !state.canceled; }
i::Map movement_map() {
    return {{"trigger", i::ActionType::button, {{{i::ControlKind::key, 4}}}},
            {"move",
             i::ActionType::vector2,
             {{{i::ControlKind::key, 5}, i::Channel::x},
              {{i::ControlKind::key, 6}, i::Channel::x, -1},
              {{i::ControlKind::key, 7}, i::Channel::y}}}};
}
} // namespace

TEST_CASE("Action state follows events across frames, devices, focus, enablement, copies and rebinding") {
    static_assert(std::is_nothrow_move_assignable_v<i::Context>);
    i::Context c(movement_map());
    c.process(key(4, true));
    c.process(key(4, true));
    c.process(key(4, false));
    auto s = c.state("trigger");
    // A tap between frames latches both edges.
    CHECK_FALSE(s.active);
    CHECK(s.pressed);
    CHECK(s.released);
    CHECK(s.value.x == 0);
    c.begin_frame();
    CHECK_FALSE(c.state("trigger").pressed);
    CHECK_FALSE(c.state("trigger").released);
    c.process(key(5, true));
    c.process(key(7, true));
    s = c.state("move");
    CHECK(s.value.x == Near{std::sqrt(.5F), tolerance}); // Diagonal movement is normalized.
    CHECK(s.value.y == s.value.x);
    c.process(key(6, true));
    CHECK(c.state("move").value.x == 0); // Opposing keys cancel.
    CHECK(c.state("move").value.y == 1);
    c.process(key(4, true, 1));
    c.process(key(4, true, 2));
    c.process(key(4, false, 1));
    CHECK(c.state("trigger").active); // Releasing one keyboard keeps another's press.
    c.process({i::EventType::disconnect, {i::ControlKind::key, 0, 2}});
    CHECK_FALSE(c.state("trigger").active);
    CHECK(c.state("move").active); // Removing a keyboard forgets only its own controls.
    c.process(key(4, true));
    c.process({i::EventType::focus, {}, 0});
    CHECK(c.state("trigger").canceled); // Losing focus cancels the pending press.
    CHECK(c.state("trigger").released);
    CHECK_FALSE(c.state("trigger").pressed);
    c.process(key(4, true));
    c.set_focused(true);
    CHECK_FALSE(c.state("trigger").active); // Regaining focus replays nothing ignored meanwhile.
    c.process(key(4, true));
    c.begin_frame();
    c.process(key(4, true));
    CHECK_FALSE(c.state("trigger").pressed); // A repeated value does not press again.
    CHECK(c.state("trigger").active);
    i::Context snapshot;
    snapshot = c;
    c.set_enabled(false);
    c.process(key(4, true));
    c.set_enabled(true);
    CHECK_FALSE(c.state("trigger").active); // Disabling canceled the original, not its copy.
    CHECK(snapshot.state("trigger").active);
    snapshot.rebind("trigger", {{{i::ControlKind::key, 9}}});
    CHECK(snapshot.state("trigger").canceled); // Rebinding cancels the held action.
    CHECK_FALSE(snapshot.state("trigger").active);
    snapshot.process(key(4, true));
    CHECK_FALSE(snapshot.state("trigger").active); // The old binding no longer applies.
    snapshot.process(key(9, true));
    CHECK(snapshot.state("trigger").active);
    CHECK_THROWS_WITH_AS(snapshot.rebind("trigger", {{{i::ControlKind::key, 512}}}), code_range, std::invalid_argument);
    CHECK(snapshot.state("trigger").active); // The rejected rebind changed nothing.
    CHECK(snapshot.actions()[0].bindings[0].control.code == 9);
}

TEST_CASE("A paired gamepad axis applies its deadzone and signed bindings until the gamepad disconnects") {
    i::Context analog(
        {{"steer", i::ActionType::axis, {{{i::ControlKind::gamepad_axis, 0, 7}, i::Channel::x, 1, .2F}}},
         {"negative", i::ActionType::button, {{{i::ControlKind::gamepad_axis, 0, 7}, i::Channel::x, -1}}}});
    analog.process({i::EventType::control, {i::ControlKind::gamepad_axis, 0, 8}, 1});
    CHECK(analog.state("steer").value.x == 0); // Another gamepad does not affect the paired one.
    analog.process({i::EventType::control, {i::ControlKind::gamepad_axis, 0, 7}, .1F});
    CHECK(analog.state("steer").value.x == 0); // The deadzone suppresses drift.
    analog.process({i::EventType::control, {i::ControlKind::gamepad_axis, 0, 7}, .6F});
    // Magnitudes beyond the deadzone are remapped to [0, 1].
    CHECK(analog.state("steer").value.x == Near{.5F, tolerance});
    analog.process({i::EventType::control, {i::ControlKind::gamepad_axis, 0, 7}, -1});
    CHECK(analog.state("negative").active);
    CHECK(analog.state("steer").value.x == -1);
    analog.process({i::EventType::disconnect, {i::ControlKind::gamepad_button, 0, 7}});
    CHECK_FALSE(analog.state("negative").active);
    CHECK(analog.state("steer").value.x == 0);
}

TEST_CASE("Invalid events, unknown actions and invalid maps are rejected") {
    i::Context c(movement_map());
    CHECK_THROWS_WITH_AS(c.process({i::EventType::control, {i::ControlKind::key, 4, i::any_device}, 1}),
                         "Input events require a concrete device ID", std::invalid_argument);
    CHECK_THROWS_WITH_AS(c.process({i::EventType::control, {i::ControlKind::key, 4, 0}, .5F}),
                         "Invalid input control value", std::invalid_argument);
    CHECK_THROWS_WITH_AS(c.process({i::EventType::focus, {}, std::numeric_limits<float>::quiet_NaN()}),
                         "Input focus must be 0 or 1", std::invalid_argument);
    CHECK_THROWS_WITH_AS(c.state("absent"), "Unknown input action", std::out_of_range);
    CHECK_THROWS_WITH_AS(i::Context({{"same", i::ActionType::button, {}}, {"same", i::ActionType::button, {}}}),
                         "Duplicate input action name", std::invalid_argument);
    CHECK_THROWS_WITH_AS(i::Context({{"bad name", i::ActionType::button, {}}}),
                         "Input action names must be printable ASCII without spaces", std::invalid_argument);
    CHECK_THROWS_WITH_AS(i::Context({{"bad", i::ActionType::button, {{{}, i::Channel::y}}}}), binding_error,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(i::Context({{"bad", i::ActionType::button, {{{}, i::Channel::x, 1, 1}}}}), binding_error,
                         std::invalid_argument);
}

TEST_CASE("A control beyond the recorded-control capacity is rejected without being recorded") {
    i::Context capacity({{"held", i::ActionType::button, {{{i::ControlKind::gamepad_button, 0}}}}});
    for (unsigned device = 0; device < control_capacity; ++device)
        capacity.process({i::EventType::control, {i::ControlKind::gamepad_button, 0, device}, 1});
    CHECK_THROWS_WITH_AS(
        capacity.process({i::EventType::control, {i::ControlKind::gamepad_button, 0, control_capacity}, 1}),
        over_capacity, std::length_error);
    for (unsigned device = 0; device < control_capacity; ++device)
        capacity.process({i::EventType::disconnect, {i::ControlKind::gamepad_button, 0, device}});
    CHECK_FALSE(capacity.state("held").active); // The rejected gamepad was never recorded.
}

TEST_CASE("A chord's modifiers gate its primary in either order") {
    const auto binding = chord({i::ControlKind::key, 4}, {{i::ControlKind::key, 224}});
    i::Context context({{"chord", i::ActionType::button, {binding}}});
    context.process(key(4, true));
    CHECK_FALSE(context.state("chord").active); // The primary alone does not complete the chord.
    context.process(key(224, true));
    CHECK(context.state("chord").pressed); // A modifier pressed after the primary completes it.
    context.begin_frame();
    context.process(key(224, false));
    auto state = context.state("chord");
    // Releasing the modifier releases the chord without canceling it.
    CHECK_FALSE(state.active);
    CHECK(state.released);
    CHECK_FALSE(state.pressed);
    CHECK_FALSE(state.canceled);
    context.process(key(224, true));
    state = context.state("chord");
    // The release and the repress both stay latched until the next frame.
    CHECK(state.active);
    CHECK(state.pressed);
    CHECK(state.released);
    CHECK_FALSE(state.canceled);
    context.begin_frame();
    context.process(key(4, true));
    context.process(key(224, true));
    CHECK(context.state("chord").active); // Repeated primary and modifier events do not press again.
    CHECK_FALSE(context.state("chord").pressed);
    context.process(key(4, false));
    context.begin_frame();
    context.process(key(4, true));
    CHECK(context.state("chord").pressed); // A held modifier gates a newly pressed primary.
    i::Context copy = context;
    context.set_focused(false);
    state = context.state("chord");
    // Losing focus cancels the chord in this context only.
    CHECK(state.canceled);
    CHECK(state.released);
    CHECK_FALSE(state.pressed);
    CHECK(copy.state("chord").active);
    context.process(key(224, true));
    context.set_focused(true);
    context.begin_frame();
    context.process(key(4, true));
    CHECK_FALSE(context.state("chord").active); // Regaining focus replays no held modifier.
    context.process(key(224, true));
    context.set_enabled(false);
    context.process(key(224, true));
    context.set_enabled(true);
    context.begin_frame();
    context.process(key(4, true));
    CHECK_FALSE(context.state("chord").active); // Nor does enabling the context again.
    context.process(key(224, true));
    const auto replacement = chord({i::ControlKind::key, 5}, {{i::ControlKind::key, 225}});
    context.rebind("chord", {replacement});
    CHECK(context.state("chord").canceled); // Rebinding drops the prior physical state.
    CHECK_FALSE(context.state("chord").active);
    context.begin_frame();
    context.process(key(5, true));
    context.process(key(224, true));
    CHECK_FALSE(context.state("chord").active); // The old modifier no longer applies.
    context.process(key(225, true));
    CHECK(context.state("chord").pressed);
    CHECK(copy.state("chord").active); // The copy keeps its own bindings.
    CHECK(copy.actions()[0].bindings[0].modifiers[0].code == 224);
}

TEST_CASE("A held chord outranks bindings of its primary that require only some of its modifiers") {
    constexpr unsigned s = 22, ctrl = 224, shift = 225;
    const i::Binding back{{i::ControlKind::key, s}};
    const auto save = chord({i::ControlKind::key, s}, {{i::ControlKind::key, ctrl}});
    const auto shout = chord({i::ControlKind::key, s}, {{i::ControlKind::key, shift}});
    const auto save_as = chord({i::ControlKind::key, s}, {{i::ControlKind::key, ctrl}, {i::ControlKind::key, shift}});
    i::Context shortcuts({{"back", i::ActionType::button, {back}},
                          {"save", i::ActionType::button, {save}},
                          {"quick_save", i::ActionType::button, {save}},
                          {"shout", i::ActionType::button, {shout}},
                          {"save_as", i::ActionType::button, {save_as}},
                          {"crouch", i::ActionType::button, {{{i::ControlKind::key, ctrl}}}}});
    shortcuts.process(key(s, true));
    CHECK(shortcuts.state("back").pressed);
    shortcuts.begin_frame();
    shortcuts.process(key(ctrl, true));
    // Ctrl+S outranks S, so holding Ctrl releases the plain binding without canceling it.
    CHECK(shortcuts.state("save").pressed);
    CHECK_FALSE(shortcuts.state("back").active);
    CHECK(shortcuts.state("back").released);
    CHECK_FALSE(shortcuts.state("back").canceled);
    CHECK(shortcuts.state("quick_save").active); // Equal modifier sets apply together.
    CHECK(shortcuts.state("crouch").active);     // No modifier is consumed.
    shortcuts.process(key(shift, true));
    // Ctrl+Shift+S outranks both Ctrl+S and Shift+S.
    CHECK(shortcuts.state("save_as").pressed);
    CHECK_FALSE(shortcuts.state("save").active);
    CHECK_FALSE(shortcuts.state("quick_save").active);
    CHECK_FALSE(shortcuts.state("shout").active);
    CHECK(shortcuts.state("crouch").active);
    shortcuts.process(key(ctrl, false));
    // Shift+S takes over, and still outranks S.
    CHECK_FALSE(shortcuts.state("save_as").active);
    CHECK(shortcuts.state("shout").pressed);
    CHECK_FALSE(shortcuts.state("back").active);
    shortcuts.begin_frame();
    shortcuts.process(key(shift, false));
    CHECK(shortcuts.state("shout").released);
    CHECK(shortcuts.state("back").pressed); // Releasing the last modifier restores the plain binding.

    // Without Ctrl+Shift+S, the partly shared chords Ctrl+S and Shift+S outrank S but not each other.
    i::Context partial({{"back", i::ActionType::button, {back}},
                        {"save", i::ActionType::button, {save}},
                        {"shout", i::ActionType::button, {shout}}});
    partial.process(key(ctrl, true));
    partial.process(key(shift, true));
    partial.process(key(s, true));
    CHECK(partial.state("save").active);
    CHECK(partial.state("shout").active);
    CHECK_FALSE(partial.state("back").active);
}

TEST_CASE("Chord precedence applies per device and per context, across device classes") {
    i::Context keyboards(
        {{"back", i::ActionType::button, {{{i::ControlKind::key, 22}}}},
         {"save", i::ActionType::button, {chord({i::ControlKind::key, 22}, {{i::ControlKind::key, 224}})}}});
    keyboards.process(key(224, true, 1));
    keyboards.process(key(22, true, 2));
    // Keyboard 1's Ctrl neither completes nor outranks keyboard 2's S.
    CHECK(keyboards.state("back").active);
    CHECK_FALSE(keyboards.state("save").active);
    keyboards.process(key(22, true, 1));
    CHECK(keyboards.state("save").active);
    CHECK(keyboards.state("back").active); // Keyboard 2's S still drives the plain binding.
    keyboards.process(key(22, false, 2));
    CHECK_FALSE(keyboards.state("back").active); // Keyboard 1's S is outranked.
    i::Context separate({{"back", i::ActionType::button, {{{i::ControlKind::key, 22}}}}});
    separate.process(key(22, true, 1));
    CHECK(separate.state("back").active); // Chords of another context outrank nothing here.

    // A shoulder button switches its own gamepad's stick from steering to aiming.
    const auto aim = chord({i::ControlKind::gamepad_axis, 0}, {{i::ControlKind::gamepad_button, 9}});
    i::Context layers(
        {{"steer", i::ActionType::axis, {{{i::ControlKind::gamepad_axis, 0}}}}, {"aim", i::ActionType::axis, {aim}}});
    layers.process(event(i::ControlKind::gamepad_axis, 0, 5, .75F));
    layers.process(event(i::ControlKind::gamepad_button, 9, 6));
    CHECK(layers.state("steer").value.x == .75F); // Gamepad 6's shoulder does not outrank gamepad 5's stick.
    CHECK(layers.state("aim").value.x == 0);
    layers.process(event(i::ControlKind::gamepad_button, 9, 5));
    CHECK(layers.state("steer").value.x == 0);
    CHECK(layers.state("aim").value.x == .75F);
    layers.process(event(i::ControlKind::gamepad_button, 9, 5, 0));
    CHECK(layers.state("steer").value.x == .75F);

    // A keyboard modifier outranks the unmodified binding of a mouse button.
    i::Context mixed(
        {{"fire", i::ActionType::button, {{{i::ControlKind::mouse_button, 1}}}},
         {"inspect", i::ActionType::button, {chord({i::ControlKind::mouse_button, 1}, {{i::ControlKind::key, 224}})}}});
    mixed.process(event(i::ControlKind::mouse_button, 1, 0));
    CHECK(mixed.state("fire").active);
    mixed.process(key(224, true, 3));
    CHECK(mixed.state("inspect").active);
    CHECK_FALSE(mixed.state("fire").active);
}

TEST_CASE("A chord takes its primary and modifiers from one keyboard") {
    i::Context keyboard(
        {{"chord", i::ActionType::button, {chord({i::ControlKind::key, 4}, {{i::ControlKind::key, 224}})}}});
    keyboard.process(key(4, true, 1));
    keyboard.process(key(224, true, 2));
    CHECK_FALSE(keyboard.state("chord").active); // Keys of different keyboards do not combine.
    keyboard.process(key(4, true, 2));
    CHECK(keyboard.state("chord").active);
    keyboard.process({i::EventType::disconnect, {i::ControlKind::key, 0, 2}});
    // Removing the only complete keyboard releases the chord without canceling it.
    CHECK_FALSE(keyboard.state("chord").active);
    CHECK(keyboard.state("chord").released);
    CHECK_FALSE(keyboard.state("chord").canceled);
    keyboard.process(key(224, true, 1));
    CHECK(keyboard.state("chord").active); // The other keyboard kept its primary.
}

TEST_CASE("Each device class of a chord selects its own device") {
    const auto mixed = chord({i::ControlKind::key, 4, 3}, {{i::ControlKind::mouse_button, 1},
                                                           {i::ControlKind::mouse_button, 2},
                                                           {i::ControlKind::gamepad_button, 0},
                                                           {i::ControlKind::gamepad_button, 1}});
    i::Context classes({{"chord", i::ActionType::button, {mixed}}});
    classes.process(key(4, true, 3));
    classes.process(event(i::ControlKind::mouse_button, 1, 8));
    classes.process(event(i::ControlKind::mouse_button, 2, 9));
    classes.process(event(i::ControlKind::gamepad_button, 0, 7));
    classes.process(event(i::ControlKind::gamepad_button, 1, 6));
    CHECK_FALSE(classes.state("chord").active); // No class has its modifiers on one device.
    classes.process(event(i::ControlKind::mouse_button, 2, 8));
    CHECK_FALSE(classes.state("chord").active); // The mouse modifiers are complete, the gamepad ones are not.
    classes.process(event(i::ControlKind::gamepad_button, 1, 7));
    CHECK(classes.state("chord").active); // The classes need not share a device ID.
    classes.process(event(i::ControlKind::mouse_button, 1, 8, 0));
    CHECK(classes.state("chord").released); // Releasing a modifier of any class releases the chord.
    CHECK_FALSE(classes.state("chord").canceled);
}

TEST_CASE("A keyboard fixed by one key modifier applies to every key modifier of a chord") {
    i::Context secondary(
        {{"chord",
          i::ActionType::button,
          {chord({i::ControlKind::mouse_button, 1}, {{i::ControlKind::key, 224}, {i::ControlKind::key, 225, 7}})}}});
    secondary.process(event(i::ControlKind::mouse_button, 1, 3));
    secondary.process(key(225, true, 7));
    // More keyboards than a context records; the chord observes none of them, since 225 selects keyboard 7.
    for (unsigned device = 100; device < 1200; ++device)
        secondary.process(key(224, true, device));
    CHECK_FALSE(secondary.state("chord").active);
    secondary.process(key(224, true, 7));
    CHECK(secondary.state("chord").pressed);
}

TEST_CASE("A wildcard chord axis reads the strongest gamepad that holds its modifiers") {
    const auto axis = chord({i::ControlKind::gamepad_axis, 0}, {{i::ControlKind::gamepad_button, 0}});
    i::Context analog({{"steer", i::ActionType::axis, {axis}}});
    analog.process(event(i::ControlKind::gamepad_axis, 0, 1, .9F));
    analog.process(event(i::ControlKind::gamepad_axis, 0, 2, -.6F));
    analog.process(event(i::ControlKind::gamepad_button, 0, 2));
    CHECK(analog.state("steer").value.x == -.6F); // Only gamepad 2 holds the modifier.
    analog.process(event(i::ControlKind::gamepad_button, 0, 1));
    CHECK(analog.state("steer").value.x == .9F); // The stronger eligible axis wins.
    analog.process(event(i::ControlKind::gamepad_axis, 0, 1, .6F));
    CHECK(analog.state("steer").value.x == .6F); // Equal magnitudes choose the lowest device ID.
    analog.begin_frame();
    analog.process({i::EventType::disconnect, {i::ControlKind::gamepad_axis, 0, 1}});
    // A disconnect falls back to the other complete gamepad without an edge or cancellation.
    CHECK(analog.state("steer").value.x == -.6F);
    CHECK_FALSE(analog.state("steer").released);
    CHECK_FALSE(analog.state("steer").pressed);
    CHECK_FALSE(analog.state("steer").canceled);
    analog.process(event(i::ControlKind::gamepad_button, 0, 2, 0));
    CHECK(analog.state("steer").released); // Releasing the modifier zeroes the axis.
    CHECK(analog.state("steer").value.x == 0);
}

TEST_CASE("A fixed modifier selector limits a wildcard chord axis to its gamepad") {
    auto axis = chord({i::ControlKind::gamepad_axis, 0}, {{i::ControlKind::gamepad_button, 0}});
    axis.modifiers[0].device = 7;
    i::Context paired({{"steer", i::ActionType::axis, {axis}}});
    // More gamepads than a context records; the chord observes none of them.
    for (unsigned device = 100; device < 1200; ++device)
        paired.process(event(i::ControlKind::gamepad_axis, 0, device));
    paired.process(event(i::ControlKind::gamepad_axis, 0, 7, .75F));
    paired.process(event(i::ControlKind::gamepad_button, 0, 7));
    CHECK(paired.state("steer").value.x == .75F);
}

TEST_CASE("A binding's identity selects the devices reporting it, under whatever IDs they have") {
    const auto pad = model_identity(1), other_pad = model_identity(2);
    i::Binding jump{{i::ControlKind::gamepad_button, 0}};
    jump.control.identity = pad;
    i::Binding steer{{i::ControlKind::gamepad_axis, 0}};
    steer.control.identity = pad;
    i::Context identified({{"jump", i::ActionType::button, {jump}}, {"steer", i::ActionType::axis, {steer}}});
    identified.process(reported(i::ControlKind::gamepad_button, 0, 4, other_pad));
    CHECK_FALSE(identified.state("jump").active); // A device reporting another identity does not match,
    identified.process(event(i::ControlKind::gamepad_button, 0, 5));
    CHECK_FALSE(identified.state("jump").active); // nor does one reporting none.
    identified.process(reported(i::ControlKind::gamepad_button, 0, 6, pad));
    CHECK(identified.state("jump").pressed);
    identified.begin_frame();
    identified.process({i::EventType::disconnect, {i::ControlKind::gamepad_button, 0, 6}});
    CHECK(identified.state("jump").released);
    identified.process(reported(i::ControlKind::gamepad_button, 0, 9, pad));
    CHECK(identified.state("jump").pressed); // The reconnected device matches under its new ID.
    identified.process(event(i::ControlKind::gamepad_button, 0, 9, 0));
    CHECK_FALSE(identified.state("jump").active); // A release without the identity still releases the press.
    // Identical devices share an identity, and the binding chooses among them as a wildcard does.
    identified.process(reported(i::ControlKind::gamepad_axis, 0, 3, pad, .4F));
    identified.process(reported(i::ControlKind::gamepad_axis, 0, 7, other_pad, .9F));
    identified.process(reported(i::ControlKind::gamepad_axis, 0, 8, pad, -.6F));
    CHECK(identified.state("steer").value.x == -.6F);
    identified.process(reported(i::ControlKind::gamepad_axis, 0, 8, pad, .4F));
    CHECK(identified.state("steer").value.x == .4F);
    // An update reporting another identity replaces the record, which the binding then rejects.
    identified.process(reported(i::ControlKind::gamepad_axis, 0, 3, other_pad, .4F));
    identified.process(reported(i::ControlKind::gamepad_axis, 0, 8, other_pad, .4F));
    CHECK(identified.state("steer").value.x == 0);
}

TEST_CASE("An identity on any control of a class selects that class's device, and identities must agree") {
    const auto pad = model_identity(1), other_pad = model_identity(2), keyboard = model_identity(3),
               mouse = model_identity(4);
    auto aim = chord({i::ControlKind::gamepad_axis, 0}, {{i::ControlKind::gamepad_button, 9}});
    aim.modifiers[0].identity = pad;
    i::Context layers(
        {{"steer", i::ActionType::axis, {{{i::ControlKind::gamepad_axis, 0}}}}, {"aim", i::ActionType::axis, {aim}}});
    layers.process(reported(i::ControlKind::gamepad_axis, 0, 3, other_pad, .75F));
    layers.process(reported(i::ControlKind::gamepad_button, 9, 3, other_pad));
    // The modifier's identity also applies to the stick, so gamepad 3 neither aims nor outranks its steering.
    CHECK(layers.state("aim").value.x == 0);
    CHECK(layers.state("steer").value.x == .75F);
    layers.process(reported(i::ControlKind::gamepad_axis, 0, 4, pad, -.5F));
    CHECK(layers.state("aim").value.x == 0); // Gamepad 4 has not pressed the modifier yet.
    layers.process(reported(i::ControlKind::gamepad_button, 9, 4, pad));
    CHECK(layers.state("aim").value.x == -.5F);
    CHECK(layers.state("steer").value.x == .75F); // Gamepad 4's stick now aims; gamepad 3's still steers.

    // An explicit ID and an identity both apply, and each class takes its own identity.
    i::Binding pinned{{i::ControlKind::mouse_button, 1, 2, mouse}};
    pinned.modifiers = {{i::ControlKind::key, 224, i::any_device, keyboard}};
    i::Context mixed({{"pinned", i::ActionType::button, {pinned}}});
    mixed.process(reported(i::ControlKind::key, 224, 0, keyboard));
    mixed.process(reported(i::ControlKind::mouse_button, 1, 2, pad));
    CHECK_FALSE(mixed.state("pinned").active);
    mixed.process(reported(i::ControlKind::mouse_button, 1, 1, mouse));
    CHECK_FALSE(mixed.state("pinned").active);
    mixed.process(reported(i::ControlKind::mouse_button, 1, 2, mouse));
    CHECK(mixed.state("pinned").active);

    auto conflicting = aim;
    conflicting.control.identity = other_pad;
    CHECK_THROWS_WITH_AS(i::Context({{"aim", i::ActionType::axis, {conflicting}}}), conflict, std::invalid_argument);
    CHECK_THROWS_WITH_AS(layers.rebind("aim", {conflicting}), conflict, std::invalid_argument);
    CHECK(layers.state("aim").value.x == -.5F); // The rejected rebind changed nothing.
}

TEST_CASE("Events whose identity no binding accepts are not recorded") {
    const auto pad = model_identity(1), other_pad = model_identity(2);
    i::Binding jump{{i::ControlKind::gamepad_button, 0}};
    jump.control.identity = pad;
    i::Context identified({{"jump", i::ActionType::button, {jump}}});
    // More gamepads than a context records; the binding observes none of them.
    for (unsigned device = 0; device < control_capacity + 1; ++device)
        identified.process(reported(i::ControlKind::gamepad_button, 0, device, other_pad));
    identified.process(reported(i::ControlKind::gamepad_button, 0, control_capacity + 1, pad));
    CHECK(identified.state("jump").pressed);
}

TEST_CASE("A recorded control that changes to an identity no binding accepts is released and not recorded") {
    const auto pad = model_identity(1), other_pad = model_identity(2);
    i::Binding jump{{i::ControlKind::gamepad_button, 0}};
    jump.control.identity = pad;
    i::Context identified({{"jump", i::ActionType::button, {jump}}});
    for (unsigned device = 0; device < control_capacity; ++device)
        identified.process(reported(i::ControlKind::gamepad_button, 0, device, pad));
    CHECK(identified.state("jump").active);
    identified.begin_frame();
    for (unsigned device = 0; device < control_capacity; ++device)
        identified.process(reported(i::ControlKind::gamepad_button, 0, device, other_pad));
    CHECK_FALSE(identified.state("jump").active);
    CHECK(identified.state("jump").released);
    // The released controls leave room for another accepted pad.
    identified.begin_frame();
    identified.process(reported(i::ControlKind::gamepad_button, 0, control_capacity, pad));
    CHECK(identified.state("jump").pressed);
}

TEST_CASE("Invalid chords are rejected by construction and rebinding without changing the context") {
    const auto valid = chord({i::ControlKind::key, 4, 1}, {{i::ControlKind::key, 224, 1}});
    i::Context context({{"chord", i::ActionType::button, {valid}}});
    context.process(key(224, true, 1));
    context.process(key(4, true, 1));
    struct Invalid {
        std::vector<i::Control> modifiers;
        const char *error;
    };
    const std::vector<Invalid> invalids{
        {{{i::ControlKind::key, 225},
          {i::ControlKind::key, 226},
          {i::ControlKind::key, 227},
          {i::ControlKind::key, 228},
          {i::ControlKind::key, 229}},
         "Input binding exceeds four modifiers"},
        {{{i::ControlKind::gamepad_axis, 0}}, "Input chord modifiers must be digital"},
        {{{static_cast<i::ControlKind>(99), 0}}, "Unknown input control kind"},
        {{{i::ControlKind::key, 512}}, code_range},
        {{{i::ControlKind::mouse_button, 0}}, code_range},
        {{{i::ControlKind::key, 224}, {i::ControlKind::key, 224, 1}}, repeated},
        {{{i::ControlKind::key, 4}}, repeated},
        {{{i::ControlKind::key, 224, 2}}, conflict},
        {{{i::ControlKind::gamepad_button, 0, 7}, {i::ControlKind::gamepad_button, 1, 8}}, conflict},
    };
    for (std::size_t index = 0; index < invalids.size(); ++index) {
        CAPTURE(index);
        const auto &invalid = invalids[index];
        auto binding = valid;
        binding.modifiers = invalid.modifiers;
        CHECK_THROWS_WITH_AS(i::Context({{"chord", i::ActionType::button, {binding}}}), invalid.error,
                             std::invalid_argument);
        CHECK_THROWS_WITH_AS(context.rebind("chord", {binding}), invalid.error, std::invalid_argument);
        // Neither the configuration nor the recorded controls or edge latches changed.
        const auto state = context.state("chord");
        CHECK(state.active);
        CHECK(state.pressed);
        CHECK_FALSE(state.released);
        CHECK_FALSE(state.canceled);
        CHECK(context.actions()[0].bindings[0].modifiers == valid.modifiers);
    }
    context.process(key(224, false, 1));
    CHECK(context.state("chord").released); // The accepted modifier is still recorded.
}

TEST_CASE("A chord modifier beyond the recorded-control capacity is rejected without being recorded") {
    i::Context capacity(
        {{"chord", i::ActionType::button, {chord({i::ControlKind::key, 4}, {{i::ControlKind::key, 224}})}}});
    for (unsigned device = 0; device < control_capacity; ++device)
        capacity.process(key(4, true, device));
    const auto last = control_capacity - 1;
    CHECK_THROWS_WITH_AS(capacity.process(key(224, true, last)), over_capacity, std::length_error);
    CHECK_FALSE(capacity.state("chord").active);
    CHECK_FALSE(capacity.state("chord").pressed);
    capacity.process(key(4, false, 0));
    capacity.process(key(4, true, last));
    CHECK_FALSE(capacity.state("chord").active); // The rejected modifier was not recorded.
    capacity.process(key(224, true, last));
    CHECK(capacity.state("chord").pressed); // Freeing one recorded control admits it.
    for (unsigned device = 0; device < control_capacity; ++device)
        capacity.process({i::EventType::disconnect, {i::ControlKind::key, 0, device}});
    CHECK_FALSE(capacity.state("chord").active);
    CHECK(capacity.state("chord").released);
}

TEST_CASE("Increments sum within a frame, scale without clamping, and reset at begin_frame") {
    i::Context c({{"look",
                   i::ActionType::vector2,
                   {delta(i::ControlKind::mouse_motion, x_code, i::Channel::x, .5F),
                    delta(i::ControlKind::mouse_motion, y_code, i::Channel::y, -.5F)}},
                  {"zoom", i::ActionType::axis, {delta(i::ControlKind::mouse_wheel, y_code)}},
                  {"inverted", i::ActionType::axis, {delta(i::ControlKind::mouse_wheel, y_code, i::Channel::x, -1)}}});
    c.process(motion(x_code, 40));
    CHECK(c.state("look").value.x == 20); // 40 with scale 0.5 reads 20, unclamped.
    CHECK(c.state("look").pressed);
    c.process(motion(x_code, 10));
    c.process(motion(y_code, 8));
    auto look = c.state("look");
    // The increments sum, and the negative scale inverts y; the vector is not normalized.
    CHECK(look.value.x == 25);
    CHECK(look.value.y == -4);
    CHECK(look.active);
    c.process(wheel(y_code, 1.5F));
    c.process(wheel(y_code, -.25F));
    CHECK(c.state("zoom").value.x == 1.25F);
    CHECK(c.state("inverted").value.x == -1.25F);
    c.begin_frame();
    c.process(motion(x_code, 0)); // A zero increment changes nothing.
    // The new frame starts every sum at zero, and the actions that increments kept active release in it.
    for (const auto *name : {"look", "zoom", "inverted"}) {
        CAPTURE(name);
        const auto state = c.state(name);
        CHECK(state.value.x == 0);
        CHECK(state.value.y == 0);
        CHECK_FALSE(state.active);
        CHECK(state.released);
        CHECK_FALSE(state.canceled);
    }
    c.process(motion(x_code, -6));
    CHECK(c.state("look").value.x == -3);
    c.begin_frame();
    c.begin_frame();
    CHECK(unlatched(c.state("look"))); // A frame without increments leaves nothing to release.
    // A value too large for any level passes through unchanged.
    c.process(wheel(y_code, 1e30F));
    CHECK(c.state("zoom").value.x == 1e30F);
}

TEST_CASE("A wheel-driven button presses in one frame and releases in the next") {
    i::Context c(
        {{"next", i::ActionType::button, {delta(i::ControlKind::mouse_wheel, y_code)}},
         {"previous", i::ActionType::button, {delta(i::ControlKind::mouse_wheel, y_code, i::Channel::x, -1)}},
         // A button may mix held and delta controls.
         {"either", i::ActionType::button, {{{i::ControlKind::key, 4}}, delta(i::ControlKind::mouse_wheel, y_code)}}});
    c.process(wheel(y_code, .25F));
    CHECK_FALSE(c.state("next").active); // Below the threshold,
    c.process(wheel(y_code, .25F));
    auto next = c.state("next");
    CHECK(next.active); // until the sum reaches it.
    CHECK(next.pressed);
    CHECK(next.value.x == 1);
    CHECK_FALSE(c.state("previous").active); // A button takes only positive contributions.
    CHECK(c.state("either").active);
    c.begin_frame();
    next = c.state("next");
    CHECK_FALSE(next.active);
    CHECK(next.released);
    CHECK_FALSE(next.pressed);
    CHECK_FALSE(next.canceled);
    CHECK(next.value.x == 0);
    c.begin_frame();
    CHECK(unlatched(c.state("next")));
    c.process(wheel(y_code, -3));
    CHECK(c.state("previous").pressed);
    c.process(key(4, true));
    c.begin_frame();
    // The held key keeps the mixed button active through the frame boundary, with no edge.
    CHECK(c.state("either").active);
    CHECK(unlatched(c.state("either")));
    CHECK(c.state("previous").released);
}

TEST_CASE("A chord gates each increment as it arrives, and precedence applies then") {
    auto zoom = delta(i::ControlKind::mouse_wheel, y_code);
    zoom.modifiers = {{i::ControlKind::key, left_ctrl}};
    auto aim = delta(i::ControlKind::mouse_motion, x_code);
    aim.modifiers = {{i::ControlKind::mouse_button, right_button}};
    i::Context c({{"scroll", i::ActionType::axis, {delta(i::ControlKind::mouse_wheel, y_code)}},
                  {"zoom", i::ActionType::axis, {zoom}},
                  {"aim", i::ActionType::axis, {aim}}});
    c.process(wheel(y_code, 1));
    c.process(key(left_ctrl, true));
    // Ctrl pressed after the increment in the same frame does not claim it, nor take it from the plain binding.
    CHECK(c.state("zoom").value.x == 0);
    CHECK(c.state("scroll").value.x == 1);
    c.process(wheel(y_code, 2));
    // While Ctrl is held, Ctrl + wheel outranks the plain wheel binding.
    CHECK(c.state("zoom").value.x == 2);
    CHECK(c.state("scroll").value.x == 1);
    c.process(key(left_ctrl, false));
    CHECK(c.state("zoom").value.x == 2); // Releasing Ctrl keeps what the chord accepted this frame.
    c.process(wheel(y_code, 4));
    CHECK(c.state("scroll").value.x == 5);
    CHECK(c.state("zoom").value.x == 2);
    // Motion counts toward right button + motion only while the button is held on the motion's mouse.
    c.process(motion(x_code, 3));
    c.process(event(i::ControlKind::mouse_button, right_button, 1));
    c.process(motion(x_code, 5));
    CHECK(c.state("aim").value.x == 0);
    c.process(event(i::ControlKind::mouse_button, right_button, 0));
    c.process(motion(x_code, 7));
    CHECK(c.state("aim").value.x == 7);
}

TEST_CASE("Focus loss, cancellation, disabling, rebinding and a mouse disconnect drop increments") {
    const auto look = [] {
        return i::Context({{"look", i::ActionType::axis, {delta(i::ControlKind::mouse_motion, x_code)}}});
    };
    const auto dropped = [](const i::Context &context, const char *step) {
        CAPTURE(step);
        const auto state = context.state("look");
        CHECK(state.value.x == 0);
        CHECK_FALSE(state.active);
        CHECK(state.released);
    };
    auto focus = look();
    focus.process(motion(x_code, 4));
    focus.process({i::EventType::focus, {}, 0});
    dropped(focus, "focus loss");
    CHECK(focus.state("look").canceled);
    focus.process(motion(x_code, 4));
    focus.process({i::EventType::focus, {}, 1});
    CHECK(focus.state("look").value.x == 0); // Increments while unfocused are ignored.
    auto canceled = look();
    canceled.process(motion(x_code, 4));
    canceled.cancel();
    dropped(canceled, "cancel");
    canceled.process(motion(x_code, 2));
    CHECK(canceled.state("look").value.x == 2); // The next increment starts a new sum.
    auto disabled = look();
    disabled.process(motion(x_code, 4));
    disabled.set_enabled(false);
    dropped(disabled, "disabling");
    auto rebound = look();
    rebound.process(motion(x_code, 4));
    rebound.rebind("look", {delta(i::ControlKind::mouse_motion, x_code, i::Channel::x, 2)});
    dropped(rebound, "rebinding");
    rebound.process(motion(x_code, 4));
    CHECK(rebound.state("look").value.x == 8);
    // A disconnect drops the increments of that mouse only, and a keyboard's none.
    auto mice = look();
    mice.process(motion(x_code, 4, 0));
    mice.process(motion(x_code, 6, 1));
    CHECK(mice.state("look").value.x == 10); // A wildcard binding sums every mouse.
    mice.process({i::EventType::disconnect, {i::ControlKind::key, 0, 1}});
    CHECK(mice.state("look").value.x == 10);
    mice.process({i::EventType::disconnect, {i::ControlKind::mouse_button, 0, 1}});
    CHECK(mice.state("look").value.x == 4);
    CHECK_FALSE(mice.state("look").released);
    mice.process({i::EventType::disconnect, {i::ControlKind::mouse_motion, 0, 0}});
    dropped(mice, "mouse disconnect");
    CHECK_FALSE(mice.state("look").canceled);
    // A binding that selects a device reads only its increments.
    i::Context paired({{"look", i::ActionType::axis, {{{i::ControlKind::mouse_motion, x_code, 1}}}}});
    paired.process(motion(x_code, 4, 0));
    paired.process(motion(x_code, 6, 1));
    CHECK(paired.state("look").value.x == 6);
}

TEST_CASE("Delta modifiers, deadzones, mixed analog actions, codes and non-finite increments are rejected") {
    const auto valid = delta(i::ControlKind::mouse_motion, x_code);
    i::Context context({{"look", i::ActionType::axis, {valid}}});
    context.process(motion(x_code, 3));
    auto motion_modifier = valid, wheel_modifier = valid, deadzone = valid, motion_code = valid, wheel_code = valid;
    motion_modifier.control = {i::ControlKind::mouse_wheel, y_code};
    motion_modifier.modifiers = {{i::ControlKind::mouse_motion, x_code}};
    wheel_modifier.modifiers = {{i::ControlKind::mouse_wheel, y_code}};
    deadzone.deadzone = .1F;
    motion_code.control.code = 2;
    wheel_code.control = {i::ControlKind::mouse_wheel, 2};
    struct Invalid {
        i::ActionType type;
        std::vector<i::Binding> bindings;
        const char *error;
    };
    const std::vector<Invalid> invalids{
        {i::ActionType::axis, {motion_modifier}, digital_modifiers},
        {i::ActionType::axis, {wheel_modifier}, digital_modifiers},
        {i::ActionType::axis, {deadzone}, delta_deadzone},
        {i::ActionType::axis, {valid, {{i::ControlKind::gamepad_axis, 0}}}, mixed_controls},
        {i::ActionType::vector2,
         {{{i::ControlKind::key, 4}}, delta(i::ControlKind::mouse_wheel, y_code)},
         mixed_controls},
        {i::ActionType::axis, {motion_code}, code_range},
        {i::ActionType::axis, {wheel_code}, code_range},
    };
    for (std::size_t index = 0; index < invalids.size(); ++index) {
        CAPTURE(index);
        const auto &invalid = invalids[index];
        CHECK_THROWS_WITH_AS(i::Context({{"look", invalid.type, invalid.bindings}}), invalid.error,
                             std::invalid_argument);
        CHECK_THROWS_WITH_AS(context.rebind("look", invalid.bindings), invalid.error, std::invalid_argument);
        CHECK(context.state("look").value.x == 3); // The rejected rebind kept the sum and the binding.
        CHECK(context.actions()[0].bindings[0].control == valid.control);
    }
    CHECK_THROWS_WITH_AS(context.process(motion(2, 1)), code_range, std::invalid_argument);
    CHECK_THROWS_WITH_AS(context.process(wheel(2, 1)), code_range, std::invalid_argument);
    for (const float amount : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity()}) {
        CAPTURE(amount);
        CHECK_THROWS_WITH_AS(context.process(motion(x_code, amount)), invalid_value, std::invalid_argument);
    }
    CHECK(context.state("look").value.x == 3);
}

TEST_CASE("An increment that would overflow a sum changes nothing, and delta values saturate") {
    constexpr float largest = std::numeric_limits<float>::max();
    i::Context c({{"raw", i::ActionType::axis, {delta(i::ControlKind::mouse_motion, x_code)}},
                  {"fast", i::ActionType::axis, {delta(i::ControlKind::mouse_motion, x_code, i::Channel::x, 16)}}});
    c.process(motion(x_code, largest));
    CHECK(c.state("raw").value.x == largest);
    CHECK(c.state("fast").value.x == largest); // Sixteen times the sum saturates at the largest float.
    c.begin_frame();
    c.begin_frame();
    c.process(motion(x_code, largest));
    CHECK_THROWS_WITH_AS(c.process(motion(x_code, largest)), sum_overflow, std::overflow_error);
    const auto raw = c.state("raw");
    CHECK(raw.value.x == largest);
    CHECK(raw.active);
    CHECK(raw.pressed);
    CHECK_FALSE(raw.released);
    c.process(motion(x_code, -largest));
    // Neither sum took the rejected increment, so the opposite one cancels exactly.
    CHECK(c.state("raw").value.x == 0);
    CHECK(c.state("fast").value.x == 0);
}

TEST_CASE("A delta sum beyond the context's capacity is rejected without being recorded") {
    i::Context c({{"look", i::ActionType::axis, {delta(i::ControlKind::mouse_motion, x_code)}}});
    for (unsigned device = 0; device < sum_capacity; ++device)
        c.process(motion(x_code, 1, device));
    CHECK_THROWS_WITH_AS(c.process(motion(x_code, 1, sum_capacity)), over_sum_capacity, std::length_error);
    CHECK(c.state("look").value.x == float(sum_capacity));
    c.process(motion(x_code, 1, 0)); // An existing sum still takes increments.
    CHECK(c.state("look").value.x == float(sum_capacity + 1));
    c.begin_frame();
    c.process(motion(x_code, 1, sum_capacity)); // The next frame starts with none.
    CHECK(c.state("look").value.x == 1);
}
