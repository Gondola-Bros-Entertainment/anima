#include "../detail/input.hpp"
#include <algorithm>
#include <anima/input.hpp>
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
    const unsigned limit = c.kind == ControlKind::key              ? limits::key_code
                           : c.kind == ControlKind::mouse_button   ? limits::mouse_button_code
                           : c.kind == ControlKind::gamepad_button ? limits::gamepad_button_code
                                                                   : limits::gamepad_axis_code;
    if (c.code > limit || (c.kind == ControlKind::mouse_button && !c.code))
        throw std::invalid_argument("Input control code outside supported range");
}
ControlKind device_class(ControlKind value) {
    return value == ControlKind::gamepad_axis ? ControlKind::gamepad_button : value;
}
void action(const Action &a) {
    if (a.name.empty() || a.name.size() > limits::action_name_bytes || a.type < ActionType::button ||
        a.type > ActionType::vector2 || a.bindings.size() > limits::bindings_per_action ||
        !std::isfinite(a.threshold) || a.threshold < limits::minimum_threshold || a.threshold > 1)
        throw std::invalid_argument("Invalid input action name/type/bindings/threshold");
    for (unsigned char c : a.name)
        if (c < 33 || c > 126)
            throw std::invalid_argument("Input action names must be printable ASCII without spaces");
    for (const auto &b : a.bindings) {
        control(b.control);
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
float Context::read(Control c) const {
    if (c.device != any_device) {
        const auto it = values_.find(c);
        return it == values_.end() ? 0 : it->second;
    }
    float value{};
    c.device = 0;
    for (auto it = values_.lower_bound(c); it != values_.end() && it->first.kind == c.kind && it->first.code == c.code;
         ++it)
        if (std::abs(it->second) > std::abs(value))
            value = it->second;
    return value; // Greatest magnitude; ties choose the lowest device ID.
}
void Context::evaluate() {
    for (std::size_t i = 0; i < map_.size(); ++i) {
        const auto &a = map_[i];
        double x{}, y{};
        for (const auto &b : a.bindings) {
            const float raw = read(b.control);
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
            observed |= b.control.kind == e.source.kind && b.control.code == e.source.code &&
                        (b.control.device == any_device || b.control.device == e.source.device);
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
