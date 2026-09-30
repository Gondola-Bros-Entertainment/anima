#include "../detail/input.hpp"
#include "../detail/json.hpp"
#include "../detail/scene_driver.hpp"
#include <anima/input_scene.hpp>
namespace anima::input {
namespace {
using Json = nlohmann::json;
constexpr unsigned document_version = 3;
constexpr std::string_view hex_digits = "0123456789abcdef";
unsigned integer(const Json &v, std::uint64_t maximum) {
    if (!v.is_number_integer() || v.get<std::int64_t>() < 0 || v.get<std::uint64_t>() > maximum)
        throw std::invalid_argument("Invalid input configuration integer");
    return v.get<unsigned>();
}
float number(const Json &v) {
    if (!v.is_number())
        throw std::invalid_argument("Invalid input configuration number");
    return detail::json_float(v);
}
// @p identity as 32 lowercase hexadecimal digits, first byte first, or null for none.
Json encode_identity(const DeviceIdentity &identity) {
    if (identity == DeviceIdentity{})
        return nullptr;
    std::string text;
    for (const std::size_t byte : identity) {
        text += hex_digits[byte >> 4];
        text += hex_digits[byte & 0xF];
    }
    return text;
}
DeviceIdentity decode_identity(const Json &value) {
    DeviceIdentity identity{};
    if (value.is_null())
        return identity;
    const auto *text = value.get_ptr<const std::string *>();
    if (!text || text->size() != 2 * identity.size())
        throw std::invalid_argument("Invalid input device identity");
    for (std::size_t index = 0; index < text->size(); ++index) {
        const auto digit = hex_digits.find((*text)[index]);
        if (digit == std::string_view::npos)
            throw std::invalid_argument("Invalid input device identity");
        auto &byte = identity[index / 2];
        byte = static_cast<std::uint8_t>((byte << 4) | digit);
    }
    if (identity == DeviceIdentity{})
        throw std::invalid_argument("Invalid input device identity");
    return identity;
}
Json encode_control(const Control &control) {
    if (control.device != any_device)
        throw std::invalid_argument("Input configuration cannot persist a device ID");
    return {{"kind", static_cast<int>(control.kind)},
            {"code", control.code},
            {"identity", encode_identity(control.identity)}};
}
Control decode_control(const Json &value) {
    return {static_cast<ControlKind>(integer(value.at("kind"), static_cast<unsigned>(limits::last_kind))),
            static_cast<std::uint16_t>(integer(value.at("code"), limits::key_code)), any_device,
            decode_identity(value.at("identity"))};
}
} // namespace
std::string serialize_map(const Map &map) {
    validate(map);
    Json actions = Json::array();
    for (const auto &a : map) {
        Json bindings = Json::array();
        for (const auto &b : a.bindings) {
            auto modifiers = Json::array();
            for (const auto &modifier : b.modifiers)
                modifiers.push_back(encode_control(modifier));
            auto binding = encode_control(b.control);
            binding["channel"] = static_cast<int>(b.channel);
            binding["scale"] = b.scale;
            binding["deadzone"] = b.deadzone;
            binding["modifiers"] = modifiers;
            bindings.push_back(binding);
        }
        actions.push_back(
            {{"name", a.name}, {"type", static_cast<int>(a.type)}, {"threshold", a.threshold}, {"bindings", bindings}});
    }
    auto document = detail::json_step([&] { return Json{{"version", document_version}, {"actions", actions}}.dump(); });
    if (document.size() > limits::document_bytes)
        throw std::invalid_argument("Input configuration exceeds byte limit");
    return document;
}
namespace {
Map decode_map(std::string_view data) {
    const auto j = detail::parse_json(data, limits::document_bytes);
    detail::json_version(j, "version", document_version, "Unsupported input configuration version");
    detail::json_fields(j, {"version", "actions"});
    if (!j.at("actions").is_array() || j.at("actions").size() > limits::actions)
        throw std::invalid_argument("Invalid input configuration envelope");
    Map map;
    for (const auto &a : j.at("actions")) {
        detail::json_fields(a, {"name", "type", "threshold", "bindings"});
        if (!a.at("name").is_string() || !a.at("bindings").is_array() ||
            a.at("bindings").size() > limits::bindings_per_action)
            throw std::invalid_argument("Invalid input action fields");
        Action action{a.at("name").get<std::string>(),
                      static_cast<ActionType>(integer(a.at("type"), static_cast<unsigned>(ActionType::vector2))),
                      {},
                      number(a.at("threshold"))};
        for (const auto &b : a.at("bindings")) {
            detail::json_fields(b, {"kind", "code", "identity", "channel", "scale", "deadzone", "modifiers"});
            const auto &modifiers = b.at("modifiers");
            if (!modifiers.is_array() || modifiers.size() > limits::modifiers_per_binding)
                throw std::invalid_argument("Invalid input modifier count");
            Binding binding{decode_control(b),
                            static_cast<Channel>(integer(b.at("channel"), static_cast<unsigned>(Channel::y))),
                            number(b.at("scale")),
                            number(b.at("deadzone")),
                            {}};
            for (const auto &modifier : modifiers) {
                detail::json_fields(modifier, {"kind", "code", "identity"});
                binding.modifiers.push_back(decode_control(modifier));
            }
            action.bindings.push_back(std::move(binding));
        }
        map.push_back(std::move(action));
    }
    validate(map);
    return map;
}
} // namespace
Map deserialize_map(std::string_view data) {
    return detail::json_step([&] { return decode_map(data); });
}
void add_component_codec(ComponentCodecs &codecs) {
    codecs.add<ActionInput>(
        "anima.action-input.v3",
        [](const ActionInput &input, const ObjectReferences &) {
            return serialize_map(Map(input.context().actions().begin(), input.context().actions().end()));
        },
        [](GameObject object, std::string_view data, const ObjectReferences &) {
            object.add_component<ActionInput>(deserialize_map(data));
        });
}
namespace {
template <class Scenes> void begin_frame_scenes(Scenes &scenes) {
    anima::detail::SceneDriver::check(scenes);
    for (auto input : scenes.template components<ActionInput>()) {
        input->context().begin_frame();
        input->context().set_enabled(input.active());
    }
}
template <class Scenes> void dispatch_scenes(Scenes &scenes, const Event &event) {
    using anima::detail::InputStaging;
    anima::detail::SceneDriver::check(scenes);
    validate(event);
    // Every context stages the event, which checks it and allocates what applying it needs, before any context
    // applies it, and applying cannot fail. No context is copied.
    struct Pending {
        ComponentRef<ActionInput> target;
        InputStaging::Staged staged;
    };
    const auto inputs = scenes.template components<ActionInput>();
    std::vector<Pending> pending;
    pending.reserve(inputs.size());
    for (auto input : inputs)
        pending.push_back({input, InputStaging::stage(input->context(), event, input.active())});
    for (auto &p : pending)
        InputStaging::apply(p.target->context(), std::move(p.staged));
}
} // namespace
void begin_frame(Scene &scene) { begin_frame_scenes(scene); }
void begin_frame(SceneSet &scenes) { begin_frame_scenes(scenes); }
void dispatch(Scene &scene, const Event &event) { dispatch_scenes(scene, event); }
void dispatch(SceneSet &scenes, const Event &event) { dispatch_scenes(scenes, event); }
} // namespace anima::input
