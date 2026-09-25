#include <anima/input.hpp>
#include <cmath>
#include <iostream>
#include <limits>
#include <type_traits>
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
        std::cout << "PASS action input transitions, maps, devices, focus and rebinding\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
