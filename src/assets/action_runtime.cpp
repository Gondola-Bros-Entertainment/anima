#include "presentation_data.hpp"
#include <anima/assets/action_runtime.hpp>
namespace anima {
namespace {
// Authored and authoritative action durations match within this many seconds.
constexpr double timing_tolerance = 1e-6;
} // namespace
struct ActionRuntime::Impl {
  public:
    Impl(std::shared_ptr<const anima::Asset> asset, std::shared_ptr<const MotionRuntime> motion,
         const nlohmann::json &document)
        : asset_(std::move(asset)), motion_(std::move(motion)) {
        using namespace presentation_data;
        if (!asset_ || !motion_)
            throw std::invalid_argument("Actions require a bound motion resource");
        anima::detail::json_fields(document, {"schema_version", "actions"});
        if (document.at("schema_version") != 1 || !document.at("actions").is_array() ||
            document.at("actions").empty() || document.at("actions").size() > 4096)
            throw std::invalid_argument("Invalid action catalog");
        for (const auto &value : document.at("actions")) {
            anima::detail::json_fields(value, {"id", "handling", "phases"}, {"roles"});
            std::set<std::string, std::less<>> handling;
            if (!value.at("handling").is_array() || value.at("handling").empty())
                throw std::invalid_argument("Action needs explicit compatible handling");
            for (const auto &h : value.at("handling"))
                if (!handling.insert(text(h)).second)
                    throw std::invalid_argument("Duplicate action handling");
            std::vector<anima::ActionPhase> phases;
            std::vector<ActionPhaseBinding> bindings;
            if (!value.at("phases").is_array())
                throw std::invalid_argument("Action phases must be an array");
            for (const auto &p : value.at("phases")) {
                anima::detail::json_fields(p, {"id", "duration", "layers"}, {"held", "cues", "props", "contacts"});
                anima::ActionPhase phase{text(p.at("id")), p.at("duration").get<double>(), p.value("held", false), {}};
                if (p.contains("cues")) {
                    if (!p.at("cues").is_array())
                        throw std::invalid_argument("Action cues must be an array");
                    for (const auto &cue : p.at("cues")) {
                        anima::detail::json_fields(cue, {"id", "at"});
                        phase.cues.push_back({text(cue.at("id")), cue.at("at").get<double>()});
                    }
                }
                ActionPhaseBinding binding;
                if (!p.at("layers").is_array() || p.at("layers").empty() || p.at("layers").size() > 8)
                    throw std::invalid_argument("Action needs 1..8 pose layers per phase");
                bool full = false;
                for (const auto &l : p.at("layers")) {
                    anima::detail::json_fields(l, {"clip", "interval"}, {"mask", "mode", "weight", "reference"});
                    ActionLayer layer;
                    layer.clip = text(l.at("clip"));
                    layer.interval = interval(l.at("interval"));
                    layer.mask = l.value("mask", std::string{});
                    const auto mode = l.value("mode", std::string("override"));
                    if (mode != "override" && mode != "additive")
                        throw std::invalid_argument("Unknown action layer mode");
                    layer.mode = mode == "additive" ? anima::LayerMode::additive : anima::LayerMode::override_pose;
                    if (layer.mask.empty()) {
                        if (full || !binding.layers.empty() || mode != "override")
                            throw std::invalid_argument("Full-body action layer must be the first override");
                        full = true;
                    } else if (!motion_ || !motion_->has_mask(layer.mask))
                        throw std::invalid_argument("Unknown action layer mask");
                    if (l.contains("weight"))
                        layer.weight = weight(l.at("weight"));
                    (void)clip(layer.clip);
                    if (motion_->is_layer(layer.clip) && layer.mask != motion_->layer_mask(layer.clip))
                        throw std::invalid_argument("Action must respect handling resource ownership");
                    if (l.contains("reference")) {
                        anima::detail::json_fields(l.at("reference"), {"clip", "at"});
                        layer.reference = text(l.at("reference").at("clip"));
                        layer.reference_at = unit(l.at("reference").at("at"));
                        (void)clip(layer.reference);
                    }
                    if ((mode == "additive") != !layer.reference.empty())
                        throw std::invalid_argument("Additive action layer needs exactly one reference pose");
                    binding.layers.push_back(std::move(layer));
                }
                if (p.contains("props")) {
                    if (!p.at("props").is_array() || p.at("props").size() > 8)
                        throw std::invalid_argument("Invalid action prop tracks");
                    std::set<std::string> roles;
                    for (const auto &prop : p.at("props")) {
                        anima::detail::json_fields(prop, {"role", "track", "interval"}, {"required"});
                        ActionPropTrack track{text(prop.at("role")), text(prop.at("track")),
                                              interval(prop.at("interval")), prop.value("required", true)};
                        if (!roles.insert(track.role).second)
                            throw std::invalid_argument("Duplicate action prop role");
                        binding.props.push_back(std::move(track));
                    }
                }
                if (p.contains("contacts")) {
                    if (!p.at("contacts").is_object())
                        throw std::invalid_argument("Action contacts must be named weight curves");
                    for (const auto &[chain, curve] : p.at("contacts").items()) {
                        if (!motion_)
                            throw std::invalid_argument("Action contacts need a motion rig");
                        (void)motion_->contact_end_node(chain);
                        binding.contacts.emplace(chain, weight(curve));
                    }
                }
                phases.push_back(std::move(phase));
                bindings.push_back(std::move(binding));
            }
            ActionDefinition action{text(value.at("id")),
                                    anima::ActionTimeline(std::move(phases)),
                                    std::move(bindings),
                                    std::move(handling),
                                    {}};
            if (value.contains("roles")) {
                if (!value.at("roles").is_object() || value.at("roles").size() > 8)
                    throw std::invalid_argument("Invalid required action roles");
                for (const auto &[role, profiles] : value.at("roles").items()) {
                    if (role.empty() || !profiles.is_array() || profiles.empty())
                        throw std::invalid_argument("Action role requires handling profiles");
                    for (const auto &profile : profiles)
                        if (!action.required_roles[role].insert(text(profile)).second)
                            throw std::invalid_argument("Duplicate role handling profile");
                }
            }
            const auto id = action.id;
            if (!actions_.emplace(id, std::move(action)).second)
                throw std::invalid_argument("Duplicate action ID");
        }
    }

    const ActionDefinition &definition(std::string_view id) const {
        const auto found = actions_.find(id);
        if (found == actions_.end())
            throw std::invalid_argument("Unknown action: " + std::string(id));
        return found->second;
    }
    // Generic caller timing contract: no inventory, damage, class or network rules.
    void validate_timing(std::string_view id, std::string_view handling, std::optional<double> windup,
                         double fixed_duration) const {
        const auto &action = definition(id);
        if (!std::isfinite(fixed_duration) || fixed_duration <= 0 ||
            (windup && (!std::isfinite(*windup) || *windup < 0)) || action.timeline.held() != bool(windup) ||
            !action.handling.contains(handling))
            throw std::invalid_argument("Gameplay and presentation action modes/handling disagree");
        if (!windup)
            return; // Existing timed performances use the duration adapter.
        double before = 0, duration = 0;
        bool held = false;
        for (const auto &phase : action.timeline.phases()) {
            if (phase.held) {
                held = true;
                continue;
            }
            if (!held)
                before += phase.duration;
            duration += phase.duration;
        }
        if (std::abs(before - *windup) > timing_tolerance || std::abs(duration - fixed_duration) > timing_tolerance)
            throw std::invalid_argument("Held action windup/recovery must match authoritative timing");
    }
    std::string loadout_handling(std::string_view id, const std::map<std::string, std::string, std::less<>> &roles,
                                 std::string_view empty_handling) const {
        const auto &action = definition(id);
        for (const auto &[role, profiles] : action.required_roles) {
            const auto found = roles.find(role);
            if (found == roles.end() || !profiles.contains(found->second))
                throw std::invalid_argument("Missing or incompatible required action role: " + role);
        }
        if (roles.empty() && action.handling.contains(empty_handling))
            return std::string(empty_handling);
        for (const auto &[role, profile] : roles) {
            (void)role;
            if (action.handling.contains(profile))
                return profile;
        }
        throw std::invalid_argument("No equipped handling can perform this action");
    }
    const auto &definitions() const { return actions_; }
    double scale(const ActionRequest &request) const {
        const auto &action = definition(request.action);
        if (!request.instance)
            throw std::invalid_argument("Action needs a nonzero instance ID");
        if (!request.duration)
            return 1;
        if (action.timeline.held() || !std::isfinite(*request.duration) || *request.duration <= 0)
            throw std::invalid_argument("Invalid fixed action duration");
        return action.timeline.duration() / *request.duration;
    }
    ActionSample sample(const anima::Pose &base, const ActionRequest &request, std::string_view handling) const {
        const auto &action = definition(request.action);
        if (!action.handling.contains(handling))
            throw std::invalid_argument("Action is incompatible with this handling profile");
        const auto rate = scale(request);
        const auto released = request.released_at ? std::optional(*request.released_at * rate) : std::nullopt;
        ActionSample result{base, action.timeline.sample(request.elapsed * rate, released), {}, {}, {}};
        const auto &phase = action.phases.at(result.clock.phase);
        for (const auto &layer : phase.layers) {
            const double time =
                std::lerp(layer.interval[0], layer.interval[1], result.clock.progress) * clip(layer.clip).duration;
            const float amount = layer.weight.sample(result.clock.progress);
            if (layer.mask.empty()) {
                const auto contribution = motion_->sample(layer.clip, time);
                result.pose = motion_->blend(result.pose, contribution, amount);
            } else {
                MotionControls controls;
                controls.layers.push_back(
                    {layer.clip, layer.mask, time, amount, layer.mode, layer.reference,
                     layer.reference.empty() ? 0 : layer.reference_at * clip(layer.reference).duration});
                result.pose = motion_->evaluate(result.pose, controls).pose;
            }
            if (result.clip.empty())
                result.clip = layer.clip;
        }
        for (const auto &track : phase.props)
            result.props.push_back({track.role, track.track,
                                    std::lerp(track.interval[0], track.interval[1], result.clock.progress),
                                    track.required});
        for (const auto &[chain, curve] : phase.contacts)
            result.contacts[chain] = curve.sample(result.clock.progress);
        return result;
    }

  private:
    const anima::Animation &clip(std::string_view name) const { return motion_->clip(name); }
    static double unit(const nlohmann::json &value) {
        const auto n = value.get<double>();
        if (!std::isfinite(n) || n < 0 || n > 1)
            throw std::invalid_argument("Action interval/weight must be 0..1");
        return n;
    }
    static std::array<double, 2> interval(const nlohmann::json &value) {
        if (!value.is_array() || value.size() != 2)
            throw std::invalid_argument("Action clip interval needs two normalized endpoints");
        // Reversed intervals intentionally support authored reverse playback.
        return {unit(value.at(0)), unit(value.at(1))};
    }

  public:
    // Shared phase curves are also used by coordinated actor contacts.
    static ActionWeight weight(const nlohmann::json &value) {
        ActionWeight result;
        result.keys.clear();
        if (!value.is_array() || value.size() < 2 || value.size() > 32)
            throw std::invalid_argument("Action weight needs 2..32 keys");
        double previous = -1;
        for (const auto &key : value) {
            const auto point = interval(key);
            if (point[0] <= previous)
                throw std::invalid_argument("Action weight keys must increase");
            previous = point[0];
            result.keys.emplace_back(point[0], static_cast<float>(point[1]));
        }
        if (result.keys.front().first != 0 || result.keys.back().first != 1)
            throw std::invalid_argument("Action weight must cover 0..1");
        return result;
    }

  private:
    std::shared_ptr<const anima::Asset> asset_;
    std::shared_ptr<const MotionRuntime> motion_;
    std::map<std::string, ActionDefinition, std::less<>> actions_;
};
ActionRuntime::ActionRuntime(std::shared_ptr<const Asset> asset, std::shared_ptr<const MotionRuntime> motion,
                             std::string_view document)
    : impl_(detail::json_step([&] {
          return std::make_shared<Impl>(std::move(asset), std::move(motion), presentation_data::parse(document));
      })) {}
ActionWeight ActionRuntime::weight(std::string_view document) {
    return detail::json_step([&] { return Impl::weight(presentation_data::parse(document)); });
}
const ActionDefinition &ActionRuntime::definition(std::string_view id) const { return impl_->definition(id); }
void ActionRuntime::validate_timing(std::string_view id, std::string_view handling, std::optional<double> windup,
                                    double fixed_duration) const {
    return impl_->validate_timing(id, handling, windup, fixed_duration);
}
std::string ActionRuntime::loadout_handling(std::string_view id,
                                            const std::map<std::string, std::string, std::less<>> &roles,
                                            std::string_view empty_handling) const {
    return impl_->loadout_handling(id, roles, empty_handling);
}
const ActionRuntime::Definitions &ActionRuntime::definitions() const { return impl_->definitions(); }
double ActionRuntime::scale(const ActionRequest &request) const { return impl_->scale(request); }
ActionSample ActionRuntime::sample(const Pose &base, const ActionRequest &request, std::string_view handling) const {
    return impl_->sample(base, request, handling);
}
} // namespace anima
