#include <SDL3/SDL_events.h>
#include <anima/input_sdl.hpp>
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>

// The event sequences below are ones SDL 3.4 can post; the comment above each test names the SDL 3.4.16 source that
// produces them.
namespace i = anima::input;
namespace {
constexpr std::uint32_t window = 8;

// A GUID in the layout SDL builds for a USB gamepad (src/joystick/SDL_joystick.c:2932-2974), with vendor @p vendor.
SDL_GUID model_guid(Uint8 vendor) {
    SDL_GUID guid{};
    guid.data[0] = 0x03;
    guid.data[4] = vendor;
    return guid;
}
// Stands in for SDL_GetGamepadGUIDForID while gamepads 5 and 9, identical units of one model, and 7, of another,
// are connected. Like SDL, it returns a zero GUID for an instance it does not report
// (src/joystick/SDL_joystick.c:3485-3500).
SDL_GUID connected_guid(std::uint32_t instance) {
    if (instance == 5 || instance == 9)
        return model_guid(0x5e);
    return instance == 7 ? model_guid(0x4c) : SDL_GUID{};
}
// Stands in for SDL_GetGamepadGUIDForID once SDL reports no gamepad.
SDL_GUID no_guid(std::uint32_t) { return SDL_GUID{}; }
// Converts @p event for the input context of the window, which must accept it.
i::Event convert(const SDL_Event &event, i::SdlGamepadGuid lookup = connected_guid) {
    const auto converted = i::from_sdl(event, window, lookup);
    REQUIRE(converted);
    return *converted;
}
bool converts(const SDL_Event &event, std::uint32_t target = window) {
    return i::from_sdl(event, target, connected_guid).has_value();
}
SDL_Event gamepad_button(SDL_GamepadButton button, bool down, SDL_JoystickID gamepad) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    event.gbutton.which = gamepad;
    event.gbutton.button = static_cast<Uint8>(button);
    event.gbutton.down = down;
    return event;
}
// A key event in the window, as SDL_SendKeyboardKeyInternal posts it (src/events/SDL_keyboard.c:638-652).
SDL_Event key_event(SDL_Scancode code, bool down, SDL_KeyboardID keyboard, bool repeat = false) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.windowID = window;
    event.key.which = keyboard;
    event.key.scancode = code;
    event.key.down = down;
    event.key.repeat = repeat;
    return event;
}
SDL_Event window_event(SDL_EventType type) {
    SDL_Event event{};
    event.type = type;
    event.window.windowID = window;
    return event;
}
} // namespace

// SDL posts a repeat for a press of a key it already holds, such as the system's auto-repeat
// (src/events/SDL_keyboard.c:550-556). A focus loss with the key still held is what SDL posts when focus moves to
// another of the application's windows, which resets no keys (src/events/SDL_keyboard.c:348-356).
TEST_CASE("Key presses in the target window drive actions, without repeats") {
    i::Context c({{"accept", i::ActionType::button, {{{i::ControlKind::key, SDL_SCANCODE_SPACE}}}}});
    auto e = key_event(SDL_SCANCODE_SPACE, true, 3);
    CHECK_FALSE(converts(e, 7)); // Another window's event is filtered out.
    c.process(convert(e));
    CHECK(c.state("accept").pressed);
    CHECK_FALSE(converts(key_event(SDL_SCANCODE_SPACE, true, 3, true)));
    c.process(convert(window_event(SDL_EVENT_WINDOW_FOCUS_LOST)));
    CHECK(c.state("accept").canceled); // SDL focus loss cancels held input.
    c.process(convert(window_event(SDL_EVENT_WINDOW_FOCUS_GAINED)));
    CHECK(c.focused());
}

// Windows raw keyboard input posts each key under its keyboard's device handle
// (src/video/windows/SDL_windowsevents.c:735-794). SDL keeps one state per scancode for all keyboards, so it posts
// keyboard 4's press of the S keyboard 3 holds as a repeat (src/events/SDL_keyboard.c:550-556), posts keyboard 4's
// release, and drops keyboard 3's release of the released key (src/events/SDL_keyboard.c:559-561).
TEST_CASE("Keys of every keyboard share SDL's global keyboard, so a key two keyboards hold never sticks") {
    i::Context c({{"back", i::ActionType::button, {{{i::ControlKind::key, SDL_SCANCODE_S}}}}});
    const auto pressed = convert(key_event(SDL_SCANCODE_S, true, 3));
    CHECK(pressed.source.device == 0u);
    c.process(pressed);
    CHECK(c.state("back").active);
    CHECK_FALSE(converts(key_event(SDL_SCANCODE_S, true, 4, true)));
    c.process(convert(key_event(SDL_SCANCODE_S, false, 4)));
    // The first release releases the key while keyboard 3 still holds it, as SDL's own key state does.
    CHECK_FALSE(c.state("back").active);
    CHECK(c.state("back").released);
    CHECK_FALSE(c.state("back").canceled);
    c.begin_frame();
    c.process(convert(key_event(SDL_SCANCODE_S, true, 3)));
    CHECK(c.state("back").pressed); // SDL dropped keyboard 3's release, so its next press is a fresh one.
}

// Windows raw keyboard input posts the press under the keyboard's device handle
// (src/video/windows/SDL_windowsevents.c:735-794). Entering a window's modal move or resize loop resets the keyboard
// without a focus change (src/video/windows/SDL_windowsevents.c:1920-1922), and SDL_ResetKeyboard posts the release
// under SDL_GLOBAL_KEYBOARD_ID, 0 (src/events/SDL_keyboard.c:224-237).
TEST_CASE("A key SDL resets on its global keyboard releases a press made under a keyboard's own ID") {
    constexpr SDL_KeyboardID raw_keyboard = 0x10043;
    i::Context c({{"forward", i::ActionType::button, {{{i::ControlKind::key, SDL_SCANCODE_W}}}}});
    c.process(convert(key_event(SDL_SCANCODE_W, true, raw_keyboard)));
    CHECK(c.state("forward").active);
    c.process(convert(key_event(SDL_SCANCODE_W, false, 0)));
    CHECK_FALSE(c.state("forward").active);
    CHECK(c.state("forward").released);
}

// Windows' default message loop posts every key under SDL_GLOBAL_KEYBOARD_ID
// (src/video/windows/SDL_windowsevents.c:1594-1596), while its hotplug check adds and removes keyboards under their
// raw input handles (src/video/windows/SDL_windowsevents.c:1058-1090). SDL posts the removal without releasing any
// key (src/events/SDL_keyboard.c:150-170).
TEST_CASE("Removing any keyboard releases every key") {
    constexpr SDL_KeyboardID removed = 0x2002b;
    i::Context c({{"forward", i::ActionType::button, {{{i::ControlKind::key, SDL_SCANCODE_W}}}}});
    c.process(convert(key_event(SDL_SCANCODE_W, true, 0)));
    CHECK(c.state("forward").active);
    SDL_Event event{};
    event.type = SDL_EVENT_KEYBOARD_REMOVED;
    event.kdevice.which = removed;
    const auto disconnect = convert(event);
    CHECK(disconnect.type == i::EventType::disconnect);
    CHECK(disconnect.source.kind == i::ControlKind::key);
    CHECK(disconnect.source.device == 0u);
    c.process(disconnect);
    CHECK_FALSE(c.state("forward").active);
    CHECK(c.state("forward").released);
    CHECK_FALSE(c.state("forward").canceled);
}

// SDL posts gamepad axis motion through the gamepad's mapping (src/joystick/SDL_gamepad.c:4395-4409). When a new
// mapping applies to an open gamepad, SDL loads it without releasing any control and posts the remapping
// (src/joystick/SDL_gamepad.c:685-690, :1990-2025 and :458-470).
TEST_CASE("Gamepad axes reach -1 and 1 exactly, and remapping or removing the gamepad disconnects it") {
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
    e.type = SDL_EVENT_GAMEPAD_REMAPPED;
    e.gdevice.which = 12;
    c.process(convert(e));
    CHECK(c.state("stick").value.x == 0);
    e.type = SDL_EVENT_GAMEPAD_REMOVED;
    const auto removed = convert(e);
    CHECK(removed.type == i::EventType::disconnect);
    CHECK(removed.source.kind == i::ControlKind::gamepad_button);
    CHECK(removed.source.device == 12u);
}

// SDL posts a gamepad's addition, then its button events under its instance ID (src/joystick/SDL_gamepad.c:4411-4453).
// Removing it releases its held buttons before posting the removal (src/joystick/SDL_gamepad.c:357-375 and :434-456);
// the HIDAPI driver drops the instance first (src/joystick/hidapi/SDL_hidapijoystick.c:821-839), so when the
// application converts those releases, SDL_GetGamepadGUIDForID returns a zero GUID for it. Reconnecting gives the
// gamepad a new instance ID, since SDL never reuses one (include/SDL3/SDL_joystick.h:96-106).
TEST_CASE("Gamepad events carry SDL's GUID, so a binding by identity follows its gamepad to a new instance ID") {
    SDL_Event added{};
    added.type = SDL_EVENT_GAMEPAD_ADDED;
    added.gdevice.which = 5;
    CHECK_FALSE(converts(added));
    const auto pressed = convert(gamepad_button(SDL_GAMEPAD_BUTTON_SOUTH, true, 5));
    const auto guid = model_guid(0x5e);
    CHECK(std::ranges::equal(pressed.source.identity, guid.data)); // Byte for byte, first byte first.
    i::Binding jump{{i::ControlKind::gamepad_button, SDL_GAMEPAD_BUTTON_SOUTH, i::any_device, pressed.source.identity}};
    i::Context c({{"jump", i::ActionType::button, {jump}}});
    c.process(pressed);
    CHECK(c.state("jump").pressed);
    const auto released = convert(gamepad_button(SDL_GAMEPAD_BUTTON_SOUTH, false, 5), no_guid);
    CHECK(released.source.identity == i::DeviceIdentity{});
    c.process(released);
    CHECK_FALSE(c.state("jump").active); // A release converted without the GUID still releases the press.
    SDL_Event removed{};
    removed.type = SDL_EVENT_GAMEPAD_REMOVED;
    removed.gdevice.which = 5;
    c.process(convert(removed, no_guid));
    c.begin_frame();
    c.process(convert(gamepad_button(SDL_GAMEPAD_BUTTON_SOUTH, true, 9)));
    CHECK(c.state("jump").pressed); // The reconnected gamepad matches under its new instance ID.
    c.process(convert(gamepad_button(SDL_GAMEPAD_BUTTON_SOUTH, false, 9)));
    c.process(convert(gamepad_button(SDL_GAMEPAD_BUTTON_SOUTH, true, 7)));
    CHECK_FALSE(c.state("jump").active); // A gamepad of another model does not.
    // Keys and mouse buttons carry no identity.
    CHECK(convert(key_event(SDL_SCANCODE_SPACE, true, 3)).source.identity == i::DeviceIdentity{});
}

// SDL posts button presses from the global mouse outside relative mode (src/events/SDL_mouse.c:991-1003); the touch
// events it emulates as mouse buttons carry SDL_TOUCH_MOUSEID.
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
    CHECK_FALSE(converts(e));
}

// A text event, as SDL_SendKeyboardText posts it (src/events/SDL_keyboard.c:782-784).
TEST_CASE("Text input is not an action input, and a zero target window or a null GUID lookup is rejected") {
    SDL_Event e{};
    e.type = SDL_EVENT_TEXT_INPUT;
    CHECK_FALSE(converts(e));
    CHECK_THROWS_WITH_AS(i::from_sdl(e, 0, connected_guid), "SDL input conversion requires a nonzero target window ID",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(i::from_sdl(e, window, nullptr), "SDL input conversion requires a gamepad GUID lookup",
                         std::invalid_argument);
}

// The highest code of each documented ControlKind range converts. One past it, as an unusual device can
// report, is dropped rather than thrown, so the application's event pump keeps running. SDL 3.4 itself posts
// gamepad buttons and axes only below SDL_GAMEPAD_BUTTON_COUNT and SDL_GAMEPAD_AXIS_COUNT, so these gamepad codes
// stand for a later SDL whose headers the converter also accepts.
TEST_CASE("Codes beyond each control kind's range are dropped") {
    constexpr int highest_key = 511;
    constexpr Uint8 highest_mouse_button = 32;
    constexpr Uint8 highest_gamepad_button = 63;
    constexpr Uint8 highest_gamepad_axis = 15;
    constexpr SDL_JoystickID gamepad = 7;
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

// Keys arrive under per-keyboard IDs, as Windows raw keyboard input posts them
// (src/video/windows/SDL_windowsevents.c:735-794), and SDL merges them into one state per scancode
// (src/events/SDL_keyboard.c:548-567). SDL posts the removal of keyboard 4 without releasing keys
// (src/events/SDL_keyboard.c:150-170), so keyboard 3's keys stay held in SDL and their next key-ups are posted. The
// focus loss is one SDL posts when focus moves to another of the application's windows, which resets no keys
// (src/events/SDL_keyboard.c:348-356); S is released there, so its key-up carries the other window's ID.
TEST_CASE("Converted SDL keys drive a chord across keyboards through removal and focus changes") {
    i::Binding save{{i::ControlKind::key, SDL_SCANCODE_S}};
    save.modifiers = {{i::ControlKind::key, SDL_SCANCODE_LCTRL}};
    i::Context shortcuts({{"save", i::ActionType::button, {save}}});
    const auto key = [&](SDL_Scancode code, bool down, SDL_KeyboardID device) {
        shortcuts.process(convert(key_event(code, down, device)));
    };
    key(SDL_SCANCODE_S, true, 3);
    CHECK_FALSE(shortcuts.state("save").active); // The shortcut needs its modifier.
    key(SDL_SCANCODE_LCTRL, true, 4);
    CHECK(shortcuts.state("save").pressed); // SDL's keyboards share one key state, so they combine.
    shortcuts.begin_frame();
    key(SDL_SCANCODE_LCTRL, false, 4);
    CHECK(shortcuts.state("save").released);
    CHECK_FALSE(shortcuts.state("save").canceled);
    key(SDL_SCANCODE_LCTRL, true, 3);
    // Keyboard 3's modifier completes the chord again, and both edges stay latched.
    CHECK(shortcuts.state("save").active);
    CHECK(shortcuts.state("save").pressed);
    CHECK(shortcuts.state("save").released);
    shortcuts.begin_frame();
    SDL_Event event{};
    event.type = SDL_EVENT_KEYBOARD_REMOVED;
    event.kdevice.which = 4;
    shortcuts.process(convert(event));
    // Removing any keyboard releases every key, including keyboard 3's.
    CHECK_FALSE(shortcuts.state("save").active);
    CHECK(shortcuts.state("save").released);
    CHECK_FALSE(shortcuts.state("save").canceled);
    key(SDL_SCANCODE_S, false, 3);
    key(SDL_SCANCODE_LCTRL, false, 3);
    key(SDL_SCANCODE_LCTRL, true, 3);
    key(SDL_SCANCODE_S, true, 3);
    CHECK(shortcuts.state("save").active); // Keyboard 3's keys count again once pressed again.
    shortcuts.process(convert(window_event(SDL_EVENT_WINDOW_FOCUS_LOST)));
    CHECK(shortcuts.state("save").canceled);
    CHECK_FALSE(shortcuts.state("save").pressed);
    auto elsewhere = key_event(SDL_SCANCODE_S, false, 3);
    elsewhere.key.windowID = window + 1;
    CHECK_FALSE(converts(elsewhere));
    shortcuts.process(convert(window_event(SDL_EVENT_WINDOW_FOCUS_GAINED)));
    shortcuts.begin_frame();
    key(SDL_SCANCODE_S, true, 3);
    CHECK_FALSE(shortcuts.state("save").active); // Regaining focus replays no held modifier.
}

// SDL reports a mouse button's `which` as 0 outside relative mode and as the mouse instance inside it
// (src/events/SDL_mouse.c:991-1003), and posts a mouse's removal without releasing its buttons
// (src/events/SDL_mouse.c:357-392).
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
