#include "../detail/input.hpp"
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_version.h>
#include <algorithm>
#include <anima/input_sdl.hpp>
#include <cmath>
#include <stdexcept>
#include <utility>
#if !SDL_VERSION_ATLEAST(3, 2, 0)
#error Anima input requires SDL 3.2 or later headers
#endif
namespace anima::input {
namespace {
// SDL reports a mouse event's `which` as 0 outside relative mouse mode and as the mouse instance ID inside it, and
// toggling the mode sends no releases, so one physical button can arrive under two IDs. Reporting every mouse button,
// motion and wheel event on SDL's global mouse (ID 0) keeps each press and release paired, and a button held across
// the switch on the device of the motion it chords with.
constexpr std::uint32_t global_mouse = 0;
// SDL keeps one pressed state per scancode for all keyboards: a second keyboard's press of a held key arrives as
// a repeat, and the release of a released key is dropped. SDL also releases keys under its global keyboard (ID 0)
// when it resets them, though their presses can carry a keyboard's own ID. Reporting every key on the global
// keyboard keeps each press and release paired, as SDL's own key state does.
constexpr std::uint32_t global_keyboard = 0;
static_assert(sizeof(SDL_GUID::data) == DeviceIdentity{}.size());
// The identity of a gamepad with @p guid, byte for byte; a zero GUID becomes none.
DeviceIdentity gamepad_identity(const SDL_GUID &guid) {
    DeviceIdentity result{};
    std::ranges::copy(guid.data, result.begin());
    return result;
}
} // namespace
SdlEvents from_sdl(const SDL_Event &e, std::uint32_t window, SdlGamepadGuid gamepad_guid) {
    if (!window)
        throw std::invalid_argument("SDL input conversion requires a nonzero target window ID");
    if (!gamepad_guid)
        throw std::invalid_argument("SDL input conversion requires a gamepad GUID lookup");
    SdlEvents result;
    const auto add = [&result](const Event &event) {
        // An unusual device can report a code beyond its kind's range. Dropping the event, like an unknown key,
        // keeps one device from making the application's event pump throw.
        if (event.type == EventType::control && !limits::in_range(event.source))
            return;
        validate(event);
        result.events_[result.count_++] = event;
    };
    // One increment per nonzero axis; zero is no motion, as in the warp SDL reports in relative mode, and a
    // non-finite amount is dropped like an out-of-range code.
    const auto increments = [&add](ControlKind kind, float x, float y) {
        for (const auto &[code, amount] :
             {std::pair<std::uint16_t, float>{0, x}, std::pair<std::uint16_t, float>{1, y}})
            if (amount != 0 && std::isfinite(amount))
                add({EventType::control, {kind, code, global_mouse}, amount});
    };
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        // Checking the range before narrowing keeps a large scancode from wrapping into it.
        if (e.key.windowID == window && !e.key.repeat && e.key.scancode > SDL_SCANCODE_UNKNOWN &&
            static_cast<unsigned>(e.key.scancode) <= limits::key_code)
            add({EventType::control,
                 {ControlKind::key, static_cast<std::uint16_t>(e.key.scancode), global_keyboard},
                 e.type == SDL_EVENT_KEY_DOWN ? 1.F : 0.F});
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.windowID == window && e.button.which != SDL_TOUCH_MOUSEID)
            add({EventType::control,
                 {ControlKind::mouse_button, e.button.button, global_mouse},
                 e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 1.F : 0.F});
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (e.motion.windowID == window && e.motion.which != SDL_TOUCH_MOUSEID)
            increments(ControlKind::mouse_motion, e.motion.xrel, e.motion.yrel);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (e.wheel.windowID == window && e.wheel.which != SDL_TOUCH_MOUSEID)
            increments(ControlKind::mouse_wheel, e.wheel.x, e.wheel.y);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        add({EventType::control,
             {ControlKind::gamepad_button, e.gbutton.button, e.gbutton.which,
              gamepad_identity(gamepad_guid(e.gbutton.which))},
             e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? 1.F : 0.F});
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        add({EventType::control,
             {ControlKind::gamepad_axis, e.gaxis.axis, e.gaxis.which, gamepad_identity(gamepad_guid(e.gaxis.which))},
             float(e.gaxis.value) /
                 (e.gaxis.value < 0 ? -float(SDL_JOYSTICK_AXIS_MIN) : float(SDL_JOYSTICK_AXIS_MAX))});
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (e.window.windowID == window)
            add({EventType::focus, {}, e.type == SDL_EVENT_WINDOW_FOCUS_GAINED ? 1.F : 0.F});
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_GAMEPAD_REMAPPED:
        add({EventType::disconnect, {ControlKind::gamepad_button, 0, e.gdevice.which}, 0});
        break;
    case SDL_EVENT_KEYBOARD_REMOVED:
        // SDL releases no keys when a keyboard is removed, and every key is held on the global keyboard.
        add({EventType::disconnect, {ControlKind::key, 0, global_keyboard}, 0});
        break;
    case SDL_EVENT_MOUSE_REMOVED:
        // SDL releases no buttons when a mouse is removed, and every button is held, and every increment summed, on
        // the global mouse.
        if (e.mdevice.which != SDL_TOUCH_MOUSEID)
            add({EventType::disconnect, {ControlKind::mouse_button, 0, global_mouse}, 0});
        break;
    default:
        break;
    }
    return result;
}
} // namespace anima::input
