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
                         "Input events require a concrete device identity", std::invalid_argument);
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
