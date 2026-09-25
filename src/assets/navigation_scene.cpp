#include "../detail/json.hpp"
#include "../detail/navigation.hpp"
#include <anima/navigation_scene.hpp>
namespace anima::navigation {
namespace {
constexpr std::size_t maximum_component_bytes = 16 * 1024 * 1024;
}
Agent::Agent(std::vector<Vec3> route, float speed, float arrival_distance, std::size_t next)
    : follower_(std::move(route), next), speed_(speed), arrival_distance_(arrival_distance) {
    detail::navigation_settings(speed_, arrival_distance_);
}
void Agent::set_speed(float speed) {
    detail::navigation_settings(speed, arrival_distance_);
    speed_ = speed;
    velocity_ = {};
}
void Agent::set_arrival_distance(float distance) {
    detail::navigation_settings(speed_, distance);
    arrival_distance_ = distance;
    velocity_ = {};
}
void Agent::set_route(std::vector<Vec3> route, std::size_t next) {
    follower_.set_route(std::move(route), next);
    velocity_ = {};
}
void update_agents(Scene &scene, double seconds) {
    // Stage results so a bad object transform cannot partially consume routes.
    struct Update {
        ComponentRef<Agent> component;
        Follower follower;
        Vec3 velocity;
    };
    std::vector<Update> updates;
    detail::navigation_step(seconds);
    for (auto component : scene.components<Agent>()) {
        auto follower = component->follower_;
        Vec3 velocity{};
        if (component.active()) {
            const auto m = component.object().world_matrix();
            velocity = follower.steer({m[12], m[13], m[14]}, component->speed_, seconds, component->arrival_distance_);
        }
        updates.push_back({component, std::move(follower), velocity});
    }
    for (auto &update : updates) {
        update.component->follower_ = std::move(update.follower);
        update.component->velocity_ = update.velocity;
    }
}
void add_component_codec(ComponentCodecs &codecs) {
    using Json = nlohmann::json;
    codecs.add<Agent>(
        "anima.navigation-agent.v1",
        [](const Agent &agent, const ObjectReferences &) {
            Json points = Json::array();
            for (const auto p : agent.follower().route())
                points.push_back(Json::array({p.x, p.y, p.z}));
            return Json{{"route", points},
                        {"next", agent.follower().next()},
                        {"speed", agent.speed()},
                        {"arrival_distance", agent.arrival_distance()}}
                .dump();
        },
        [](GameObject object, std::string_view data, const ObjectReferences &) {
            const auto j = detail::parse_json(data, maximum_component_bytes);
            detail::json_fields(j, {"route", "next", "speed", "arrival_distance"});
            if (!j.at("route").is_array() || !j.at("next").is_number_integer() ||
                j.at("next").get<std::int64_t>() < 0 || j.at("next").get<std::uint64_t>() > j.at("route").size() ||
                !j.at("speed").is_number() || !j.at("arrival_distance").is_number())
                throw std::invalid_argument("Invalid navigation agent fields");
            std::vector<Vec3> route;
            if (j.at("route").size() > detail::maximum_navigation_nodes)
                throw std::invalid_argument("Navigation route too large");
            for (const auto &p : j.at("route")) {
                if (!p.is_array() || p.size() != 3 || !p[0].is_number() || !p[1].is_number() || !p[2].is_number())
                    throw std::invalid_argument("Invalid navigation waypoint");
                route.push_back({p[0].get<float>(), p[1].get<float>(), p[2].get<float>()});
            }
            object.add_component<Agent>(std::move(route), j.at("speed").get<float>(),
                                        j.at("arrival_distance").get<float>(), j.at("next").get<std::size_t>());
        });
}
} // namespace anima::navigation
