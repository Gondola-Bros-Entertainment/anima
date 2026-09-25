#include <anima/input.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <type_traits>
#include <utility>
namespace i = anima::input;
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
    check(caught, "Expected input rejection");
}
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
void chord_transitions() {
    const auto binding = chord({i::ControlKind::key, 4}, {{i::ControlKind::key, 224}});
    i::Context context(
        {{"chord", i::ActionType::button, {binding}}, {"plain", i::ActionType::button, {{{i::ControlKind::key, 4}}}}});
    context.process(key(4, true));
    check(!context.state("chord").active, "Chord ignored its required modifier");
    context.process(key(224, true));
    check(context.state("chord").pressed, "Modifier pressed after primary failed to complete chord");
    check(context.state("plain").pressed && context.state("plain").active,
          "Chord consumed its primary instead of also evaluating an ordinary action");
    context.begin_frame();
    context.process(key(224, false));
    auto state = context.state("chord");
    check(!state.active && state.released && !state.pressed && !state.canceled,
          "Modifier release failed to release a chord or incorrectly canceled it");
    context.process(key(224, true));
    state = context.state("chord");
    check(state.active && state.pressed && state.released && !state.canceled,
          "Modifier release/repress lost between-frame edge latches");
    context.begin_frame();
    context.process(key(4, true));
    context.process(key(224, true));
    check(context.state("chord").active && !context.state("chord").pressed,
          "Repeated primary or modifier events retriggered a held chord");
    context.process(key(4, false));
    context.begin_frame();
    context.process(key(4, true));
    check(context.state("chord").pressed, "Held modifier failed to gate a newly pressed primary");
    i::Context copy = context;
    context.set_focused(false);
    state = context.state("chord");
    check(state.canceled && state.released && !state.pressed && copy.state("chord").active,
          "Chord focus cancellation or context-copy isolation failed");
    context.process(key(224, true));
    context.set_focused(true);
    context.begin_frame();
    context.process(key(4, true));
    check(!context.state("chord").active, "Focus regain replayed held chord modifiers");
    context.process(key(224, true));
    context.set_enabled(false);
    context.process(key(224, true));
    context.set_enabled(true);
    context.begin_frame();
    context.process(key(4, true));
    check(!context.state("chord").active, "Re-enabling replayed held chord modifiers");
    context.process(key(224, true));
    const auto replacement = chord({i::ControlKind::key, 5}, {{i::ControlKind::key, 225}});
    context.rebind("chord", {replacement});
    check(context.state("chord").canceled && !context.state("chord").active,
          "Chord rebind retained prior physical state");
    context.begin_frame();
    context.process(key(5, true));
    context.process(key(224, true));
    check(!context.state("chord").active, "Rebound chord accepted an old modifier");
    context.process(key(225, true));
    check(context.state("chord").pressed && copy.state("chord").active &&
              copy.actions()[0].bindings[0].modifiers[0].code == 224,
          "Chord rebind lost fresh input or changed a copied binding");
}
void chord_devices() {
    i::Context keyboard(
        {{"chord", i::ActionType::button, {chord({i::ControlKind::key, 4}, {{i::ControlKind::key, 224}})}}});
    keyboard.process(key(4, true, 1));
    keyboard.process(key(224, true, 2));
    check(!keyboard.state("chord").active, "Chord combined primary and modifier from different keyboards");
    keyboard.process(key(4, true, 2));
    check(keyboard.state("chord").active, "Chord failed to use a complete second keyboard");
    keyboard.process({i::EventType::disconnect, {i::ControlKind::key, 0, 2}});
    check(!keyboard.state("chord").active && keyboard.state("chord").released && !keyboard.state("chord").canceled,
          "Disconnect failed to release the only complete chord");
    keyboard.process(key(224, true, 1));
    check(keyboard.state("chord").active, "Disconnect cleared another keyboard's retained primary");

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
    check(!classes.state("chord").active, "Chord split a modifier class across devices");
    classes.process(event(i::ControlKind::mouse_button, 2, 8));
    check(!classes.state("chord").active, "Complete mouse modifiers masked an incomplete gamepad chord");
    classes.process(event(i::ControlKind::gamepad_button, 1, 7));
    check(classes.state("chord").active, "Chord incorrectly required equal identities across device classes");
    classes.process(event(i::ControlKind::mouse_button, 1, 8, 0));
    check(classes.state("chord").released && !classes.state("chord").canceled,
          "Cross-class modifier release failed to release the action");

    i::Context secondary(
        {{"chord",
          i::ActionType::button,
          {chord({i::ControlKind::mouse_button, 1}, {{i::ControlKind::key, 224}, {i::ControlKind::key, 225, 7}})}}});
    secondary.process(event(i::ControlKind::mouse_button, 1, 3));
    secondary.process(key(225, true, 7));
    for (unsigned device = 100; device < 1200; ++device)
        secondary.process(key(224, true, device));
    check(!secondary.state("chord").active, "Secondary fixed selector accepted another keyboard's modifier");
    secondary.process(key(224, true, 7));
    check(secondary.state("chord").pressed,
          "Secondary fixed selector incorrectly used the primary's device or observed unrelated keyboards");

    auto axis = chord({i::ControlKind::gamepad_axis, 0}, {{i::ControlKind::gamepad_button, 0}});
    i::Context analog({{"steer", i::ActionType::axis, {axis}}});
    analog.process(event(i::ControlKind::gamepad_axis, 0, 1, .9F));
    analog.process(event(i::ControlKind::gamepad_axis, 0, 2, -.6F));
    analog.process(event(i::ControlKind::gamepad_button, 0, 2));
    check(analog.state("steer").value.x == -.6F,
          "Wildcard axis chose an ineligible stronger device before gating modifiers");
    analog.process(event(i::ControlKind::gamepad_button, 0, 1));
    check(analog.state("steer").value.x == .9F, "Eligible stronger chord axis failed to win");
    analog.process(event(i::ControlKind::gamepad_axis, 0, 1, .6F));
    check(analog.state("steer").value.x == .6F, "Eligible equal-magnitude axes did not choose the lowest device ID");
    analog.begin_frame();
    analog.process({i::EventType::disconnect, {i::ControlKind::gamepad_axis, 0, 1}});
    check(analog.state("steer").value.x == -.6F && !analog.state("steer").released && !analog.state("steer").pressed &&
              !analog.state("steer").canceled,
          "Disconnect canceled a chord instead of falling back to another complete device");
    analog.process(event(i::ControlKind::gamepad_button, 0, 2, 0));
    check(analog.state("steer").released && analog.state("steer").value.x == 0,
          "Modifier release retained a wildcard axis value");
    axis.modifiers[0].device = 7;
    i::Context paired({{"steer", i::ActionType::axis, {axis}}});
    for (unsigned device = 100; device < 1200; ++device)
        paired.process(event(i::ControlKind::gamepad_axis, 0, device));
    paired.process(event(i::ControlKind::gamepad_axis, 0, 7, .75F));
    paired.process(event(i::ControlKind::gamepad_button, 0, 7));
    check(paired.state("steer").value.x == .75F,
          "Fixed modifier pairing observed irrelevant wildcard-primary devices or exhausted physical capacity");
}
void chord_validation_and_capacity() {
    const auto valid = chord({i::ControlKind::key, 4, 1}, {{i::ControlKind::key, 224, 1}});
    i::Context context({{"chord", i::ActionType::button, {valid}}});
    context.process(key(224, true, 1));
    context.process(key(4, true, 1));
    for (const auto &modifiers : std::vector<std::vector<i::Control>>{
             {{i::ControlKind::key, 225},
              {i::ControlKind::key, 226},
              {i::ControlKind::key, 227},
              {i::ControlKind::key, 228},
              {i::ControlKind::key, 229}},
             {{i::ControlKind::gamepad_axis, 0}},
             {{static_cast<i::ControlKind>(99), 0}},
             {{i::ControlKind::key, 512}},
             {{i::ControlKind::mouse_button, 0}},
             {{i::ControlKind::key, 224}, {i::ControlKind::key, 224, 1}},
             {{i::ControlKind::key, 4}},
             {{i::ControlKind::key, 224, 2}},
             {{i::ControlKind::gamepad_button, 0, 7}, {i::ControlKind::gamepad_button, 1, 8}},
         }) {
        auto invalid = valid;
        invalid.modifiers = modifiers;
        rejects([&] { (void)i::Context({{"chord", i::ActionType::button, {invalid}}}); });
        rejects([&] { context.rebind("chord", {invalid}); });
        check(context.state("chord").active && context.state("chord").pressed && !context.state("chord").released &&
                  !context.state("chord").canceled && context.actions()[0].bindings[0].modifiers == valid.modifiers,
              "Invalid chord rebind changed configuration, physical state or edge latches");
    }
    context.process(key(224, false, 1));
    check(context.state("chord").released, "Rejected rebind lost accepted modifier state");

    i::Context capacity(
        {{"chord", i::ActionType::button, {chord({i::ControlKind::key, 4}, {{i::ControlKind::key, 224}})}}});
    for (unsigned device = 0; device < 1024; ++device)
        capacity.process(key(4, true, device));
    rejects([&] { capacity.process(key(224, true, 1023)); });
    check(!capacity.state("chord").active && !capacity.state("chord").pressed,
          "Capacity rejection partially applied a chord modifier");
    capacity.process(key(4, false, 0));
    capacity.process(key(4, true, 1023));
    check(!capacity.state("chord").active, "Rejected modifier remained in observed physical state");
    capacity.process(key(224, true, 1023));
    check(capacity.state("chord").pressed, "Freeing one observed control did not admit a valid chord modifier");
    for (unsigned device = 0; device < 1024; ++device)
        capacity.process({i::EventType::disconnect, {i::ControlKind::key, 0, device}});
    check(!capacity.state("chord").active && capacity.state("chord").released,
          "Disconnect cleanup retained capacity-bound chord state");
}
void run() {
    static_assert(std::is_nothrow_move_assignable_v<i::Context>);
    i::Context c({{"trigger", i::ActionType::button, {{{i::ControlKind::key, 4}}}},
                  {"move",
                   i::ActionType::vector2,
                   {{{i::ControlKind::key, 5}, i::Channel::x},
                    {{i::ControlKind::key, 6}, i::Channel::x, -1},
                    {{i::ControlKind::key, 7}, i::Channel::y}}}});
    c.process(key(4, true));
    c.process(key(4, true));
    c.process(key(4, false));
    auto s = c.state("trigger");
    check(!s.active && s.pressed && s.released && s.value.x == 0, "A between-frame tap was lost");
    c.begin_frame();
    check(!c.state("trigger").pressed && !c.state("trigger").released, "Edges not cleared");
    c.process(key(5, true));
    c.process(key(7, true));
    s = c.state("move");
    check(std::abs(s.value.x - std::sqrt(.5F)) < 1e-6F && s.value.y == s.value.x, "Diagonal movement not normalized");
    c.process(key(6, true));
    check(c.state("move").value.x == 0 && c.state("move").value.y == 1, "Opposing keys did not cancel");
    c.process(key(4, true, 1));
    c.process(key(4, true, 2));
    c.process(key(4, false, 1));
    check(c.state("trigger").active, "Releasing one keyboard cleared another");
    c.process({i::EventType::disconnect, {i::ControlKind::key, 0, 2}});
    check(!c.state("trigger").active && c.state("move").active, "Device removal leaked into other device state");
    c.process(key(4, true));
    c.process({i::EventType::focus, {}, 0});
    check(c.state("trigger").canceled && c.state("trigger").released && !c.state("trigger").pressed,
          "Focus loss did not cancel pending press");
    c.process(key(4, true));
    c.set_focused(true);
    check(!c.state("trigger").active, "Focus regain replayed ignored input");
    c.process(key(4, true));
    c.begin_frame();
    c.process(key(4, true));
    check(!c.state("trigger").pressed && c.state("trigger").active, "Repeat input generated press");
    i::Context snapshot;
    snapshot = c;
    c.set_enabled(false);
    c.process(key(4, true));
    c.set_enabled(true);
    check(!c.state("trigger").active && snapshot.state("trigger").active, "Disable/copy isolation failed");
    snapshot.rebind("trigger", {{{i::ControlKind::key, 9}}});
    check(snapshot.state("trigger").canceled && !snapshot.state("trigger").active, "Rebind retained held action");
    snapshot.process(key(4, true));
    check(!snapshot.state("trigger").active, "Old binding still active");
    snapshot.process(key(9, true));
    check(snapshot.state("trigger").active, "New binding did not activate");
    rejects([&] { snapshot.rebind("trigger", {{{i::ControlKind::key, 512}}}); });
    check(snapshot.state("trigger").active && snapshot.actions()[0].bindings[0].control.code == 9,
          "Invalid rebind mutated accepted state");
    i::Context analog(
        {{"steer", i::ActionType::axis, {{{i::ControlKind::gamepad_axis, 0, 7}, i::Channel::x, 1, .2F}}},
         {"negative", i::ActionType::button, {{{i::ControlKind::gamepad_axis, 0, 7}, i::Channel::x, -1}}}});
    analog.process({i::EventType::control, {i::ControlKind::gamepad_axis, 0, 8}, 1});
    check(analog.state("steer").value.x == 0, "Foreign controller affected a paired context");
    analog.process({i::EventType::control, {i::ControlKind::gamepad_axis, 0, 7}, .1F});
    check(analog.state("steer").value.x == 0, "Deadzone did not suppress drift");
    analog.process({i::EventType::control, {i::ControlKind::gamepad_axis, 0, 7}, .6F});
    check(std::abs(analog.state("steer").value.x - .5F) < 1e-6F, "Deadzone remapping incorrect");
    analog.process({i::EventType::control, {i::ControlKind::gamepad_axis, 0, 7}, -1});
    check(analog.state("negative").active && analog.state("steer").value.x == -1, "Signed trigger binding failed");
    analog.process({i::EventType::disconnect, {i::ControlKind::gamepad_button, 0, 7}});
    check(!analog.state("negative").active && analog.state("steer").value.x == 0, "Gamepad removal retained axes");
    rejects([&] { c.process({i::EventType::control, {i::ControlKind::key, 4, i::any_device}, 1}); });
    rejects([&] { c.process({i::EventType::control, {i::ControlKind::key, 4, 0}, .5F}); });
    rejects([&] { c.process({i::EventType::focus, {}, std::numeric_limits<float>::quiet_NaN()}); });
    rejects([&] { (void)c.state("absent"); });
    rejects([] { i::Context invalid({{"same", i::ActionType::button, {}}, {"same", i::ActionType::button, {}}}); });
    rejects([] { i::Context invalid({{"bad name", i::ActionType::button, {}}}); });
    rejects([] { i::Context invalid({{"bad", i::ActionType::button, {{{}, i::Channel::y}}}}); });
    rejects([] { i::Context invalid({{"bad", i::ActionType::button, {{{}, i::Channel::x, 1, 1}}}}); });
    i::Context capacity({{"held", i::ActionType::button, {{{i::ControlKind::gamepad_button, 0}}}}});
    for (unsigned device = 0; device < 1024; ++device)
        capacity.process({i::EventType::control, {i::ControlKind::gamepad_button, 0, device}, 1});
    rejects([&] { capacity.process({i::EventType::control, {i::ControlKind::gamepad_button, 0, 1024}, 1}); });
    for (unsigned device = 0; device < 1024; ++device)
        capacity.process({i::EventType::disconnect, {i::ControlKind::gamepad_button, 0, device}});
    check(!capacity.state("held").active, "Capacity rejection stored an extra physical control");
}
} // namespace
int main() {
    try {
        run();
        chord_transitions();
        chord_devices();
        chord_validation_and_capacity();
        std::cout << "PASS action input transitions, maps, devices, focus and rebinding\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
