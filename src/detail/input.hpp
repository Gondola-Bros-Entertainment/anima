#pragma once
#include <anima/input.hpp>
#include <cstddef>

namespace anima::input::limits {
inline constexpr std::size_t actions = 128;
inline constexpr std::size_t action_name_bytes = 128;
inline constexpr std::size_t bindings_per_action = 32;
inline constexpr std::size_t modifiers_per_binding = 4;
inline constexpr std::size_t active_controls = 1024;
inline constexpr std::size_t document_bytes = 1024 * 1024;
inline constexpr unsigned key_code = 511;
inline constexpr unsigned mouse_button_code = 32;
inline constexpr unsigned gamepad_button_code = 63;
inline constexpr unsigned gamepad_axis_code = 15;
inline constexpr float minimum_threshold = .001F;
inline constexpr float maximum_binding_scale = 16;

// Whether @p control's code is inside the range of its kind, which must be valid. Mouse buttons start at 1.
[[nodiscard]] inline bool in_range(Control control) noexcept {
    const unsigned limit = control.kind == ControlKind::key              ? key_code
                           : control.kind == ControlKind::mouse_button   ? mouse_button_code
                           : control.kind == ControlKind::gamepad_button ? gamepad_button_code
                                                                         : gamepad_axis_code;
    return control.code <= limit && (control.kind != ControlKind::mouse_button || control.code != 0);
}
} // namespace anima::input::limits
