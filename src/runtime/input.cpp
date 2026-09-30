#include "../detail/input.hpp"
#include <algorithm>
#include <anima/input.hpp>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace anima::input {
namespace {
void kind(ControlKind value) {
    if (value < ControlKind::key || value > limits::last_kind)
        throw std::invalid_argument("Unknown input control kind");
}
void control(Control c) {
    kind(c.kind);
    if (!limits::in_range(c))
        throw std::invalid_argument("Input control code outside supported range");
}
ControlKind device_class(ControlKind value) {
    if (value == ControlKind::gamepad_axis)
        return ControlKind::gamepad_button;
    return limits::delta(value) ? ControlKind::mouse_button : value;
}
constexpr DeviceIdentity no_identity{};
// Recorded control values, keyed by each control's latest event source, identity included, with at most one entry
// per kind, code and device.
using Values = std::map<Control, float>;
// The device classes, each named by its first control kind.
constexpr std::array device_classes{ControlKind::key, ControlKind::mouse_button, ControlKind::gamepad_button};
// What a binding requires of the device it reads in one class: an explicit ID or any_device, and an identity or none,
// each taken from its control or a modifier of that class, which validation keeps consistent.
struct Selection {
    std::uint32_t device = any_device;
    DeviceIdentity identity{};
    [[nodiscard]] bool admits(const Control &recorded) const {
        return (device == any_device || recorded.device == device) &&
               (identity == no_identity || recorded.identity == identity);
    }
};
Selection selection(const Binding &binding, ControlKind group) {
    Selection result;
    const auto take = [&](const Control &selector) {
        if (device_class(selector.kind) != group)
            return;
        if (selector.device != any_device)
            result.device = selector.device;
        if (selector.identity != no_identity)
            result.identity = selector.identity;
    };
    take(binding.control);
    for (const auto &modifier : binding.modifiers)
        take(modifier);
    return result;
}
// The entry of @p recorded_values that records @p control's kind and code on @p device, whatever identity its event
// carried, or its end.
template <class Recorded> auto entry(Recorded &recorded_values, Control control, std::uint32_t device) {
    control.device = device;
    control.identity = no_identity;
    const auto it = recorded_values.lower_bound(control);
    return it != recorded_values.end() && it->first.kind == control.kind && it->first.code == control.code &&
                   it->first.device == device
               ? it
               : recorded_values.end();
}
// Whether every modifier of @p binding in class @p group is recorded on @p device and admitted by @p required.
bool held_on(const Values &values, const Binding &binding, ControlKind group, std::uint32_t device,
             const Selection &required) {
    for (const auto &modifier : binding.modifiers) {
        if (device_class(modifier.kind) != group)
            continue;
        const auto it = entry(values, modifier, device);
        if (it == values.end() || !required.admits(it->first))
            return false;
    }
    return true;
}
// Whether every modifier of @p binding outside its bound control's class is held, those of each class together on
// one device that class selects.
bool other_modifiers_held(const Values &values, const Binding &binding) {
    for (auto group : device_classes) {
        const auto first = std::ranges::find_if(
            binding.modifiers, [group](Control modifier) { return device_class(modifier.kind) == group; });
        if (group == device_class(binding.control.kind) || first == binding.modifiers.end())
            continue;
        const auto required = selection(binding, group);
        if (required.device != any_device) {
            if (!held_on(values, binding, group, required.device, required))
                return false;
            continue;
        }
        auto candidate = *first;
        candidate.device = 0;
        candidate.identity = no_identity;
        bool accepted = false;
        for (auto it = values.lower_bound(candidate);
             !accepted && it != values.end() && it->first.kind == candidate.kind && it->first.code == candidate.code;
             ++it)
            accepted = held_on(values, binding, group, it->first.device, required);
        if (!accepted)
            return false;
    }
    return true;
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
// Whether a binding of @p map outranks @p binding on the device of @p bound, the record of its bound control: one
// that reads the same bound control, extends the binding's modifiers, admits that record and holds all its
// modifiers there.
bool outranked(const Map &map, const Values &values, const Binding &binding, const Control &bound) {
    for (const auto &action : map)
        for (const auto &other : action.bindings) {
            if (other.control.kind != binding.control.kind || other.control.code != binding.control.code ||
                !extends(other, binding))
                continue;
            const auto group = device_class(other.control.kind);
            const auto required = selection(other, group);
            if (required.admits(bound) && held_on(values, other, group, bound.device, required) &&
                other_modifiers_held(values, other))
                return true;
        }
    return false;
}
void compatible(Control first, Control second) {
    if (first.kind == second.kind && first.code == second.code)
        throw std::invalid_argument("Repeated input chord control");
    if (device_class(first.kind) == device_class(second.kind) &&
        ((first.device != any_device && second.device != any_device && first.device != second.device) ||
         (first.identity != no_identity && second.identity != no_identity && first.identity != second.identity)))
        throw std::invalid_argument("Input chord device selectors conflict");
}
bool observes(const Binding &binding, const Control &source) {
    const auto same_device = [&source](const Control &selector) {
        return device_class(selector.kind) != device_class(source.kind) ||
               ((selector.device == any_device || selector.device == source.device) &&
                (selector.identity == no_identity || selector.identity == source.identity));
    };
    const auto matches = [&](const Control &selector) {
        return selector.kind == source.kind && selector.code == source.code && same_device(selector);
    };
    if (!same_device(binding.control))
        return false;
    bool matched = matches(binding.control);
    for (const auto &modifier : binding.modifiers) {
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
    bool held = false, delta = false;
    for (const auto &b : a.bindings) {
        control(b.control);
        if (b.modifiers.size() > limits::modifiers_per_binding)
            throw std::invalid_argument("Input binding exceeds four modifiers");
        for (std::size_t i = 0; i < b.modifiers.size(); ++i) {
            const auto modifier = b.modifiers[i];
            control(modifier);
            if (modifier.kind == ControlKind::gamepad_axis || limits::delta(modifier.kind))
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
        if (limits::delta(b.control.kind) && b.deadzone != 0)
            throw std::invalid_argument("Input delta bindings require a zero deadzone");
        (limits::delta(b.control.kind) ? delta : held) = true;
    }
    if (a.type != ActionType::button && held && delta)
        throw std::invalid_argument("Input axis and vector2 actions cannot mix held and delta controls");
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
    // A delta control's increment may have any finite magnitude.
    if (!std::isfinite(e.value) ||
        (!limits::delta(e.source.kind) &&
         (std::abs(e.value) > 1 || (e.source.kind != ControlKind::gamepad_axis && e.value != 0 && e.value != 1))))
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
    if (sums_.empty())
        return;
    sums_.clear();
    evaluate();
}
void Context::cancel() {
    values_.clear();
    sums_.clear();
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
    const auto required = selection(binding, primary_class);
    auto control = binding.control;
    control.device = required.device == any_device ? 0 : required.device;
    control.identity = no_identity;
    float value{};
    for (auto it = values_.lower_bound(control);
         it != values_.end() && it->first.kind == control.kind && it->first.code == control.code &&
         (required.device == any_device || it->first.device == required.device);
         ++it)
        if (std::abs(it->second) > std::abs(value) && required.admits(it->first) &&
            held_on(values_, binding, primary_class, it->first.device, required) &&
            !outranked(map_, values_, binding, it->first))
            value = it->second;
    return value; // Greatest eligible magnitude; ties choose the lowest device ID.
}
Context::Combined Context::combine(std::size_t action_index, std::size_t first_binding) const {
    const auto &a = map_[action_index];
    double x{}, y{};
    bool delta = false;
    for (std::size_t j = 0; j < a.bindings.size(); ++j) {
        const auto &b = a.bindings[j];
        double adjusted{};
        if (limits::delta(b.control.kind)) {
            delta = true;
            // Summed and scaled in double, so a sum near the float limit cannot overflow before saturation below.
            double sum{};
            const auto slot = first_binding + j;
            for (auto it = sums_.lower_bound({slot, 0}); it != sums_.end() && it->first.first == slot; ++it)
                sum += it->second;
            adjusted = sum * b.scale;
        } else {
            const float raw = read(b);
            const float level = std::copysign(std::max(0.F, std::abs(raw) - b.deadzone) / (1 - b.deadzone), raw);
            adjusted = double(level) * b.scale; // Exact: a double holds any product of two floats.
        }
        if (a.type == ActionType::button)
            x = std::max(x, adjusted);
        else if (b.channel == Channel::x)
            x += adjusted;
        else
            y += adjusted;
    }
    // Held controls report bounded levels, so their sums are clamped and normalized. Increments are displacements,
    // whose sums are neither, short of saturating at the largest float. A button combines either kind into 0 or 1.
    const bool bounded = a.type == ActionType::button || !delta;
    if (bounded) {
        x = std::clamp(x, -1.0, 1.0);
        y = std::clamp(y, -1.0, 1.0);
    }
    const auto magnitude = std::sqrt(x * x + y * y);
    if (bounded && magnitude > 1) {
        x /= magnitude;
        y /= magnitude;
    }
    const bool active = std::min(magnitude, 1.0) >= a.threshold;
    if (a.type == ActionType::button)
        x = active ? 1 : 0;
    constexpr double largest = std::numeric_limits<float>::max();
    return {
        {static_cast<float>(std::clamp(x, -largest, largest)), static_cast<float>(std::clamp(y, -largest, largest))},
        active};
}
void Context::publish(std::size_t action_index, const Combined &combined) {
    auto &s = states_[action_index];
    s.pressed |= combined.active && !s.active;
    s.released |= !combined.active && s.active;
    s.active = combined.active;
    s.value = combined.value;
}
void Context::evaluate() {
    std::size_t first_binding = 0;
    for (std::size_t i = 0; i < map_.size(); ++i) {
        publish(i, combine(i, first_binding));
        first_binding += map_[i].bindings.size();
    }
}
void Context::stage_increment(Staged &staged) const {
    const auto &e = staged.event;
    if (e.value == 0)
        return;
    const auto group = device_class(e.source.kind);
    std::size_t first_binding = 0;
    for (std::size_t i = 0; i < map_.size(); ++i) {
        const auto &bindings = map_[i].bindings;
        for (std::size_t j = 0; j < bindings.size(); ++j) {
            const auto &b = bindings[j];
            if (b.control.kind != e.source.kind || b.control.code != e.source.code)
                continue;
            // The chord and its precedence gate the increment now, as it arrives.
            const auto required = selection(b, group);
            if (!required.admits(e.source) || !held_on(values_, b, group, e.source.device, required) ||
                !other_modifiers_held(values_, b) || outranked(map_, values_, b, e.source))
                continue;
            const auto slot = first_binding + j;
            const auto recorded = sums_.find({slot, e.source.device});
            const double after = double(recorded == sums_.end() ? 0.F : recorded->second) + e.value;
            if (std::abs(after) > std::numeric_limits<float>::max())
                throw std::overflow_error("Input increment would overflow a delta binding's sum");
            // New sums are built apart, so a failed allocation changes nothing, and merging them allocates nothing.
            if (recorded == sums_.end())
                staged.started.emplace(std::pair{slot, e.source.device}, static_cast<float>(after));
            staged.accepted.push_back({i, first_binding, slot, static_cast<float>(after)});
        }
        first_binding += bindings.size();
    }
    if (sums_.size() + staged.started.size() > limits::delta_accumulators)
        throw std::length_error("Input context exceeds 1024 delta sums");
}
Context::Staged Context::stage(const Event &e, bool enable) const {
    Staged staged{e, enable, {}, {}, {}};
    // Only a control event can be rejected, and a disabled or unfocused context ignores it.
    if (e.type != EventType::control || !enable || !focused_)
        return staged;
    if (limits::delta(e.source.kind)) {
        stage_increment(staged);
        return staged;
    }
    // Releasing or updating a recorded control needs nothing new; see apply().
    const auto previous = entry(values_, e.source, e.source.device);
    if (e.value == 0 || (previous != values_.end() && previous->first == e.source))
        return staged;
    bool observed = false;
    for (const auto &a : map_)
        for (const auto &b : a.bindings)
            observed |= observes(b, e.source);
    if (!observed)
        return staged;
    if (previous == values_.end() && values_.size() == limits::active_controls)
        throw std::length_error("Input context exceeds 1024 active physical controls");
    staged.record.emplace(e.source, e.value);
    return staged;
}
void Context::apply(Staged &&staged) noexcept {
    const auto &e = staged.event;
    // Enabling changes nothing that staging read, and disabling makes a control event change nothing.
    set_enabled(staged.enabled);
    if (e.type == EventType::focus) {
        set_focused(e.value != 0);
        return;
    }
    if (e.type == EventType::disconnect) {
        const auto group = device_class(e.source.kind);
        std::erase_if(values_, [&](const auto &entry) {
            return device_class(entry.first.kind) == group && entry.first.device == e.source.device;
        });
        // Every sum belongs to a delta binding, whose controls are in the mouse class.
        if (group == device_class(ControlKind::mouse_motion))
            std::erase_if(sums_, [&](const auto &sum) { return sum.first.second == e.source.device; });
        evaluate();
        return;
    }
    if (!enabled_ || !focused_)
        return;
    if (limits::delta(e.source.kind)) {
        for (const auto &a : staged.accepted)
            if (const auto recorded = sums_.find({a.slot, e.source.device}); recorded != sums_.end())
                recorded->second = a.after;
        sums_.merge(staged.started);
        // Only the accepting actions changed; they arrive in map order.
        const auto &accepted = staged.accepted;
        for (std::size_t k = 0; k < accepted.size(); ++k)
            if (k == 0 || accepted[k].action_index != accepted[k - 1].action_index)
                publish(accepted[k].action_index, combine(accepted[k].action_index, accepted[k].first_binding));
        return;
    }
    // A recorded control is released or updated whatever identity the event carries, so a release that arrives
    // without the identity of the press still releases it.
    const auto previous = entry(values_, e.source, e.source.device);
    if (e.value == 0) {
        if (previous == values_.end())
            return;
        values_.erase(previous);
    } else if (previous != values_.end() && previous->first == e.source)
        previous->second = e.value;
    else {
        // Staging recorded the control only if a binding observes it.
        if (previous == values_.end() && staged.record.empty())
            return;
        if (previous != values_.end())
            values_.erase(previous);
        // A control that now reports an identity no binding accepts is released, not recorded.
        values_.merge(staged.record);
    }
    evaluate();
}
void Context::process(const Event &e) {
    validate(e);
    apply(stage(e, enabled_));
}
} // namespace anima::input
