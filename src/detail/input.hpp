#pragma once
#include <anima/input.hpp>
#include <cstddef>
#include <map>
#include <utility>
#include <vector>

namespace anima::input::limits {
inline constexpr std::size_t actions = 128;
inline constexpr std::size_t action_name_bytes = 128;
inline constexpr std::size_t bindings_per_action = 32;
inline constexpr std::size_t modifiers_per_binding = 4;
inline constexpr std::size_t active_controls = 1024;
inline constexpr std::size_t delta_accumulators = 1024;
inline constexpr std::size_t document_bytes = 1024 * 1024;
inline constexpr unsigned key_code = 511;
inline constexpr unsigned mouse_button_code = 32;
inline constexpr unsigned gamepad_button_code = 63;
inline constexpr unsigned gamepad_axis_code = 15;
inline constexpr unsigned delta_code = 1; // Code 0 is x and code 1 is y.
inline constexpr ControlKind last_kind = ControlKind::mouse_wheel;
inline constexpr float minimum_threshold = .001F;
inline constexpr float maximum_binding_scale = 16;

// Whether @p kind is a delta control, whose events carry increments rather than values.
[[nodiscard]] constexpr bool delta(ControlKind kind) noexcept {
    return kind == ControlKind::mouse_motion || kind == ControlKind::mouse_wheel;
}
// Whether @p control's code is inside the range of its kind, which must be valid. Mouse buttons start at 1.
[[nodiscard]] inline bool in_range(Control control) noexcept {
    const unsigned limit = control.kind == ControlKind::key              ? key_code
                           : control.kind == ControlKind::mouse_button   ? mouse_button_code
                           : control.kind == ControlKind::gamepad_button ? gamepad_button_code
                           : control.kind == ControlKind::gamepad_axis   ? gamepad_axis_code
                                                                         : delta_code;
    return control.code <= limit && (control.kind != ControlKind::mouse_button || control.code != 0);
}
} // namespace anima::input::limits

namespace anima::input {
struct Context::Staged {
    // A delta binding that accepts the increment: its action, the action's first binding and its own position
    // among all the map's bindings, and its sum for the event's device after the increment.
    struct Accepted {
        std::size_t action_index, first_binding, slot;
        float after;
    };
    // An event that validate(const Event &) accepts.
    Event event;
    // Applied first, as set_enabled() applies it.
    bool enabled{};
    // The node that records a held control anew, or none.
    std::map<Control, float> record;
    // The delta bindings that accept an increment, in map order.
    std::vector<Accepted> accepted;
    // The nodes of the sums an increment starts.
    Sums started;
};
} // namespace anima::input

namespace anima::detail {
// Lets scene dispatch stage an event for every context before it applies the event to any.
struct InputStaging {
    using Staged = input::Context::Staged;
    [[nodiscard]] static Staged stage(const input::Context &context, const input::Event &event, bool enable) {
        return context.stage(event, enable);
    }
    static void apply(input::Context &context, Staged &&staged) noexcept { context.apply(std::move(staged)); }
};
} // namespace anima::detail
