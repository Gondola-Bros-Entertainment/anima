#include "../detail/json.hpp"
#include <algorithm>
#include <anima/animation_state_machine.hpp>
#include <anima/assets/motion_runtime.hpp>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace anima {
namespace detail {
// A validated machine, indexed for evaluation.
struct AnimationStateMachineData {
    struct Clip {
        std::string name;
        // Borrowed from the machine's retained source asset, or from its retained motion runtime.
        const Animation *animation{};
        bool loop{};
        std::vector<ClipEvent> events;
    };
    struct State {
        std::string name;
        // Indices into clips; a blend also has one threshold per clip and the index of its parameter.
        std::vector<std::size_t> clips;
        std::vector<float> thresholds;
        std::size_t blend_parameter{};
        double speed = 1;
        std::optional<std::size_t> speed_parameter;
        bool loop{};
    };
    struct Test {
        std::size_t parameter{};
        AnimationStateMachine::ConditionMode mode{};
        // A float parameter's threshold, rounded to float.
        double threshold{};
    };
    struct Route {
        std::optional<std::size_t> from;
        std::size_t to{};
        std::vector<Test> tests;
        std::optional<double> exit_time;
        double duration{};
        double offset{};
        AnimationStateMachine::Interruption interruption{};
        bool to_self{};
        // Priority among the transitions that leave the same state, or among those from any state.
        std::size_t rank{};
    };
    // The machine's retained motion runtime, which samples the clips, or null when the source asset does.
    const MotionRuntime *motion{};
    std::vector<Clip> clips;
    std::vector<State> states;
    std::vector<Route> transitions;
    // Transitions from any state, and the transitions leaving each state, in priority order.
    std::vector<std::size_t> any;
    std::vector<std::vector<std::size_t>> outgoing;
    std::vector<AnimationStateMachine::ParameterType> parameter_types;
    // Initial parameter values, as AnimationStateRuntime::parameters holds them.
    std::vector<double> initial;
    std::map<std::string, std::size_t, std::less<>> parameter_names, state_names;
};
struct StateMachineAnimatorAccess {
    static const AnimationStateMachineData &data(const AnimationStateMachine &machine) { return *machine.data_; }
    static const AnimationStateRuntime &runtime(const StateMachineAnimator &animator) { return animator.runtime_; }
    static void restore(StateMachineAnimator &animator, AnimationStateRuntime next) {
        animator.commit(std::move(next));
    }
};
} // namespace detail
namespace {
using Machine = AnimationStateMachine;
using Data = detail::AnimationStateMachineData;
using Runtime = detail::AnimationStateRuntime;
using Playing = detail::AnimationStatePlaying;
using Json = nlohmann::json;
// The largest normalized advance of a looping state in one update, as Playback bounds a looping step.
constexpr double maximum_state_loops = 10'000;
// Seconds per pass of a state whose clips have zero duration, at speed 1, so that its exit times are reached.
constexpr double pose_pass_seconds = 1;
constexpr std::size_t maximum_machine_document_bytes = 4 * 1024 * 1024;
constexpr int maximum_machine_document_depth = 16;
constexpr std::int64_t machine_document_version = 1;
constexpr std::string_view machine_document_kind = "anima.animation-state-machine";
constexpr std::size_t maximum_animator_payload_bytes = 16 * 1024 * 1024;
constexpr std::size_t maximum_machine_key_bytes = 4096;
constexpr auto replaced_mesh = "StateMachineAnimator mesh was replaced; bind a new StateMachineAnimator explicitly";

void require(bool accepted, const std::string &reason) {
    if (!accepted)
        throw std::invalid_argument(reason);
}
bool within_float(double number) {
    return std::isfinite(number) && std::abs(number) <= std::numeric_limits<float>::max();
}
bool within_int32(double number) {
    return std::isfinite(number) && std::floor(number) == number &&
           number >= std::numeric_limits<std::int32_t>::min() && number <= std::numeric_limits<std::int32_t>::max();
}
// A float parameter's value or threshold as AnimationStateRuntime::parameters holds it: rounded to float.
double rounded(double number) { return static_cast<double>(static_cast<float>(number)); }

// Document spellings of the enumerators.
constexpr std::array<std::pair<std::string_view, Machine::ParameterType>, 4> parameter_type_names{{
    {"float", Machine::ParameterType::real},
    {"int", Machine::ParameterType::integer},
    {"bool", Machine::ParameterType::boolean},
    {"trigger", Machine::ParameterType::trigger},
}};
constexpr std::array<std::pair<std::string_view, Machine::ConditionMode>, 6> condition_mode_names{{
    {"is_true", Machine::ConditionMode::is_true},
    {"is_false", Machine::ConditionMode::is_false},
    {"greater", Machine::ConditionMode::greater},
    {"less", Machine::ConditionMode::less},
    {"equals", Machine::ConditionMode::equals},
    {"not_equal", Machine::ConditionMode::not_equal},
}};
constexpr std::array<std::pair<std::string_view, Machine::Interruption>, 5> interruption_names{{
    {"none", Machine::Interruption::none},
    {"source", Machine::Interruption::source},
    {"destination", Machine::Interruption::destination},
    {"source_then_destination", Machine::Interruption::source_then_destination},
    {"destination_then_source", Machine::Interruption::destination_then_source},
}};
template <class Enum, std::size_t Count>
Enum enumerator(const std::array<std::pair<std::string_view, Enum>, Count> &spellings, const Json &text,
                const char *reason) {
    const auto spelled = text.get<std::string>();
    for (const auto &[spelling, named] : spellings)
        if (spelling == spelled)
            return named;
    throw std::invalid_argument(reason + spelled);
}
// A JSON number as a double; nlohmann's own conversion would also accept a boolean.
double json_number(const Json &value) {
    if (!value.is_number())
        throw std::invalid_argument("JSON value must be a number");
    return value.get<double>();
}
const Json &array_field(const Json &object, const char *field) {
    const auto &list = object.at(field);
    if (!list.is_array())
        throw std::invalid_argument("Animation state machine field must be an array: " + std::string(field));
    return list;
}
Machine::Definition decode_definition(std::string_view document) {
    const auto parsed = detail::parse_json(document, maximum_machine_document_bytes, maximum_machine_document_depth);
    detail::json_version(parsed, "version", machine_document_version,
                         "Unsupported animation state machine document version");
    require(parsed.contains("kind") && parsed.at("kind").is_string() &&
                parsed.at("kind").get_ref<const std::string &>() == machine_document_kind,
            "Invalid animation state machine document kind");
    detail::json_fields(parsed, {"version", "kind", "parameters", "states", "transitions"});
    Machine::Definition definition;
    for (const auto &entry : array_field(parsed, "parameters")) {
        detail::json_fields(entry, {"name", "type"}, {"initial"});
        Machine::Parameter parameter;
        parameter.name = entry.at("name").get<std::string>();
        parameter.type = enumerator(parameter_type_names, entry.at("type"), "Unknown animation parameter type: ");
        if (parameter.type == Machine::ParameterType::trigger)
            detail::json_fields(entry, {"name", "type"});
        if (entry.contains("initial")) {
            const auto &initial = entry.at("initial");
            if (parameter.type == Machine::ParameterType::boolean)
                parameter.initial = initial.get<bool>() ? 1 : 0;
            else
                parameter.initial = json_number(initial);
        }
        definition.parameters.push_back(std::move(parameter));
    }
    for (const auto &entry : array_field(parsed, "states")) {
        detail::json_fields(entry, {"name"}, {"clip", "blend", "speed", "speed_parameter"});
        Machine::State state;
        state.name = entry.at("name").get<std::string>();
        require(entry.contains("clip") != entry.contains("blend"),
                "Animation state needs exactly one of a clip and a blend: " + state.name);
        if (entry.contains("clip"))
            state.clip = entry.at("clip").get<std::string>();
        else {
            const auto &blend = entry.at("blend");
            detail::json_fields(blend, {"parameter", "clips"});
            Machine::Blend decoded;
            decoded.parameter = blend.at("parameter").get<std::string>();
            for (const auto &point : array_field(blend, "clips")) {
                detail::json_fields(point, {"clip", "threshold"});
                decoded.clips.push_back(
                    {point.at("clip").get<std::string>(), detail::json_float(point.at("threshold"))});
            }
            state.blend = std::move(decoded);
        }
        if (entry.contains("speed"))
            state.speed = json_number(entry.at("speed"));
        if (entry.contains("speed_parameter"))
            state.speed_parameter = entry.at("speed_parameter").get<std::string>();
        definition.states.push_back(std::move(state));
    }
    for (const auto &entry : array_field(parsed, "transitions")) {
        detail::json_fields(entry, {"from", "to"},
                            {"conditions", "exit_time", "duration", "offset", "interruption", "to_self"});
        Machine::Transition transition;
        if (!entry.at("from").is_null())
            transition.from = entry.at("from").get<std::string>();
        transition.to = entry.at("to").get<std::string>();
        if (entry.contains("conditions"))
            for (const auto &test : array_field(entry, "conditions")) {
                detail::json_fields(test, {"parameter", "mode"}, {"threshold"});
                Machine::Condition condition;
                condition.parameter = test.at("parameter").get<std::string>();
                condition.mode =
                    enumerator(condition_mode_names, test.at("mode"), "Unknown animation condition mode: ");
                if (test.contains("threshold"))
                    condition.threshold = json_number(test.at("threshold"));
                transition.conditions.push_back(std::move(condition));
            }
        if (entry.contains("exit_time"))
            transition.exit_time = json_number(entry.at("exit_time"));
        if (entry.contains("duration"))
            transition.duration = json_number(entry.at("duration"));
        if (entry.contains("offset"))
            transition.offset = json_number(entry.at("offset"));
        if (entry.contains("interruption"))
            transition.interruption =
                enumerator(interruption_names, entry.at("interruption"), "Unknown animation transition interruption: ");
        if (entry.contains("to_self"))
            transition.to_self = entry.at("to_self").get<bool>();
        definition.transitions.push_back(std::move(transition));
    }
    return definition;
}

// The clips that a state samples: `a`, and `b` with `weight`, which is 0 when `a` plays alone.
struct ClipPair {
    std::size_t a{}, b{};
    float weight{};
};
ClipPair blend_pair(const Data::State &state, const std::vector<double> &values) {
    const auto &thresholds = state.thresholds;
    if (thresholds.empty())
        return {state.clips.front(), state.clips.front(), 0};
    const auto position = static_cast<float>(values[state.blend_parameter]);
    if (position <= thresholds.front())
        return {state.clips.front(), state.clips.front(), 0};
    if (position >= thresholds.back())
        return {state.clips.back(), state.clips.back(), 0};
    const auto upper =
        static_cast<std::size_t>(std::upper_bound(thresholds.begin(), thresholds.end(), position) - thresholds.begin());
    const auto lower = upper - 1;
    // In double, so that thresholds far apart cannot overflow.
    const auto fraction = (static_cast<double>(position) - thresholds[lower]) /
                          (static_cast<double>(thresholds[upper]) - thresholds[lower]);
    return {state.clips[lower], state.clips[upper], static_cast<float>(fraction)};
}
// Normalized time per second of a state that plays @p pair.
double state_rate(const Data &data, const Data::State &state, const std::vector<double> &values, const ClipPair &pair) {
    double multiplier = 1;
    if (state.speed_parameter) {
        multiplier = values[*state.speed_parameter];
        if (multiplier < 0)
            throw std::invalid_argument("Animation state speed must be at least 0: " + state.name);
    }
    const auto weight = static_cast<double>(pair.weight);
    // A blend's clips all have zero duration or all have a positive one, so this is 0 only when the state's clips
    // are poses, which hold their only time.
    const auto duration =
        (1 - weight) * data.clips[pair.a].animation->duration + weight * data.clips[pair.b].animation->duration;
    const auto rate = state.speed * multiplier / (duration > 0 ? duration : pose_pass_seconds);
    if (!std::isfinite(rate))
        throw std::runtime_error("Animation state time overflow");
    return rate;
}
// The weight of a state's pose through one update: 1 outside a crossfade; during one, the entered state's
// weight t seconds into the update is (elapsed + t) / duration, up to 1, and the outgoing state's is 1 minus it.
struct Fading {
    double elapsed{}, duration{};
    bool outgoing{};
    float at(double seconds) const {
        if (duration <= 0)
            return 1;
        const auto entered = std::min(1., (elapsed + seconds) / duration);
        return static_cast<float>(outgoing ? 1 - entered : entered);
    }
};
// Advances @p playing through the seconds from @p from to @p to of an update and appends the events that its
// clips cross.
void advance(const Data &data, Playing &playing, const std::vector<double> &values, double from, double to,
             const Fading &fading, std::vector<AnimationStateEvent> &events) {
    const auto &state = data.states[playing.state];
    const auto pair = blend_pair(state, values);
    const auto rate = state_rate(data, state, values, pair);
    const auto delta = rate * (to - from);
    if (state.loop && delta > maximum_state_loops)
        throw std::invalid_argument("Animation state step exceeds 10000 loops; split large offline advances");
    const auto start = playing.time, end = start + delta;
    if (!std::isfinite(end))
        throw std::runtime_error("Animation state time overflow");
    if (delta > 0) {
        const auto report = [&](std::size_t clip, float weight) {
            if (weight <= 0)
                return;
            const auto &entry = data.clips[clip];
            const auto duration = entry.animation->duration;
            for (const auto &event : entry.events) {
                // The events of a clip of zero duration all lie at its only time, 0.
                const auto point = duration > 0 ? event.time / duration : 0.;
                const auto cross = [&](double at) {
                    const auto offset = std::clamp(from + (at - start) / rate, from, to);
                    events.push_back({offset, state.name, entry.name, event, weight * fading.at(offset)});
                };
                if (state.loop) {
                    auto cycle = std::max(0., playing.fresh ? std::ceil(start - point) : std::floor(start - point) + 1);
                    for (; point + cycle <= end; cycle += 1)
                        cross(point + cycle);
                } else if ((playing.fresh ? point >= start : point > start) && point <= end)
                    cross(point);
            }
        };
        report(pair.a, 1 - pair.weight);
        if (pair.b != pair.a)
            report(pair.b, pair.weight);
        playing.fresh = false;
    }
    playing.time = end;
}
// Whether @p playing crossed @p exit since transitions were last checked.
bool crosses(const Playing &playing, double exit, bool loop) {
    const auto start = playing.checked, end = playing.time;
    if (!(end > start))
        return false;
    if (loop && exit < 1)
        return exit + std::max(0., std::floor(start - exit) + 1) <= end;
    return start < exit && exit <= end;
}
bool holds(const Data::Test &test, const std::vector<double> &values) {
    const auto number = values[test.parameter];
    switch (test.mode) {
    case Machine::ConditionMode::is_true:
        return number != 0;
    case Machine::ConditionMode::is_false:
        return number == 0;
    case Machine::ConditionMode::greater:
        return number > test.threshold;
    case Machine::ConditionMode::less:
        return number < test.threshold;
    case Machine::ConditionMode::equals:
        return number == test.threshold;
    case Machine::ConditionMode::not_equal:
        return number != test.threshold;
    }
    return false;
}
// The first transition that can start, if any.
std::optional<std::size_t> choose(const Data &data, const Runtime &runtime) {
    const auto &current = runtime.current;
    // Whether transition @p index starts; @p owner is the state it leaves while that state plays.
    const auto starts = [&](std::size_t index, const Playing *owner) {
        const auto &route = data.transitions[index];
        if (!route.from && route.to == current.state && !route.to_self)
            return false;
        for (const auto &test : route.tests)
            if (!holds(test, runtime.parameters))
                return false;
        return !route.exit_time || (owner && crosses(*owner, *route.exit_time, data.states[owner->state].loop));
    };
    if (!runtime.fade) {
        for (const auto index : data.any)
            if (starts(index, &current))
                return index;
        for (const auto index : data.outgoing[current.state])
            if (starts(index, &current))
                return index;
        return std::nullopt;
    }
    const auto &fade = *runtime.fade;
    const auto &active = data.transitions[fade.transition];
    if (active.interruption == Machine::Interruption::none)
        return std::nullopt;
    std::vector<std::pair<std::size_t, const Playing *>> candidates;
    std::vector<bool> listed(data.transitions.size());
    const auto add = [&](std::size_t index, const Playing *owner) {
        if (index != fade.transition && !listed[index]) {
            listed[index] = true;
            candidates.emplace_back(index, owner);
        }
    };
    const auto sources = [&] {
        for (const auto index : data.any)
            if (active.from || data.transitions[index].rank < active.rank)
                add(index, nullptr);
        if (active.from)
            for (const auto index : data.outgoing[fade.source_state])
                if (data.transitions[index].rank < active.rank)
                    add(index, fade.source ? &*fade.source : nullptr);
    };
    const auto destinations = [&] {
        for (const auto index : data.any)
            add(index, nullptr);
        for (const auto index : data.outgoing[current.state])
            add(index, &current);
    };
    switch (active.interruption) {
    case Machine::Interruption::none:
        break;
    case Machine::Interruption::source:
        sources();
        break;
    case Machine::Interruption::destination:
        destinations();
        break;
    case Machine::Interruption::source_then_destination:
        sources();
        destinations();
        break;
    case Machine::Interruption::destination_then_source:
        destinations();
        sources();
        break;
    }
    for (const auto &[index, owner] : candidates)
        if (starts(index, owner))
            return index;
    return std::nullopt;
}
// Starts transition @p index; @p published is the pose last published.
void start(const Data &data, Runtime &runtime, std::size_t index, const Pose &published) {
    const auto &route = data.transitions[index];
    for (const auto &test : route.tests)
        if (data.parameter_types[test.parameter] == Machine::ParameterType::trigger)
            runtime.parameters[test.parameter] = 0;
    Playing entered;
    entered.state = route.to;
    entered.time = entered.checked = route.offset;
    entered.fresh = true;
    if (route.duration <= 0) {
        runtime.fade.reset();
        runtime.current = entered;
        return;
    }
    detail::AnimationStateFade fade;
    fade.transition = index;
    fade.source_state = route.from.value_or(runtime.current.state);
    if (runtime.fade)
        fade.frozen = std::make_shared<const Pose>(published);
    else
        fade.source = runtime.current;
    runtime.fade = std::move(fade);
    runtime.current = entered;
}
Pose state_pose(const Data &data, const Asset &asset, const Playing &playing, const std::vector<double> &values) {
    const auto &state = data.states[playing.state];
    const auto pair = blend_pair(state, values);
    const auto fraction = state.loop ? playing.time - std::floor(playing.time) : std::min(playing.time, 1.);
    const auto sample = [&](std::size_t clip) {
        const auto &entry = data.clips[clip];
        const auto time = fraction * entry.animation->duration;
        return data.motion ? data.motion->sample(entry.name, time) : sample_pose(asset, entry.animation, time);
    };
    if (pair.weight <= 0)
        return sample(pair.a);
    if (pair.weight >= 1)
        return sample(pair.b);
    return blend_pose(asset, sample(pair.a), sample(pair.b), pair.weight);
}
Pose evaluate(const Data &data, const Asset &asset, const Runtime &runtime) {
    auto entered = state_pose(data, asset, runtime.current, runtime.parameters);
    if (!runtime.fade)
        return entered;
    const auto &fade = *runtime.fade;
    const auto weight = static_cast<float>(fade.elapsed / data.transitions[fade.transition].duration);
    if (fade.source)
        return blend_pose(asset, state_pose(data, asset, *fade.source, runtime.parameters), entered, weight);
    return blend_pose(asset, *fade.frozen, entered, weight);
}
void machine_key(const std::string &key) {
    require(!key.empty() && key.size() <= maximum_machine_key_bytes && key.find('\0') == std::string::npos,
            "Invalid animation state machine key");
}
} // namespace

AnimationStateMachine::AnimationStateMachine(std::shared_ptr<const Asset> source, std::span<const ClipMetadata> clips,
                                             Definition definition)
    : source_(std::move(source)), definition_(std::move(definition)) {
    require(bool(source_), "Animation state machine requires a source asset");
    compile(clips);
}
AnimationStateMachine::AnimationStateMachine(std::shared_ptr<const MotionRuntime> motion, Definition definition)
    : motion_(std::move(motion)), definition_(std::move(definition)) {
    require(bool(motion_), "Animation state machine requires a motion runtime");
    source_ = motion_->model();
    std::vector<ClipMetadata> clips;
    for (const auto &[name, metadata] : motion_->clips())
        clips.push_back(metadata);
    compile(clips);
}
void AnimationStateMachine::compile(std::span<const ClipMetadata> clips) {
    auto data = std::make_shared<Data>();
    data->motion = motion_.get();
    std::map<std::string_view, const ClipMetadata *> policies;
    for (const auto &policy : clips)
        require(policies.emplace(policy.name, &policy).second, "Duplicate animation clip metadata: " + policy.name);

    const auto &parameters = definition_.parameters;
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        const auto &parameter = parameters[i];
        require(!parameter.name.empty(), "Empty animation parameter name");
        require(data->parameter_names.emplace(parameter.name, i).second,
                "Duplicate animation parameter: " + parameter.name);
        bool valid = false;
        switch (parameter.type) {
        case ParameterType::real:
            valid = within_float(parameter.initial);
            break;
        case ParameterType::integer:
            valid = within_int32(parameter.initial);
            break;
        case ParameterType::boolean:
            valid = parameter.initial == 0 || parameter.initial == 1;
            break;
        case ParameterType::trigger:
            valid = parameter.initial == 0;
            break;
        default:
            throw std::invalid_argument("Invalid animation parameter type: " + parameter.name);
        }
        require(valid, "Invalid initial value of animation parameter: " + parameter.name);
        data->parameter_types.push_back(parameter.type);
        data->initial.push_back(parameter.type == ParameterType::real ? rounded(parameter.initial) : parameter.initial);
    }
    const auto parameter_index = [&](const std::string &name) {
        const auto found = data->parameter_names.find(name);
        require(found != data->parameter_names.end(), "Unknown animation parameter: " + name);
        return found->second;
    };
    const auto float_parameter = [&](const std::string &name) {
        const auto found = parameter_index(name);
        require(parameters[found].type == ParameterType::real, "Animation parameter must be a float: " + name);
        return found;
    };

    std::map<std::string, std::size_t, std::less<>> clip_indices;
    const auto clip_index = [&](const std::string &name) {
        if (const auto known = clip_indices.find(name); known != clip_indices.end())
            return known->second;
        const auto policy = policies.find(name);
        require(policy != policies.end(), "Animation clip has no metadata: " + name);
        const Animation *animation = nullptr;
        if (motion_)
            // The policies are the motion's base clips, each of which it holds exactly once.
            animation = &motion_->clip(name);
        else {
            std::size_t count = 0;
            for (const auto &candidate : source_->animations)
                if (candidate.name == name) {
                    animation = &candidate;
                    ++count;
                }
            require(count == 1, "Animation clip must name exactly one source clip: " + name);
        }
        require(std::isfinite(animation->duration) && animation->duration >= 0,
                "Animation clip duration must be finite and nonnegative: " + name);
        for (const auto &event : policy->second->events)
            require(std::isfinite(event.time) && event.time >= 0 && event.time <= animation->duration,
                    "Animation clip event outside its duration: " + name);
        data->clips.push_back({name, animation, policy->second->loop, policy->second->events});
        clip_indices.emplace(name, data->clips.size() - 1);
        return data->clips.size() - 1;
    };

    const auto &states = definition_.states;
    require(!states.empty(), "Animation state machine needs a state");
    for (std::size_t i = 0; i < states.size(); ++i) {
        require(!states[i].name.empty(), "Empty animation state name");
        require(data->state_names.emplace(states[i].name, i).second, "Duplicate animation state: " + states[i].name);
    }
    for (const auto &state : states) {
        const bool plays_clip = !state.clip.empty();
        require(plays_clip != state.blend.has_value(),
                "Animation state needs exactly one of a clip and a blend: " + state.name);
        require(std::isfinite(state.speed) && state.speed >= 0, "Invalid animation state speed: " + state.name);
        Data::State compiled;
        compiled.name = state.name;
        compiled.speed = state.speed;
        if (!state.speed_parameter.empty())
            compiled.speed_parameter = float_parameter(state.speed_parameter);
        if (state.blend) {
            compiled.blend_parameter = float_parameter(state.blend->parameter);
            require(state.blend->clips.size() >= 2, "Animation blend needs two or more clips: " + state.name);
            for (const auto &point : state.blend->clips) {
                require(std::isfinite(point.threshold) &&
                            (compiled.thresholds.empty() || point.threshold > compiled.thresholds.back()),
                        "Animation blend thresholds must be finite and increasing: " + state.name);
                compiled.clips.push_back(clip_index(point.clip));
                compiled.thresholds.push_back(point.threshold);
            }
        } else
            compiled.clips.push_back(clip_index(state.clip));
        // A clip of zero duration is a pose.
        const auto pose = [&](std::size_t clip) { return data->clips[clip].animation->duration == 0; };
        compiled.loop = data->clips[compiled.clips.front()].loop;
        for (const auto played : compiled.clips) {
            require(data->clips[played].loop == compiled.loop,
                    "Animation blend clips must all loop or all hold: " + state.name);
            // Weighting a pose against a clip with a duration would shorten the state's pass, and raise its rate,
            // without bound as the pose's weight approaches 1.
            require(pose(played) == pose(compiled.clips.front()),
                    "Animation blend mixes clips of zero and positive duration: " + state.name);
        }
        data->states.push_back(std::move(compiled));
    }

    const auto state_index = [&](const std::string &name) {
        const auto found = data->state_names.find(name);
        require(found != data->state_names.end(), "Unknown animation state: " + name);
        return found->second;
    };
    data->outgoing.resize(states.size());
    const auto &transitions = definition_.transitions;
    for (std::size_t i = 0; i < transitions.size(); ++i) {
        const auto &transition = transitions[i];
        Data::Route route;
        if (transition.from)
            route.from = state_index(*transition.from);
        route.to = state_index(transition.to);
        require(transition.exit_time || !transition.conditions.empty(),
                "Animation transition needs an exit time or a condition");
        require(transition.from || !transition.exit_time, "A transition from any state has no exit time");
        require(!transition.from || !transition.to_self, "Only a transition from any state can set to_self");
        require(!transition.exit_time || (std::isfinite(*transition.exit_time) && *transition.exit_time >= 0),
                "Invalid animation transition exit time");
        require(std::isfinite(transition.duration) && transition.duration >= 0,
                "Invalid animation transition duration");
        require(std::isfinite(transition.offset) && transition.offset >= 0 && transition.offset < 1,
                "Invalid animation transition offset");
        switch (transition.interruption) {
        case Interruption::none:
        case Interruption::source:
        case Interruption::destination:
        case Interruption::source_then_destination:
        case Interruption::destination_then_source:
            break;
        default:
            throw std::invalid_argument("Invalid animation transition interruption");
        }
        for (const auto &condition : transition.conditions) {
            Data::Test test{parameter_index(condition.parameter), condition.mode, condition.threshold};
            const auto type = parameters[test.parameter].type;
            const auto mode = condition.mode;
            const bool ordered = mode == ConditionMode::greater || mode == ConditionMode::less;
            const bool suits = type == ParameterType::real ? ordered
                               : type == ParameterType::integer
                                   ? ordered || mode == ConditionMode::equals || mode == ConditionMode::not_equal
                               : type == ParameterType::boolean
                                   ? mode == ConditionMode::is_true || mode == ConditionMode::is_false
                                   : mode == ConditionMode::is_true;
            require(suits, "Animation condition mode does not apply to parameter: " + condition.parameter);
            const bool valid = type == ParameterType::real      ? within_float(condition.threshold)
                               : type == ParameterType::integer ? within_int32(condition.threshold)
                                                                : condition.threshold == 0;
            require(valid, "Invalid animation condition threshold: " + condition.parameter);
            if (type == ParameterType::real)
                test.threshold = rounded(condition.threshold);
            route.tests.push_back(test);
        }
        route.exit_time = transition.exit_time;
        route.duration = transition.duration;
        route.offset = transition.offset;
        route.interruption = transition.interruption;
        route.to_self = transition.to_self;
        auto &list = route.from ? data->outgoing[*route.from] : data->any;
        route.rank = list.size();
        list.push_back(i);
        data->transitions.push_back(std::move(route));
    }
    data_ = std::move(data);
}
AnimationStateMachine AnimationStateMachine::deserialize(std::shared_ptr<const Asset> source,
                                                         std::span<const ClipMetadata> clips,
                                                         std::string_view document) {
    auto decoded = detail::json_step([&] { return decode_definition(document); });
    return AnimationStateMachine(std::move(source), clips, std::move(decoded));
}
AnimationStateMachine AnimationStateMachine::deserialize(std::shared_ptr<const MotionRuntime> motion,
                                                         std::string_view document) {
    auto decoded = detail::json_step([&] { return decode_definition(document); });
    return AnimationStateMachine(std::move(motion), std::move(decoded));
}

StateMachineAnimator::StateMachineAnimator(GameObject object, std::shared_ptr<const AnimationStateMachine> machine)
    : object_(std::move(object)), mesh_(object_.renderer().mesh()), machine_(std::move(machine)) {
    if (!machine_)
        throw std::invalid_argument("StateMachineAnimator requires a state machine");
    if (!mesh_->accepts_animation_source(*machine_->source()))
        throw std::invalid_argument("StateMachineAnimator source does not match the object's mesh hierarchy and bind");
    detail::AnimationStateRuntime entry;
    entry.parameters = machine_->data_->initial;
    entry.current.fresh = true;
    commit(std::move(entry));
}
std::size_t StateMachineAnimator::parameter(std::string_view name, AnimationStateMachine::ParameterType type) const {
    const auto &data = *machine_->data_;
    const auto found = data.parameter_names.find(name);
    if (found == data.parameter_names.end())
        throw std::out_of_range("Unknown animation parameter: " + std::string(name));
    if (data.parameter_types[found->second] != type)
        throw std::invalid_argument("Animation parameter has another type: " + std::string(name));
    return found->second;
}
void StateMachineAnimator::set_float(std::string_view name, float value) {
    const auto index = parameter(name, AnimationStateMachine::ParameterType::real);
    if (!std::isfinite(value))
        throw std::invalid_argument("Animation float parameter must be finite: " + std::string(name));
    runtime_.parameters[index] = value;
}
void StateMachineAnimator::set_integer(std::string_view name, std::int32_t value) {
    runtime_.parameters[parameter(name, AnimationStateMachine::ParameterType::integer)] = value;
}
void StateMachineAnimator::set_bool(std::string_view name, bool value) {
    runtime_.parameters[parameter(name, AnimationStateMachine::ParameterType::boolean)] = value ? 1 : 0;
}
void StateMachineAnimator::set_trigger(std::string_view name) {
    runtime_.parameters[parameter(name, AnimationStateMachine::ParameterType::trigger)] = 1;
}
void StateMachineAnimator::reset_trigger(std::string_view name) {
    runtime_.parameters[parameter(name, AnimationStateMachine::ParameterType::trigger)] = 0;
}
float StateMachineAnimator::get_float(std::string_view name) const {
    return static_cast<float>(runtime_.parameters[parameter(name, AnimationStateMachine::ParameterType::real)]);
}
std::int32_t StateMachineAnimator::get_integer(std::string_view name) const {
    return static_cast<std::int32_t>(
        runtime_.parameters[parameter(name, AnimationStateMachine::ParameterType::integer)]);
}
bool StateMachineAnimator::get_bool(std::string_view name) const {
    const auto &data = *machine_->data_;
    const auto found = data.parameter_names.find(name);
    if (found != data.parameter_names.end() &&
        data.parameter_types[found->second] == AnimationStateMachine::ParameterType::trigger)
        return runtime_.parameters[found->second] != 0;
    return runtime_.parameters[parameter(name, AnimationStateMachine::ParameterType::boolean)] != 0;
}
void StateMachineAnimator::play(std::string_view name, double normalized_time) {
    const auto &names = machine_->data_->state_names;
    const auto found = names.find(name);
    if (found == names.end())
        throw std::out_of_range("Unknown animation state: " + std::string(name));
    if (!std::isfinite(normalized_time) || normalized_time < 0)
        throw std::invalid_argument("Invalid animation state time");
    auto next = runtime_;
    next.fade.reset();
    next.current.state = found->second;
    next.current.time = next.current.checked = normalized_time;
    next.current.fresh = true;
    commit(std::move(next));
}
std::vector<AnimationStateEvent> StateMachineAnimator::update(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0)
        throw std::invalid_argument("Invalid StateMachineAnimator time step");
    // Validate even for a zero step, so a replaced mesh cannot hide.
    if (object_.renderer().mesh() != mesh_)
        throw std::logic_error(replaced_mesh);
    const auto &data = *machine_->data_;
    auto next = runtime_;
    const auto chosen = choose(data, next);
    next.current.checked = next.current.time;
    if (next.fade && next.fade->source)
        next.fade->source->checked = next.fade->source->time;
    if (chosen)
        start(data, next, *chosen, pose_);
    std::vector<AnimationStateEvent> events;
    if (next.fade) {
        auto &fade = *next.fade;
        const auto duration = data.transitions[fade.transition].duration;
        // The outgoing state stops once the crossfade ends.
        if (fade.source)
            advance(data, *fade.source, next.parameters, 0, std::min(seconds, duration - fade.elapsed),
                    {fade.elapsed, duration, true}, events);
        advance(data, next.current, next.parameters, 0, seconds, {fade.elapsed, duration, false}, events);
        fade.elapsed += seconds;
        if (fade.elapsed >= duration)
            next.fade.reset();
    } else
        advance(data, next.current, next.parameters, 0, seconds, {}, events);
    std::stable_sort(events.begin(), events.end(),
                     [](const AnimationStateEvent &a, const AnimationStateEvent &b) { return a.offset < b.offset; });
    commit(std::move(next));
    return events;
}
const std::string &StateMachineAnimator::state() const {
    return machine_->definition().states[runtime_.current.state].name;
}
std::optional<AnimationCrossfade> StateMachineAnimator::crossfade() const {
    if (!runtime_.fade)
        return std::nullopt;
    const auto &fade = *runtime_.fade;
    AnimationCrossfade result;
    result.transition = fade.transition;
    result.source = machine_->definition().states[fade.source_state].name;
    if (fade.source)
        result.source_time = fade.source->time;
    result.elapsed = fade.elapsed;
    result.weight = static_cast<float>(fade.elapsed / machine_->data_->transitions[fade.transition].duration);
    return result;
}
void StateMachineAnimator::commit(detail::AnimationStateRuntime next) {
    if (object_.renderer().mesh() != mesh_)
        throw std::logic_error(replaced_mesh);
    auto evaluated = evaluate(*machine_->data_, *machine_->source(), next);
    // The filter works on a copy, so that crossfades keep blending the evaluated pose, which has local transforms.
    std::optional<Pose> filtered;
    if (filter_) {
        filtered = evaluated;
        filter_(*filtered);
    }
    object_.renderer().set_pose(filtered ? *filtered : evaluated);
    runtime_ = std::move(next);
    pose_ = std::move(evaluated);
    filtered_ = std::move(filtered);
}

void add_state_machine_animator_codec(ComponentCodecs &codecs, AnimationStateMachineName name,
                                      AnimationStateMachineResolver resolve) {
    if (!name || !resolve)
        throw std::invalid_argument("State machine animator codec requires naming and resolution callbacks");
    // Keep registration atomic if the type or key is already registered.
    auto pending = codecs;
    pending.add<StateMachineAnimator>(
        "anima.state-machine-animator.v1",
        [name = std::move(name)](const StateMachineAnimator &animator, const ObjectReferences &) {
            const auto key = name(animator.machine());
            machine_key(key);
            const auto &parameters = animator.machine()->definition().parameters;
            const auto &values = detail::StateMachineAnimatorAccess::runtime(animator).parameters;
            auto encoded = Json::object();
            for (std::size_t i = 0; i < parameters.size(); ++i) {
                const auto type = parameters[i].type;
                if (type == AnimationStateMachine::ParameterType::real)
                    encoded[parameters[i].name] = static_cast<float>(values[i]);
                else if (type == AnimationStateMachine::ParameterType::integer)
                    encoded[parameters[i].name] = static_cast<std::int32_t>(values[i]);
                else
                    encoded[parameters[i].name] = values[i] != 0;
            }
            auto payload = Json{{"machine", key},
                                {"state", animator.state()},
                                {"time", animator.time()},
                                {"parameters", std::move(encoded)}}
                               .dump();
            // The decoder rejects larger payloads, so an animator captured over the limit could never be restored.
            require(payload.size() <= maximum_animator_payload_bytes, "State machine animator payload exceeds 16 MiB");
            return payload;
        },
        [resolve = std::move(resolve)](GameObject object, std::string_view payload, const ObjectReferences &) {
            const auto decoded = detail::parse_json(payload, maximum_animator_payload_bytes);
            detail::json_fields(decoded, {"machine", "state", "time", "parameters"});
            const auto key = decoded.at("machine").get<std::string>();
            machine_key(key);
            auto machine = resolve(key);
            require(bool(machine), "Animation state machine key could not be resolved");
            const auto &data = detail::StateMachineAnimatorAccess::data(*machine);
            const auto state_name = decoded.at("state").get<std::string>();
            const auto found = data.state_names.find(state_name);
            require(found != data.state_names.end(), "Unknown animation state: " + state_name);
            const auto normalized_time = json_number(decoded.at("time"));
            require(normalized_time >= 0, "Invalid animation state time");
            const auto &values = decoded.at("parameters");
            require(values.is_object(), "State machine animator parameters must be an object");
            for (auto entry = values.begin(); entry != values.end(); ++entry)
                require(data.parameter_names.contains(entry.key()), "Unknown animation parameter: " + entry.key());
            detail::AnimationStateRuntime restored;
            const auto &parameters = machine->definition().parameters;
            for (const auto &parameter : parameters) {
                require(values.contains(parameter.name), "Missing animation parameter: " + parameter.name);
                const auto &value = values.at(parameter.name);
                bool valid = false;
                double number = 0;
                if (parameter.type == AnimationStateMachine::ParameterType::real ||
                    parameter.type == AnimationStateMachine::ParameterType::integer) {
                    number = value.is_number() ? value.get<double>() : std::numeric_limits<double>::quiet_NaN();
                    valid = parameter.type == AnimationStateMachine::ParameterType::real ? within_float(number)
                                                                                         : within_int32(number);
                    if (valid && parameter.type == AnimationStateMachine::ParameterType::real)
                        number = rounded(number);
                } else if (value.is_boolean()) {
                    valid = true;
                    number = value.get<bool>() ? 1 : 0;
                }
                require(valid, "Invalid animation parameter value: " + parameter.name);
                restored.parameters.push_back(number);
            }
            restored.current.state = found->second;
            restored.current.time = restored.current.checked = normalized_time;
            auto animator = object.add_component<StateMachineAnimator>(std::move(machine));
            detail::StateMachineAnimatorAccess::restore(animator.get(), std::move(restored));
        });
    codecs = std::move(pending);
}
} // namespace anima
