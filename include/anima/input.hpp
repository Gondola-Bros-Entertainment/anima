#pragma once
#include <compare>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// Device-independent action maps and per-context action state.
///
/// Part of `anima::core`. Applications supply action names, bindings, device assignment and
/// responses, and convert platform events into Event values whose control codes follow the
/// converter's convention (see from_sdl()). Nothing here reads devices or clocks, starts threads,
/// invokes callbacks or issues game commands. Use each Context from one thread. Invalid arguments
/// throw `std::invalid_argument` unless a member states otherwise.

namespace anima::input {
/// Device selector matching every device of a control's class; valid in bindings, never in events.
inline constexpr std::uint32_t any_device = UINT32_MAX;
/// Physical control type. Each kind belongs to a device class (keyboard, mouse or gamepad), and
/// device IDs of one class are independent of the others.
enum class ControlKind {
    key,            ///< Keyboard key, code in [0, 511].
    mouse_button,   ///< Mouse button, code in [1, 32].
    gamepad_button, ///< Gamepad button, code in [0, 63].
    gamepad_axis    ///< Gamepad axis, code in [0, 15]; shares the gamepad class with buttons.
};
/// One physical control, or in a binding, a selector for one.
struct Control {
    ControlKind kind = ControlKind::key;
    /// Code within the ControlKind range.
    std::uint16_t code{};
    /// Device ID within the control's class, or any_device in a binding.
    std::uint32_t device = any_device;
    auto operator<=>(const Control &) const = default;
};
/// Action channel a binding drives.
enum class Channel {
    x, ///< The only channel of button and axis actions; the first vector2 component.
    y  ///< The second component; ActionType::vector2 only.
};
/// Maps one control, optionally gated by modifiers, onto an action channel.
///
/// A binding reads its control on the selected device, keeps the sign, remaps the magnitude beyond
/// #deadzone to [0, 1] and multiplies by #scale. It contributes zero unless every modifier is held.
///
/// Controls of one device class in a binding must come from one device. An explicit ID on the
/// control or any modifier of that class selects it, and conflicting explicit IDs are invalid.
/// Wildcards select a device that satisfies every control of the class; for the bound control, the
/// eligible device with the greatest magnitude wins, the lowest ID on ties. Each class selects its
/// device independently, so a keyboard modifier can qualify a mouse button.
struct Binding {
    Control control;
    /// Channel::y requires ActionType::vector2.
    Channel channel = Channel::x;
    /// Signed multiplier, in [-16, 16].
    float scale = 1;
    /// Magnitude at or below which the control reads as zero, in [0, 1).
    float deadzone = 0;
    /// Up to four keys, mouse buttons or gamepad buttons that must all be held, pressed before or
    /// after the bound control. No kind and code may repeat within the binding, including the
    /// bound control. Chords consume nothing: other bindings still see these controls.
    std::vector<Control> modifiers{};
};
/// How an action combines its bindings' contributions.
enum class ActionType {
    button, ///< The greatest positive contribution; the value is 1 while active and 0 otherwise.
    axis,   ///< The sum of contributions, clamped to [-1, 1].
    vector2 ///< Per-channel sums, each clamped to [-1, 1], normalized when longer than 1.
};
/// A named action and its bindings.
struct Action {
    /// Unique within the map: 1 to 128 printable ASCII characters, without spaces.
    std::string name;
    ActionType type = ActionType::button;
    /// Up to 32 bindings; an empty list leaves the action inactive.
    std::vector<Binding> bindings;
    /// Activity threshold, in [0.001, 1]: the action is active while the length of its combined
    /// value reaches it. Axis values stay continuous below it.
    float threshold = .5F;
};
/// Action map: up to 128 actions with unique names.
using Map = std::vector<Action>;
/// Action value.
struct Value {
    /// Button or axis value, or the first vector2 component.
    float x{};
    /// Second vector2 component; zero for other actions.
    float y{};
};
/// Action state. The edge flags latch until the next Context::begin_frame().
struct State {
    Value value;
    /// Whether the combined value reaches Action::threshold.
    bool active{};
    /// The action became active.
    bool pressed{};
    /// The action became inactive, including by cancellation.
    bool released{};
    /// Cancellation released the action; see Context::cancel.
    bool canceled{};
};
/// Input event kind.
enum class EventType {
    control,   ///< A control changed value.
    focus,     ///< The owning window gained or lost focus.
    disconnect ///< A device left, so its held controls are forgotten.
};
/// One input event, applied by Context::process.
struct Event {
    EventType type = EventType::control;
    /// The control that changed, or for a disconnect the device class (from the kind) and device.
    /// Needs a concrete device ID; focus events ignore it.
    Control source;
    /// Control events: 0 or 1 for keys and buttons, [-1, 1] for gamepad axes, 0 meaning released.
    /// Focus events: 1 gained or 0 lost. Disconnects ignore it.
    float value{};
};
/// Throws `std::invalid_argument` unless @p map meets the Map, Action, Binding and ControlKind
/// rules.
void validate(const Map &map);
/// Throws `std::invalid_argument` unless @p event meets the Event rules.
void validate(const Event &event);
/// Action state for one user or input context, updated by events in order.
///
/// Feed each frame's events to process() after begin_frame(), then read state(). Edges latch until
/// the next begin_frame(), so a press and release within one frame both report even when the final
/// value is zero; repeated values do not press again. The context records only nonzero values of
/// controls that some binding can use, at most 1,024 at once. Contexts are independent: copies own
/// separate bindings, values and latches.
class Context {
  public:
    /// Creates an enabled, focused context with every action inactive. Throws
    /// `std::invalid_argument` when validate(const Map &) rejects @p map.
    explicit Context(Map map = {});
    Context(const Context &) = default;
    Context &operator=(const Context &other);
    Context(Context &&) noexcept = default;
    Context &operator=(Context &&) noexcept = default;
    /// The actions and their current bindings.
    [[nodiscard]] std::span<const Action> actions() const { return map_; }
    /// A copy of @p action's state. Throws `std::out_of_range` for an unknown name.
    [[nodiscard]] State state(std::string_view action) const;
    /// Clears every State::pressed, State::released and State::canceled latch; values and activity
    /// stay. Call it once per frame before processing that frame's events.
    void begin_frame();
    /// Applies @p event, which validate(const Event &) must accept.
    ///
    /// A focus event calls set_focused(), even while disabled. A disconnect forgets the recorded
    /// controls of that device class and ID and reevaluates, so other devices keep their state. A
    /// control event is ignored while disabled or unfocused; otherwise its value is recorded (zero
    /// releases the control) and every action is reevaluated. Throws `std::length_error` when a
    /// new control would exceed 1,024 recorded controls. A rejected event changes nothing.
    void process(const Event &event);
    /// Disabling cancels the context and ignores control events until it is enabled again;
    /// enabling restores nothing.
    void set_enabled(bool enabled);
    /// Losing focus cancels the context and ignores control events until focus returns; regaining
    /// it restores nothing. Contexts start focused, so set the owning window's focus before feeding
    /// input.
    void set_focused(bool focused);
    [[nodiscard]] bool enabled() const { return enabled_; }
    [[nodiscard]] bool focused() const { return focused_; }
    /// Forgets every recorded control, zeroes every value and clears State::pressed; each active
    /// action becomes inactive and latches State::released and State::canceled. Controls still
    /// held count again only after new events.
    void cancel();
    /// Replaces @p action's bindings, then cancels the context so actions need fresh input under
    /// the new configuration. Throws `std::out_of_range` for an unknown action and
    /// `std::invalid_argument` for invalid bindings, changing nothing.
    void rebind(std::string_view action, std::vector<Binding> bindings);

  private:
    std::size_t index(std::string_view name) const;
    void evaluate();
    float read(const Binding &binding) const;
    Map map_;
    std::vector<State> states_;
    std::map<Control, float> values_;
    bool enabled_ = true, focused_ = true;
};
} // namespace anima::input
