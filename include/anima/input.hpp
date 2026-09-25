#pragma once
#include <compare>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace anima::input {
inline constexpr std::uint32_t any_device = UINT32_MAX;
enum class ControlKind { key, mouse_button, gamepad_button, gamepad_axis };
struct Control {
    ControlKind kind = ControlKind::key;
    std::uint16_t code{};
    std::uint32_t device = any_device; // Wildcard only in bindings, never events.
    auto operator<=>(const Control &) const = default;
};
enum class Channel { x, y };
struct Binding {
    Control control;
    Channel channel = Channel::x;
    float scale = 1, deadzone = 0; // Signed scale; axial deadzone remapped to [0,1].
};
enum class ActionType { button, axis, vector2 };
struct Action {
    std::string name;
    ActionType type = ActionType::button;
    std::vector<Binding> bindings;
    float threshold = .5F; // Activity threshold; buttons expose a binary value.
};
using Map = std::vector<Action>;
struct Value {
    float x{}, y{};
};
struct State {
    Value value;
    bool active{}, pressed{}, released{}, canceled{};
};
enum class EventType { control, focus, disconnect };
struct Event {
    EventType type = EventType::control;
    Control source;
    float value{}; // Control: digital 0/1 or axis [-1,1]. Focus: 0/1.
};
void validate(const Map &map);
void validate(const Event &event);
// Independent per-user/context state. No device IO, callbacks, clocks or game
// commands. Single caller thread. Copies own independent bindings and state.
class Context {
  public:
    explicit Context(Map map = {});
    Context(const Context &) = default;
    Context &operator=(const Context &other);
    Context(Context &&) noexcept = default;
    Context &operator=(Context &&) noexcept = default;
    [[nodiscard]] std::span<const Action> actions() const { return map_; }
    [[nodiscard]] State state(std::string_view action) const;
    void begin_frame(); // Clears edge latches only; call once before pumping events.
    void process(const Event &event);
    void set_enabled(bool enabled);
    void set_focused(bool focused);
    [[nodiscard]] bool enabled() const { return enabled_; }
    [[nodiscard]] bool focused() const { return focused_; }
    void cancel(); // Clears physical state and pending presses; releases active actions.
    // Transactional configuration change; cancels the context, requiring fresh input.
    void rebind(std::string_view action, std::vector<Binding> bindings);

  private:
    std::size_t index(std::string_view name) const;
    void evaluate();
    float read(Control control) const;
    Map map_;
    std::vector<State> states_;
    std::map<Control, float> values_;
    bool enabled_ = true, focused_ = true;
};
} // namespace anima::input
