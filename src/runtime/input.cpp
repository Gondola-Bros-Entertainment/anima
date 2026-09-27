#include "../detail/input.hpp"
#include <algorithm>
#include <anima/input.hpp>
#include <array>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace anima::input {
namespace {
void kind(ControlKind value) {
    if (value < ControlKind::key || value > ControlKind::gamepad_axis)
        throw std::invalid_argument("Unknown input control kind");
}
void control(Control c) {
    kind(c.kind);
    if (!limits::in_range(c))
        throw std::invalid_argument("Input control code outside supported range");
}
ControlKind device_class(ControlKind value) {
    return value == ControlKind::gamepad_axis ? ControlKind::gamepad_button : value;
}
// The device classes, each named by its first control kind.
constexpr std::array device_classes{ControlKind::key, ControlKind::mouse_button, ControlKind::gamepad_button};
// The device @p binding selects in class @p group: an explicit ID on its control or a modifier of that class, which
// validation keeps consistent, or any_device.
std::uint32_t selected_device(const Binding &binding, ControlKind group) {
    auto selected = device_class(binding.control.kind) == group ? binding.control.device : any_device;
    for (auto modifier : binding.modifiers)
        if (device_class(modifier.kind) == group && modifier.device != any_device)
            selected = modifier.device;
    return selected;
}
// Whether every modifier of @p binding in class @p group is recorded on @p device.
bool held_on(const std::map<Control, float> &values, const Binding &binding, ControlKind group, std::uint32_t device) {
    for (auto modifier : binding.modifiers) {
        if (device_class(modifier.kind) != group)
            continue;
        modifier.device = device;
        if (!values.contains(modifier))
            return false;
    }
    return true;
}
// Whether every modifier of @p binding outside its bound control's class is held, those of each class together on
// one device that class selects.
bool other_modifiers_held(const std::map<Control, float> &values, const Binding &binding) {
    for (auto group : device_classes) {
        const auto first = std::ranges::find_if(
            binding.modifiers, [group](Control modifier) { return device_class(modifier.kind) == group; });
        if (group == device_class(binding.control.kind) || first == binding.modifiers.end())
            continue;
        if (const auto selected = selected_device(binding, group); selected != any_device) {
            if (!held_on(values, binding, group, selected))
                return false;
            continue;
        }
        auto candidate = *first;
        candidate.device = 0;
        bool accepted = false;
        for (auto it = values.lower_bound(candidate);
             !accepted && it != values.end() && it->first.kind == candidate.kind && it->first.code == candidate.code;
             ++it)
            accepted = held_on(values, binding, group, it->first.device);
        if (!accepted)
            return false;
    }
    return true;
}
// Whether every modifier of @p binding is held: those of its bound control's class on @p device, which must be one
// that class selects, and the others as other_modifiers_held() requires.
bool modifiers_held(const std::map<Control, float> &values, const Binding &binding, std::uint32_t device) {
    return held_on(values, binding, device_class(binding.control.kind), device) &&
           other_modifiers_held(values, binding);
}
// Whether @p outer requires every modifier of @p inner and more, compared by kind and code.
bool extends(const Binding &outer, const Binding &inner) {
    return outer.modifiers.size() > inner.modifiers.size() &&
           std::ranges::all_of(inner.modifiers, [&outer](Control modifier) {
               return std::ranges::any_of(outer.modifiers, [modifier](Control other) {
                   return other.kind == modifier.kind && other.code == modifier.code;
               });
           });
}
// Whether a binding of @p map outranks @p binding on @p device: it reads the same bound control, extends the
// binding's modifiers, selects @p device for that control and holds all its modifiers there.
bool outranked(const Map &map, const std::map<Control, float> &values, const Binding &binding, std::uint32_t device) {
    for (const auto &action : map)
        for (const auto &other : action.bindings) {
            if (other.control.kind != binding.control.kind || other.control.code != binding.control.code ||
                !extends(other, binding))
                continue;
            const auto selected = selected_device(other, device_class(other.control.kind));
            if ((selected == any_device || selected == device) && modifiers_held(values, other, device))
                return true;
        }
    return false;
}
void compatible(Control first, Control second) {
    if (first.kind == second.kind && first.code == second.code)
        throw std::invalid_argument("Repeated input chord control");
    if (device_class(first.kind) == device_class(second.kind) && first.device != any_device &&
        second.device != any_device && first.device != second.device)
        throw std::invalid_argument("Input chord device selectors conflict");
}
bool observes(const Binding &binding, Control source) {
    const auto same_device = [source](Control selector) {
        return device_class(selector.kind) != device_class(source.kind) || selector.device == any_device ||
               selector.device == source.device;
    };
    const auto matches = [source](Control selector) {
        return selector.kind == source.kind && selector.code == source.code &&
               (selector.device == any_device || selector.device == source.device);
    };
    if (!same_device(binding.control))
        return false;
    bool matched = matches(binding.control);
    for (auto modifier : binding.modifiers) {
        if (!same_device(modifier))
            return false;
        matched |= matches(modifier);
    }
    return matched;
}
void action(const Action &a) {
    if (a.name.empty() || a.name.size() > limits::action_name_bytes || a.type < ActionType::button ||
        a.type > ActionType::vector2 || a.bindings.size() > limits::bindings_per_action ||
        !std::isfinite(a.threshold) || a.threshold < limits::minimum_threshold || a.threshold > 1)
        throw std::invalid_argument("Invalid input action name/type/bindings/threshold");
    for (unsigned char c : a.name)
        if (c < '!' || c > '~')
            throw std::invalid_argument("Input action names must be printable ASCII without spaces");
    for (const auto &b : a.bindings) {
        control(b.control);
        if (b.modifiers.size() > limits::modifiers_per_binding)
            throw std::invalid_argument("Input binding exceeds four modifiers");
        for (std::size_t i = 0; i < b.modifiers.size(); ++i) {
            const auto modifier = b.modifiers[i];
            control(modifier);
            if (modifier.kind == ControlKind::gamepad_axis)
                throw std::invalid_argument("Input chord modifiers must be digital");
            compatible(b.control, modifier);
            for (std::size_t previous = 0; previous < i; ++previous)
                compatible(b.modifiers[previous], modifier);
        }
        if (b.channel < Channel::x || b.channel > Channel::y ||
            (a.type != ActionType::vector2 && b.channel != Channel::x) || !std::isfinite(b.scale) ||
            std::abs(b.scale) > limits::maximum_binding_scale || !std::isfinite(b.deadzone) || b.deadzone < 0 ||
            b.deadzone >= 1)
            throw std::invalid_argument("Invalid input binding channel/scale/deadzone");
    }
}
} // namespace
void validate(const Map &map) {
    if (map.size() > limits::actions)
        throw std::invalid_argument("Input map exceeds 128 actions");
    std::set<std::string_view> names;
    for (const auto &a : map) {
        action(a);
        if (!names.insert(a.name).second)
            throw std::invalid_argument("Duplicate input action name");
    }
}
void validate(const Event &e) {
    if (e.type == EventType::focus) {
        if (e.value != 0 && e.value != 1)
            throw std::invalid_argument("Input focus must be 0 or 1");
        return;
    }
    if (e.type != EventType::control && e.type != EventType::disconnect)
        throw std::invalid_argument("Unknown input event type");
    kind(e.source.kind);
    if (e.source.device == any_device)
        throw std::invalid_argument("Input events require a concrete device identity");
    if (e.type == EventType::disconnect)
        return; // Code/value are not used; both gamepad control kinds name the same device class.
    control(e.source);
    if (!std::isfinite(e.value) || std::abs(e.value) > 1 ||
        (e.source.kind != ControlKind::gamepad_axis && e.value != 0 && e.value != 1))
        throw std::invalid_argument("Invalid input control value");
}
Context::Context(Map map) : map_(std::move(map)) {
    validate(map_);
    states_.resize(map_.size());
}
Context &Context::operator=(const Context &other) {
    if (this != &other) {
        Context copy(other);
        *this = std::move(copy);
    }
    return *this;
}
std::size_t Context::index(std::string_view name) const {
    for (std::size_t i = 0; i < map_.size(); ++i)
        if (map_[i].name == name)
            return i;
    throw std::out_of_range("Unknown input action");
}
State Context::state(std::string_view name) const { return states_[index(name)]; }
void Context::begin_frame() {
    for (auto &s : states_)
        s.pressed = s.released = s.canceled = false;
}
void Context::cancel() {
    values_.clear();
    for (auto &s : states_) {
        s.released |= s.active;
        s.canceled |= s.active;
        s.active = s.pressed = false;
        s.value = {};
    }
}
void Context::set_enabled(bool value) {
    if (enabled_ == value)
        return;
    if (!value)
        cancel();
    enabled_ = value;
}
void Context::set_focused(bool value) {
    if (focused_ == value)
        return;
    if (!value)
        cancel();
    focused_ = value;
}
void Context::rebind(std::string_view name, std::vector<Binding> bindings) {
    const auto i = index(name);
    auto candidate = map_[i];
    candidate.bindings = std::move(bindings);
    action(candidate);
    map_[i].bindings.swap(candidate.bindings);
    cancel();
}
float Context::read(const Binding &binding) const {
    // Other classes choose their own device, once per binding.
    if (!other_modifiers_held(values_, binding))
        return 0;
    const auto primary_class = device_class(binding.control.kind);
    const auto selected = selected_device(binding, primary_class);
    auto control = binding.control;
    control.device = selected == any_device ? 0 : selected;
    float value{};
    for (auto it = values_.lower_bound(control);
         it != values_.end() && it->first.kind == control.kind && it->first.code == control.code &&
         (selected == any_device || it->first.device == selected);
         ++it)
        if (std::abs(it->second) > std::abs(value) && held_on(values_, binding, primary_class, it->first.device) &&
            !outranked(map_, values_, binding, it->first.device))
            value = it->second;
    return value; // Greatest eligible magnitude; ties choose the lowest device ID.
}
void Context::evaluate() {
    for (std::size_t i = 0; i < map_.size(); ++i) {
        const auto &a = map_[i];
        double x{}, y{};
        for (const auto &b : a.bindings) {
            const float raw = read(b);
            const float adjusted =
                std::copysign(std::max(0.F, std::abs(raw) - b.deadzone) / (1 - b.deadzone), raw) * b.scale;
            if (a.type == ActionType::button)
                x = std::max(x, double(adjusted));
            else if (b.channel == Channel::x)
                x += adjusted;
            else
                y += adjusted;
        }
        x = std::clamp(x, -1.0, 1.0);
        y = std::clamp(y, -1.0, 1.0);
        const auto magnitude = std::sqrt(x * x + y * y);
        if (magnitude > 1) {
            x /= magnitude;
            y /= magnitude;
        }
        const bool active = std::min(magnitude, 1.0) >= a.threshold;
        if (a.type == ActionType::button)
            x = active ? 1 : 0;
        auto &s = states_[i];
        s.pressed |= active && !s.active;
        s.released |= !active && s.active;
        s.active = active;
        s.value = {static_cast<float>(x), static_cast<float>(y)};
    }
}
void Context::process(const Event &e) {
    validate(e);
    if (e.type == EventType::focus) {
        set_focused(e.value != 0);
        return;
    }
    if (e.type == EventType::disconnect) {
        std::erase_if(values_, [&](const auto &entry) {
            return device_class(entry.first.kind) == device_class(e.source.kind) &&
                   entry.first.device == e.source.device;
        });
        evaluate();
        return;
    }
    if (!enabled_ || !focused_)
        return;
    bool observed = false;
    for (const auto &a : map_)
        for (const auto &b : a.bindings)
            observed |= observes(b, e.source);
    if (!observed)
        return;
    if (e.value == 0)
        values_.erase(e.source);
    else {
        if (!values_.contains(e.source) && values_.size() == limits::active_controls)
            throw std::length_error("Input context exceeds 1024 active physical controls");
        values_.insert_or_assign(e.source, e.value);
    }
    evaluate();
}
} // namespace anima::input
