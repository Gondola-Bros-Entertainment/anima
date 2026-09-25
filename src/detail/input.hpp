#pragma once
#include <cstddef>

namespace anima::input::limits {
inline constexpr std::size_t actions = 128;
inline constexpr std::size_t action_name_bytes = 128;
inline constexpr std::size_t bindings_per_action = 32;
inline constexpr std::size_t active_controls = 1024;
inline constexpr std::size_t document_bytes = 1024 * 1024;
inline constexpr unsigned key_code = 511;
inline constexpr unsigned mouse_button_code = 32;
inline constexpr unsigned gamepad_button_code = 63;
inline constexpr unsigned gamepad_axis_code = 15;
inline constexpr float minimum_threshold = .001F;
inline constexpr float maximum_binding_scale = 16;
} // namespace anima::input::limits
