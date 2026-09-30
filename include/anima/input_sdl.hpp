#pragma once
#include <anima/input.hpp>
#include <array>
#include <cstddef>
#include <cstdint>

/// @file
/// Conversion from SDL 3 events to anima::input events.
///
/// Part of the optional `anima::input_sdl` target (`ANIMA_BUILD_INPUT_SDL=ON`), built against
/// SDL 3.2 or later headers that must be compatible with the application's SDL. The converter
/// reads event fields and calls only the gamepad GUID lookup the application passes: it calls and
/// links no SDL library itself and owns no SDL initialization, event pump, window or device.

union SDL_Event;
struct SDL_GUID;
namespace anima::input {
/// Returns the GUID SDL reports for the gamepad with instance ID @p instance, with the signature of
/// `SDL_GetGamepadGUIDForID`, which returns a zero GUID for an instance SDL does not report.
using SdlGamepadGuid = SDL_GUID (*)(std::uint32_t instance);
class SdlEvents;
/// Converts @p event for the input context of the window with ID @p window into the events to
/// process, in order, which may be none.
///
/// Key presses and releases in @p window become ControlKind::key events coded by physical
/// scancode on device 0 (SDL's global keyboard); repeats and `SDL_SCANCODE_UNKNOWN` are dropped.
/// SDL keeps one pressed state per scancode for all keyboards, and releases keys on its global
/// keyboard when it resets them, so keys of every keyboard share device 0: when two keyboards hold
/// one key, the first release releases it and SDL drops the second. Mouse button presses and
/// releases in @p window become ControlKind::mouse_button events coded by SDL button index on
/// device 0 (SDL's global mouse), which keeps presses and releases paired across relative mouse
/// mode changes. Keys and mouse buttons carry no identity. Focus changes of @p window become focus
/// events.
///
/// Mouse motion and wheel events in @p window become increments of ControlKind::mouse_motion and
/// ControlKind::mouse_wheel on device 0 as well, whatever their `which`. SDL reports `which` as 0
/// outside relative mouse mode and as the mouse's instance ID inside it, so one device lets a
/// button pressed before the application enters relative mode chord with the motion after it. Each
/// nonzero, finite axis becomes one event, x (code 0) before y (code 1), carrying SDL's amount
/// unchanged; scale it with Binding::scale. Motion takes `xrel` and `yrel`, which grow rightward
/// and downward: window coordinates outside relative mode, and inside it the platform's relative
/// motion after SDL's relative mouse scaling hints or `SDL_SetRelativeMouseTransform`. The warp
/// that `SDL_HINT_MOUSE_RELATIVE_WARP_MOTION` reports in relative mode carries no motion, so it
/// converts to nothing. The wheel takes the float `x` and `y`, positive rightward and away from
/// the user, which can be fractional: Windows divides by its notch size, so a notch is 1, and
/// macOS precise scrolling reports a tenth of AppKit's scrolling delta. The pointer position is
/// not converted; read `x` and `y` from the same event or call `SDL_GetMouseState`.
///
/// Wheel amounts keep the direction SDL delivers them in, the one UiContext scrolls by, and
/// `SDL_MouseWheelEvent::direction` is ignored. SDL reports `SDL_MOUSEWHEEL_FLIPPED` only on
/// macOS, iOS and Wayland, and there the amounts already follow the user's natural scrolling
/// setting, so undoing it would override that setting on those platforms alone. For the opposite
/// direction, bind a negative Binding::scale; to follow the device's direction where SDL reports
/// it, negate `x` and `y` of a flipped event before converting it.
///
/// Mouse events that SDL emulates for touches (`SDL_TOUCH_MOUSEID`) are dropped. Those it emulates
/// for a pen (`SDL_PEN_MOUSEID`), unless `SDL_HINT_PEN_MOUSE_EVENTS` is off, convert like the
/// mouse's, so a pen drives the mouse's actions; SDL computes a pen's motion from its positions,
/// so the motion includes the jump to where a pen comes into range.
///
/// Gamepad button and axis events carry no window and use the gamepad's instance ID, with axes
/// mapped to [-1, 1] exactly at both ends; Context focus gates them. Their Control::identity is
/// the GUID @p gamepad_guid returns for that instance, byte for byte, or none for a zero GUID, so a
/// binding can keep to a gamepad model across reconnection and restarts, when SDL assigns new
/// instance IDs. Pass `SDL_GetGamepadGUIDForID`. Its GUID combines the bus, vendor, product,
/// version, driver and a name checksum, so identical gamepads share one, one gamepad can report
/// another over a different bus or driver or once its reported version changes, and GUIDs can
/// differ between platforms. The converter calls @p gamepad_guid once for each gamepad button and
/// axis event, on the calling thread.
///
/// Gamepad removal or remapping disconnects that gamepad. Removing a keyboard or a mouse
/// disconnects device 0 of its class, releasing every key or every mouse button, since SDL sends
/// no releases for them, and dropping the frame's mouse increments; a key or button still held on
/// another device counts again once pressed again. SDL reports keyboard removal on Windows and,
/// through XInput2, on X11. On macOS it reports one keyboard and never its removal, and on Wayland
/// it reports removal only when a seat loses its last keyboard, after releasing that seat's keys;
/// there a key held on an unplugged keyboard is released by the key-up the system sends, if any,
/// or by focus loss.
///
/// A key, button or axis code outside its ControlKind range, which an unusual device can report,
/// converts to nothing instead of failing the event pump. Other events, including text and IME,
/// touch, raw joystick, sensor and device additions, also convert to nothing; the application
/// opens gamepads itself. Throws `std::invalid_argument` when @p window is 0 or @p gamepad_guid is
/// null.
SdlEvents from_sdl(const SDL_Event &event, std::uint32_t window, SdlGamepadGuid gamepad_guid);
/// The events from_sdl() converts one SDL event into, in the order to process them: none, one, or
/// for a mouse motion or wheel event one per axis. A contiguous range of at most two events.
class SdlEvents {
  public:
    /// The first event.
    [[nodiscard]] const Event *begin() const noexcept { return events_.data(); }
    /// Past the last event.
    [[nodiscard]] const Event *end() const noexcept { return events_.data() + count_; }
    /// The number of events, from 0 to 2.
    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    /// Whether there are no events.
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    /// The event at @p index, which must be less than size().
    [[nodiscard]] const Event &operator[](std::size_t index) const noexcept { return events_[index]; }

  private:
    friend SdlEvents from_sdl(const SDL_Event &, std::uint32_t, SdlGamepadGuid);
    std::array<Event, 2> events_{};
    std::size_t count_{};
};
} // namespace anima::input
