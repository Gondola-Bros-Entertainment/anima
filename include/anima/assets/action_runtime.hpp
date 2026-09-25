#pragma once
#include <anima/assets/action.hpp>
#include <anima/assets/motion_runtime.hpp>
#include <anima/core/capabilities.hpp>
namespace anima {
struct ActionRequest {
    std::string action;
    std::uint64_t instance{};
    double elapsed{};
    std::optional<double> released_at;
    // Optional caller duration for timed actions. Held actions use their
    // declared phase timing and cannot use this fixed-time adapter.
    std::optional<double> duration;
};
struct ActionWeight {
    std::vector<std::pair<double, float>> keys{{0., 1.F}, {1., 1.F}};
    // Public curves may be supplied directly, independently of JSON decoding.
    // Require ordered normalized keys; finite sample phases clamp to the ends.
    float sample(double phase) const {
        if (!std::isfinite(phase) || keys.size() < 2 || keys.size() > 32 || keys.front().first != 0 ||
            keys.back().first != 1)
            throw std::invalid_argument("Action weight requires a finite phase and 2..32 keys covering 0..1");
        double previous = -1;
        for (const auto &[at, value] : keys) {
            if (!std::isfinite(at) || at <= previous || at < 0 || at > 1 || !std::isfinite(value) || value < 0 ||
                value > 1)
                throw std::invalid_argument("Action weight keys must be ordered and normalized");
            previous = at;
        }
        for (std::size_t i = 1; i < keys.size(); ++i)
            if (phase <= keys[i].first) {
                const auto [a, x] = keys[i - 1];
                const auto [b, y] = keys[i];
                return x + (y - x) * static_cast<float>(std::clamp((phase - a) / (b - a), 0., 1.));
            }
        return keys.back().second;
    }
};
struct ActionLayer {
    std::string clip, mask;
    std::array<double, 2> interval{0, 1};
    ActionWeight weight;
    anima::LayerMode mode = anima::LayerMode::override_pose;
    std::string reference;
    double reference_at{};
};
struct ActionPropTrack {
    std::string role, track;
    std::array<double, 2> interval{0, 1};
    bool required = true;
};
struct ActionPhaseBinding {
    std::vector<ActionLayer> layers;
    std::vector<ActionPropTrack> props;
    std::map<std::string, ActionWeight, std::less<>> contacts;
};
struct ActionDefinition {
    std::string id;
    anima::ActionTimeline timeline;
    std::vector<ActionPhaseBinding> phases;
    std::set<std::string, std::less<>> handling;
    std::map<std::string, Capabilities, std::less<>> required_roles;
};
struct ActionSample {
    anima::Pose pose;
    anima::ActionTime clock;
    std::string clip;
    struct Prop {
        std::string role, track;
        double progress{};
        bool required{};
    };
    std::vector<Prop> props;
    std::map<std::string, float, std::less<>> contacts;
};
class ActionRuntime {
  public:
    using Definitions = std::map<std::string, ActionDefinition, std::less<>>;
    ActionRuntime(std::shared_ptr<const Asset> asset, std::shared_ptr<const MotionRuntime> motion,
                  std::string_view document);
    const ActionDefinition &definition(std::string_view id) const;
    void validate_timing(std::string_view id, std::string_view handling, std::optional<double> windup,
                         double fixed_duration) const;
    std::string loadout_handling(std::string_view id, const std::map<std::string, std::string, std::less<>> &roles,
                                 std::string_view empty_handling) const;
    const Definitions &definitions() const;
    double scale(const ActionRequest &request) const;
    ActionSample sample(const Pose &base, const ActionRequest &request, std::string_view handling) const;
    static ActionWeight weight(std::string_view document);

  private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};
} // namespace anima
