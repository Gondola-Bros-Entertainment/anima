#pragma once
#include <anima/input.hpp>
#include <optional>

/// @file
/// Conversion from SDL 3 events to anima::input events.
///
/// Part of the optional `anima::input_sdl` target (`ANIMA_BUILD_INPUT_SDL=ON`), built against
/// SDL 3.2 or later headers that must be compatible with the application's SDL. The converter
/// only reads event fields: it calls and links no SDL library and owns no SDL initialization,
/// event pump, window or device.

union SDL_Event;
namespace anima::input {
/// Converts @p event for the input context of the window with ID @p window, or returns nothing for
/// an event it does not convert.
///
/// Key presses and releases in @p window become ControlKind::key events coded by physical
/// scancode on device 0 (SDL's global keyboard); repeats and `SDL_SCANCODE_UNKNOWN` are dropped.
/// SDL keeps one pressed state per scancode for all keyboards, and releases keys on its global
/// keyboard when it resets them, so keys of every keyboard share device 0: when two keyboards hold
/// one key, the first release releases it and SDL drops the second. Mouse button presses and
/// releases in @p window, except touch-emulated ones, become ControlKind::mouse_button events coded
/// by SDL button index on device 0 (SDL's global mouse), which keeps presses and releases paired
/// across relative mouse mode changes. Gamepad button and axis events carry no window and use the
/// gamepad's instance ID, with axes mapped to [-1, 1] exactly at both ends; Context focus gates
/// them. Focus changes of @p window become focus events.
///
/// Gamepad removal or remapping disconnects that gamepad. Removing a keyboard or a mouse
/// disconnects device 0 of its class, releasing every key or every mouse button, since SDL sends
/// no releases for them; one still held on another device counts again once pressed again. SDL
/// reports keyboard removal on Windows and, through XInput2, on X11. On macOS it reports one
/// keyboard and never its removal, and on Wayland it reports removal only when a seat loses its
/// last keyboard, after releasing that seat's keys; there a key held on an unplugged keyboard is
/// released by the key-up the system sends, if any, or by focus loss.
///
/// A key, button or axis code outside its ControlKind range, which an unusual device can report,
/// returns nothing instead of failing the event pump. Other events, including text and IME, mouse
/// motion and wheel, touch, raw joystick, sensor and device additions, also return nothing; the
/// application opens gamepads itself. Throws `std::invalid_argument` when @p window is 0.
std::optional<Event> from_sdl(const SDL_Event &event, std::uint32_t window);
} // namespace anima::input
