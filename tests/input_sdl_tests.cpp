#include <SDL3/SDL_events.h>
#include <anima/input_sdl.hpp>
#include <doctest/doctest.h>

#include <cstdint>
#include <stdexcept>

namespace i = anima::input;
namespace {
constexpr std::uint32_t window = 8;

// Converts @p event for the input context of the window, which must accept it.
i::Event convert(const SDL_Event &event) {
    const auto converted = i::from_sdl(event, window);
    REQUIRE(converted);
    return *converted;
}
} // namespace

TEST_CASE("Key presses in the target window drive actions, without repeats") {
    i::Context c({{"accept", i::ActionType::button, {{{i::ControlKind::key, SDL_SCANCODE_SPACE}}}}});
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.windowID = window;
    e.key.scancode = SDL_SCANCODE_SPACE;
    e.key.which = 3;
    CHECK_FALSE(i::from_sdl(e, 7)); // Another window's event is filtered out.
    c.process(convert(e));
    CHECK(c.state("accept").pressed);
    e.key.repeat = true;
    CHECK_FALSE(i::from_sdl(e, window));
    e = {};
    e.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    e.window.windowID = window;
    c.process(convert(e));
    CHECK(c.state("accept").canceled); // SDL focus loss cancels held input.
    e.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    c.process(convert(e));
    CHECK(c.focused());
}

TEST_CASE("Gamepad axes reach -1 and 1 exactly, and removing the gamepad disconnects it") {
    i::Context c({{"stick", i::ActionType::axis, {{{i::ControlKind::gamepad_axis, SDL_GAMEPAD_AXIS_LEFTX}}}}});
    SDL_Event e{};
    e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.which = 12;
    e.gaxis.axis = SDL_GAMEPAD_AXIS_LEFTX;
    e.gaxis.value = -32768;
    c.process(convert(e));
    CHECK(c.state("stick").value.x == -1);
    e.gaxis.value = 32767;
    c.process(convert(e));
    CHECK(c.state("stick").value.x == 1);
    e = {};
    e.type = SDL_EVENT_GAMEPAD_REMOVED;
    e.gdevice.which = 12;
    c.process(convert(e));
    CHECK(c.state("stick").value.x == 0);
}

TEST_CASE("Mouse buttons convert, except touch-emulated ones") {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.windowID = window;
    e.button.which = 0;
    e.button.button = SDL_BUTTON_LEFT;
    const auto pressed = convert(e);
    CHECK(pressed.source.kind == i::ControlKind::mouse_button);
    CHECK(pressed.value == 1);
    e.type = SDL_EVENT_MOUSE_BUTTON_UP;
    CHECK(convert(e).value == 0);
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.which = SDL_TOUCH_MOUSEID;
    CHECK_FALSE(i::from_sdl(e, window));
}

TEST_CASE("Text input is not an action input, and a zero target window is rejected") {
    SDL_Event e{};
    e.type = SDL_EVENT_TEXT_INPUT;
    CHECK_FALSE(i::from_sdl(e, window));
    CHECK_THROWS_WITH_AS(i::from_sdl(e, 0), "SDL input conversion requires a nonzero target window ID",
                         std::invalid_argument);
}

// The highest code of each documented ControlKind range converts. One past it, as an unusual device can
// report, is dropped rather than thrown, so the application's event pump keeps running.
TEST_CASE("Codes beyond each control kind's range are dropped") {
    constexpr int highest_key = 511;
    constexpr Uint8 highest_mouse_button = 32;
    constexpr Uint8 highest_gamepad_button = 63;
    constexpr Uint8 highest_gamepad_axis = 15;
    constexpr SDL_JoystickID gamepad = 7;
    const auto converts = [](const SDL_Event &event) { return i::from_sdl(event, window).has_value(); };
    SDL_Event key{};
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.windowID = window;
    key.key.scancode = static_cast<SDL_Scancode>(highest_key);
    CHECK(converts(key));
    key.key.scancode = static_cast<SDL_Scancode>(highest_key + 1);
    CHECK_FALSE(converts(key));
    SDL_Event mouse{};
    mouse.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    mouse.button.windowID = window;
    mouse.button.button = highest_mouse_button;
    CHECK(converts(mouse));
    mouse.button.button = highest_mouse_button + 1;
    CHECK_FALSE(converts(mouse));
    SDL_Event pad{};
    pad.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    pad.gbutton.which = gamepad;
    pad.gbutton.button = highest_gamepad_button;
    CHECK(converts(pad));
    pad.gbutton.button = highest_gamepad_button + 1;
    CHECK_FALSE(converts(pad));
    SDL_Event axis{};
    axis.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    axis.gaxis.which = gamepad;
    axis.gaxis.axis = highest_gamepad_axis;
    CHECK(converts(axis));
    axis.gaxis.axis = highest_gamepad_axis + 1;
    CHECK_FALSE(converts(axis));
}

TEST_CASE("Converted SDL keys drive a chord per keyboard through removal and focus changes") {
    i::Binding save{{i::ControlKind::key, SDL_SCANCODE_S}};
    save.modifiers = {{i::ControlKind::key, SDL_SCANCODE_LCTRL}};
    i::Context shortcuts({{"save", i::ActionType::button, {save}}});
    const auto key = [&](SDL_Scancode code, bool down, SDL_KeyboardID device) {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        event.key.windowID = window;
        event.key.which = device;
        event.key.scancode = code;
        shortcuts.process(convert(event));
    };
    key(SDL_SCANCODE_S, true, 3);
    CHECK_FALSE(shortcuts.state("save").active); // The shortcut needs its modifier.
    key(SDL_SCANCODE_LCTRL, true, 4);
    CHECK_FALSE(shortcuts.state("save").active); // Separate keyboards do not combine.
    key(SDL_SCANCODE_LCTRL, true, 3);
    CHECK(shortcuts.state("save").pressed); // The modifier activates an already held primary.
    shortcuts.begin_frame();
    key(SDL_SCANCODE_LCTRL, false, 3);
    CHECK(shortcuts.state("save").released);
    CHECK_FALSE(shortcuts.state("save").canceled);
    key(SDL_SCANCODE_S, true, 4);
    // The second keyboard completes the chord, and both edges stay latched.
    CHECK(shortcuts.state("save").active);
    CHECK(shortcuts.state("save").pressed);
    CHECK(shortcuts.state("save").released);
    shortcuts.begin_frame();
    SDL_Event event{};
    event.type = SDL_EVENT_KEYBOARD_REMOVED;
    event.kdevice.which = 4;
    shortcuts.process(convert(event));
    CHECK_FALSE(shortcuts.state("save").active); // Removing that keyboard releases its chord.
    CHECK(shortcuts.state("save").released);
    key(SDL_SCANCODE_LCTRL, true, 3);
    CHECK(shortcuts.state("save").active); // The other keyboard kept its primary.
    event = {};
    event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    event.window.windowID = window;
    shortcuts.process(convert(event));
    CHECK(shortcuts.state("save").canceled);
    CHECK_FALSE(shortcuts.state("save").pressed);
    event.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    shortcuts.process(convert(event));
    shortcuts.begin_frame();
    key(SDL_SCANCODE_S, true, 3);
    CHECK_FALSE(shortcuts.state("save").active); // Regaining focus replays no held modifier.
}

TEST_CASE("Mouse buttons report on SDL's global mouse, keeping presses paired across relative mode") {
    i::Context look({{"look", i::ActionType::button, {{{i::ControlKind::mouse_button, SDL_BUTTON_RIGHT}}}}});
    const auto button = [&](bool down, SDL_MouseID which) {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.windowID = window;
        event.button.which = which;
        event.button.button = SDL_BUTTON_RIGHT;
        const auto converted = convert(event);
        look.process(converted);
        return converted.source.device;
    };
    // SDL reports `which` as 0 outside relative mode and as the mouse instance inside it.
    button(true, 0);
    CHECK(look.state("look").active);
    button(false, 5);
    CHECK_FALSE(look.state("look").active); // Released after entering relative mode.
    const auto device = button(true, 5);
    CHECK(device == 0u);
    button(false, 0);
    CHECK_FALSE(look.state("look").active); // Released after leaving relative mode.
    button(true, 0);
    SDL_Event removed{};
    removed.type = SDL_EVENT_MOUSE_REMOVED;
    removed.mdevice.which = 5;
    look.process(convert(removed));
    CHECK_FALSE(look.state("look").active); // Removing any mouse releases the buttons held on the global mouse.
}
