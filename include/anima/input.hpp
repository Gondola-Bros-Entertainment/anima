#pragma once
#include <array>
#include <compare>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// @file
/// Device-independent action maps and per-context action state.
///
/// Part of `anima::core`. Applications supply action names, bindings, device assignment and
/// responses, and convert platform events into Event values whose control codes follow the
/// converter's convention (see from_sdl()). Nothing here reads devices or clocks, starts threads,
/// invokes callbacks or issues game commands. Use each Context from one thread. Invalid arguments
/// throw `std::invalid_argument` unless a member states otherwise.

namespace anima::detail {
struct InputStaging;
} // namespace anima::detail

namespace anima::input {
/// Device selector matching every device of a control's class; valid in bindings, never in events.
inline constexpr std::uint32_t any_device = UINT32_MAX;
/// Stable identity of a device, such as the GUID SDL reports for a gamepad model, which survives
/// reconnection and restarts where device IDs do not. All zeros means none. Identical devices can
/// share one identity.
using DeviceIdentity = std::array<std::uint8_t, 16>;
/// Physical control type. Each kind belongs to a device class (keyboard, mouse or gamepad), and
/// device IDs of one class are independent of the others.
///
/// Keys, buttons and axes are held controls: an event sets a level that lasts until the control's
/// next event. Mouse motion and the wheel are delta controls: an event carries an increment, a
/// displacement with no resting level, and bindings add up the increments of one begin_frame()
/// interval (see Binding). Delta codes are 0 for x and 1 for y, with amounts in the converter's
/// units and signed as each kind states. Motion y grows downward while Anima's +Y is up, so
/// upward pitch from motion y usually takes a negative Binding::scale.
enum class ControlKind {
    key,            ///< Keyboard key, code in [0, 511].
    mouse_button,   ///< Mouse button, code in [1, 32].
    gamepad_button, ///< Gamepad button, code in [0, 63].
    gamepad_axis,   ///< Gamepad axis, code in [0, 15]; shares the gamepad class with buttons.
    mouse_motion,   ///< Pointer motion, a mouse delta control: x rightward, y downward.
    mouse_wheel     ///< Scroll wheel, a mouse delta control: x rightward, y away from the user.
};
/// One physical control, or in a binding, a selector for one.
struct Control {
    ControlKind kind = ControlKind::key;
    /// Code within the ControlKind range.
    std::uint16_t code{};
    /// Device ID within the control's class, or any_device in a binding. An ID names a device only
    /// while the converter reports it, so a binding that must outlast reconnection or a restart
    /// selects by #identity instead.
    std::uint32_t device = any_device;
    /// In an event, the identity of the source device, or none when the converter reports none. In a
    /// binding, the identity the selected device must report, or none to accept any device.
    DeviceIdentity identity{};
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
/// A binding of a delta control (a delta binding) instead contributes #scale times the sum of the
/// increments it accepted since the last Context::begin_frame(), unclamped, from every device it
/// admits. It decides as each increment arrives: it accepts the increment when it admits the
/// increment's device, holds every modifier as a held binding does (those of the mouse class on
/// that device), and no binding that outranks it holds all of its own there. A modifier pressed
/// after an increment therefore does not claim it, and one released after it does not take it
/// back.
///
/// Controls of one device class in a binding must come from one device. An explicit ID on the
/// control or any modifier of that class selects it, and an identity on any of them admits only
/// controls whose latest event carried that identity; conflicting explicit IDs or identities are
/// invalid.
/// Wildcards select a device that satisfies every control of the class; for the bound control, the
/// eligible device with the greatest magnitude wins, the lowest ID on ties. A binding matches an
/// identity through the events that carry it, so it follows its device to whatever ID the device
/// has after reconnection or a restart, and chooses among identical devices sharing the identity as
/// a wildcard does. Each class selects its device independently, so a keyboard modifier can qualify
/// a mouse button.
///
/// A binding outranks each binding of the same Context that reads the same bound control and
/// requires a strict subset of its modifiers, compared by kind and code. On a device the outranking
/// binding selects for the bound control, while it holds all its modifiers, the outranked binding
/// reads zero from that device. With S bound to one action and Ctrl+S to another, holding Ctrl and
/// S drives only the Ctrl+S action; a gamepad chord likewise takes a stick over from the stick's
/// unmodified binding. Precedence follows held controls, not press order: pressing Ctrl while S is
/// held releases the S binding without canceling it, and releasing Ctrl restores it. Bindings with
/// equal or partly shared modifier sets apply together, and no modifier is consumed: a binding
/// whose bound control is Ctrl still reads it. Delta bindings apply precedence as each increment
/// arrives: with the wheel bound alone and with Ctrl, an increment that arrives while Ctrl is held
/// counts only toward Ctrl + wheel.
struct Binding {
    Control control;
    /// Channel::y requires ActionType::vector2.
    Channel channel = Channel::x;
    /// Signed multiplier, in [-16, 16]; a negative scale inverts. For a delta binding it converts
    /// the converter's units into the action's, such as a look sensitivity.
    float scale = 1;
    /// Magnitude at or below which the control reads as zero, in [0, 1). Delta bindings require 0:
    /// the range describes a level, and a cut per increment would discard slow motion at a rate
    /// that depends on event timing.
    float deadzone = 0;
    /// Up to four keys, mouse buttons or gamepad buttons that must all be held, pressed before or
    /// after the bound control. No kind and code may repeat within the binding, including the
    /// bound control. Holding them outranks bindings of the same control that require only some
    /// of them; see Binding.
    std::vector<Control> modifiers{};
};
/// How an action combines its bindings' contributions.
///
/// An axis or vector2 action binds only held controls or only delta controls. A held control
/// reports a level, such as a stick's rate, which needs the frame time to become a displacement,
/// while an increment is already a displacement over the frame; a Context reads no clock, so no
/// single sum of both suits every frame rate. Combine a held action and a delta action with the
/// application's frame time instead.
enum class ActionType {
    /// The greatest positive contribution; the value is 1 while active and 0 otherwise. Held and
    /// delta bindings may mix.
    button,
    /// The sum of contributions, clamped to [-1, 1] for held controls. For delta controls it is
    /// not clamped, and only saturates at the largest finite float.
    axis,
    /// Per-channel sums. For held controls each is clamped to [-1, 1] and the vector normalized
    /// when longer than 1; for delta controls, neither, and each only saturates as an axis does.
    vector2
};
/// A named action and its bindings.
struct Action {
    /// Unique within the map: 1 to 128 printable ASCII characters, without spaces.
    std::string name;
    ActionType type = ActionType::button;
    /// Up to 32 bindings; an empty list leaves the action inactive. Those of an axis or vector2
    /// action are all held or all delta bindings.
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
///
/// An action that only increments keep active stays active for the rest of the frame they arrive
/// in: the next begin_frame() makes it inactive and latches #released in the new frame, so a
/// wheel-driven button presses in one frame and releases in the next.
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
    /// The control that changed, with its device's identity when the converter knows one, or for a
    /// disconnect the device class (from the kind) and device, ignoring the code and identity. Needs
    /// a concrete device ID; focus events ignore it.
    Control source;
    /// Control events: 0 or 1 for keys and buttons, [-1, 1] for gamepad axes, 0 meaning released;
    /// for delta controls, a finite increment of any magnitude, where 0 changes nothing. Focus
    /// events: 1 gained or 0 lost. Disconnects ignore it.
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
/// controls that some binding can use, at most 1,024 at once. Each delta binding keeps a float sum
/// for each device it accepted increments from, at most 1,024 sums in all at once, until the next
/// begin_frame(); a delta action's value therefore covers one begin_frame() interval, so read it
/// once per frame rather than in each fixed step. Contexts are independent: copies own separate
/// bindings, values, sums and latches.
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
    /// Clears every State::pressed, State::released and State::canceled latch, then zeroes the
    /// delta bindings' sums and reevaluates: an action that increments alone kept active becomes
    /// inactive and latches State::released. Held values and the activity they give stay. Call it
    /// once per frame before processing that frame's events.
    void begin_frame();
    /// Applies @p event, which validate(const Event &) must accept.
    ///
    /// A focus event calls set_focused(), even while disabled. A disconnect forgets the recorded
    /// controls of that device class and ID, and for a mouse the sums of its increments, and
    /// reevaluates, so other devices keep their state. A control event is ignored while disabled or
    /// unfocused. Otherwise a held control's value and identity replace the control's record on
    /// that device (zero releases the control, whatever either identity), a delta control's
    /// increment is added to the sum of each delta binding that accepts it (see Binding), and the
    /// actions are reevaluated. Throws `std::length_error` when a new control would exceed 1,024 recorded
    /// controls or a new sum would exceed 1,024 sums, and `std::overflow_error` when an increment
    /// would take a sum beyond the largest finite float. A rejected event changes nothing, nor does
    /// a failed allocation.
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
    /// Forgets every recorded control and every delta binding's sum, zeroes every value and clears
    /// State::pressed; each active action becomes inactive and latches State::released and
    /// State::canceled. Controls still held count again only after new events.
    void cancel();
    /// Replaces @p action's bindings, then cancels the context so actions need fresh input under
    /// the new configuration. Throws `std::out_of_range` for an unknown action and
    /// `std::invalid_argument` for invalid bindings, changing nothing.
    void rebind(std::string_view action, std::vector<Binding> bindings);

  private:
    friend struct anima::detail::InputStaging;
    // An action's combined value and activity, before they are published to its State.
    struct Combined {
        Value value;
        bool active{};
    };
    // Each delta binding's sum of increments per device since begin_frame(), keyed by the binding's
    // position among all the map's bindings, in order, and the device ID.
    using Sums = std::map<std::pair<std::size_t, std::uint32_t>, float>;
    // A validated event's effect on this context, with everything applying it allocates.
    struct Staged;
    std::size_t index(std::string_view name) const;
    void evaluate();
    [[nodiscard]] Combined combine(std::size_t action_index, std::size_t first_binding) const;
    void publish(std::size_t action_index, const Combined &combined);
    // Checks what process() would do with @p event after set_enabled(@p enable), throwing as it does, and
    // allocates what that needs, without changing the context.
    [[nodiscard]] Staged stage(const Event &event, bool enable) const;
    void stage_increment(Staged &staged) const;
    // Makes the changes stage() prepared, on the unchanged context it staged them for; allocates nothing.
    void apply(Staged &&staged) noexcept;
    float read(const Binding &binding) const;
    Map map_;
    std::vector<State> states_;
    std::map<Control, float> values_;
    Sums sums_;
    bool enabled_ = true, focused_ = true;
};
} // namespace anima::input
