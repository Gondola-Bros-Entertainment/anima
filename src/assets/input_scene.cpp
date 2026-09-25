#include "../detail/input.hpp"
#include "../detail/json.hpp"
#include "../detail/scene_driver.hpp"
#include <anima/input_scene.hpp>
namespace anima::input {
namespace {
using Json = nlohmann::json;
constexpr unsigned document_version = 1;
unsigned integer(const Json &v, std::uint64_t maximum) {
    if (!v.is_number_integer() || v.get<std::int64_t>() < 0 || v.get<std::uint64_t>() > maximum)
        throw std::invalid_argument("Invalid input configuration integer");
    return v.get<unsigned>();
}
float number(const Json &v) {
    if (!v.is_number())
        throw std::invalid_argument("Invalid input configuration number");
    return v.get<float>();
}
} // namespace
std::string serialize_map(const Map &map) {
    validate(map);
    Json actions = Json::array();
    for (const auto &a : map) {
        Json bindings = Json::array();
        for (const auto &b : a.bindings)
            bindings.push_back({{"kind", static_cast<int>(b.control.kind)},
                                {"code", b.control.code},
                                {"device", b.control.device},
                                {"channel", static_cast<int>(b.channel)},
                                {"scale", b.scale},
                                {"deadzone", b.deadzone}});
        actions.push_back(
            {{"name", a.name}, {"type", static_cast<int>(a.type)}, {"threshold", a.threshold}, {"bindings", bindings}});
    }
    return Json{{"version", document_version}, {"actions", actions}}.dump();
}
Map deserialize_map(std::string_view data) {
    const auto j = detail::parse_json(data, limits::document_bytes);
    detail::json_fields(j, {"version", "actions"});
    if (integer(j.at("version"), document_version) != document_version || !j.at("actions").is_array() ||
        j.at("actions").size() > limits::actions)
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
            detail::json_fields(b, {"kind", "code", "device", "channel", "scale", "deadzone"});
            action.bindings.push_back(
                {{static_cast<ControlKind>(integer(b.at("kind"), static_cast<unsigned>(ControlKind::gamepad_axis))),
                  static_cast<std::uint16_t>(integer(b.at("code"), limits::key_code)),
                  integer(b.at("device"), UINT32_MAX)},
                 static_cast<Channel>(integer(b.at("channel"), static_cast<unsigned>(Channel::y))),
                 number(b.at("scale")),
                 number(b.at("deadzone"))});
        }
        map.push_back(std::move(action));
    }
    validate(map);
    return map;
}
void add_component_codec(ComponentCodecs &codecs) {
    codecs.add<ActionInput>(
        "anima.action-input.v1",
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
    anima::detail::SceneDriver::check(scenes);
    validate(event);
    struct Pending {
        ComponentRef<ActionInput> target;
        Context context;
    };
    std::vector<Pending> pending;
    for (auto input : scenes.template components<ActionInput>()) {
        auto context = input->context();
        context.set_enabled(input.active());
        context.process(event);
        pending.push_back({input, std::move(context)});
    }
    for (auto &p : pending)
        p.target->context() = std::move(p.context);
}
} // namespace
void begin_frame(Scene &scene) { begin_frame_scenes(scene); }
void begin_frame(SceneSet &scenes) { begin_frame_scenes(scenes); }
void dispatch(Scene &scene, const Event &event) { dispatch_scenes(scene, event); }
void dispatch(SceneSet &scenes, const Event &event) { dispatch_scenes(scenes, event); }
} // namespace anima::input
