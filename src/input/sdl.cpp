#include "../detail/input.hpp"
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_version.h>
#include <anima/input_sdl.hpp>
#include <stdexcept>
#if !SDL_VERSION_ATLEAST(3, 2, 0)
#error Anima input requires SDL 3.2 or later headers
#endif
namespace anima::input {
std::optional<Event> from_sdl(const SDL_Event &e, std::uint32_t window) {
    if (!window)
        throw std::invalid_argument("SDL input conversion requires a nonzero target window ID");
    std::optional<Event> result;
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (e.key.windowID == window && !e.key.repeat && e.key.scancode > SDL_SCANCODE_UNKNOWN) {
            if (static_cast<unsigned>(e.key.scancode) > limits::key_code)
                throw std::invalid_argument("SDL scancode outside supported range");
            result = Event{EventType::control,
                           {ControlKind::key, static_cast<std::uint16_t>(e.key.scancode), e.key.which},
                           e.type == SDL_EVENT_KEY_DOWN ? 1.F : 0.F};
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.windowID == window && e.button.which != SDL_TOUCH_MOUSEID)
            result = Event{EventType::control,
                           {ControlKind::mouse_button, e.button.button, e.button.which},
                           e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 1.F : 0.F};
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        result = Event{EventType::control,
                       {ControlKind::gamepad_button, e.gbutton.button, e.gbutton.which},
                       e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? 1.F : 0.F};
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        result = Event{EventType::control,
                       {ControlKind::gamepad_axis, e.gaxis.axis, e.gaxis.which},
                       float(e.gaxis.value) / (e.gaxis.value < 0 ? 32768.F : 32767.F)};
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (e.window.windowID == window)
            result = Event{EventType::focus, {}, e.type == SDL_EVENT_WINDOW_FOCUS_GAINED ? 1.F : 0.F};
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_GAMEPAD_REMAPPED:
        result = Event{EventType::disconnect, {ControlKind::gamepad_button, 0, e.gdevice.which}, 0};
        break;
    case SDL_EVENT_KEYBOARD_REMOVED:
        result = Event{EventType::disconnect, {ControlKind::key, 0, e.kdevice.which}, 0};
        break;
    case SDL_EVENT_MOUSE_REMOVED:
        if (e.mdevice.which != SDL_TOUCH_MOUSEID)
            result = Event{EventType::disconnect, {ControlKind::mouse_button, 0, e.mdevice.which}, 0};
        break;
    default:
        break;
    }
    if (result)
        validate(*result);
    return result;
}
} // namespace anima::input
