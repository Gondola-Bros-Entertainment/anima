#include "../detail/json.hpp"
#include "../detail/navigation.hpp"
#include "../detail/scene_driver.hpp"
#include <anima/navigation_scene.hpp>
namespace anima::detail {
struct NavigationSceneAccess {
    template <class Scenes> static void update(Scenes &scenes, double seconds) {
        SceneDriver::check(scenes);
        navigation_step(seconds);
        // Stage the complete selection before consuming any route or velocity.
        struct Update {
            ComponentRef<navigation::Agent> component;
            navigation::Follower follower;
            Vec3 velocity;
        };
        std::vector<Update> updates;
        for (auto component : scenes.template components<navigation::Agent>()) {
            auto follower = component->follower_;
            Vec3 velocity{};
            if (component.active()) {
                const auto m = component.object().world_matrix();
                velocity = follower.steer(translation_of(m), component->speed_, seconds, component->arrival_distance_);
            }
            updates.push_back({component, std::move(follower), velocity});
        }
        for (auto &update : updates) {
            update.component->follower_ = std::move(update.follower);
            update.component->velocity_ = update.velocity;
        }
    }
};
} // namespace anima::detail
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
void update_agents(Scene &scene, double seconds) { detail::NavigationSceneAccess::update(scene, seconds); }
void update_agents(SceneSet &scenes, double seconds) { detail::NavigationSceneAccess::update(scenes, seconds); }
void add_component_codec(ComponentCodecs &codecs) {
    using Json = nlohmann::json;
    codecs.add<Agent>(
        "anima.navigation-agent.v1",
        [](const Agent &agent, const ObjectReferences &) {
            Json points = Json::array();
            for (const auto p : agent.follower().route())
                points.push_back(Json::array({p.x, p.y, p.z}));
            auto payload = Json{{"route", points},
                                {"next", agent.follower().next()},
                                {"speed", agent.speed()},
                                {"arrival_distance", agent.arrival_distance()}}
                               .dump();
            // The decoder rejects larger payloads, so a route that fits the follower could otherwise be
            // captured but never restored.
            if (payload.size() > maximum_component_bytes)
                throw std::invalid_argument("Navigation agent payload exceeds 16 MiB");
            return payload;
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
                route.push_back({detail::json_float(p[0]), detail::json_float(p[1]), detail::json_float(p[2])});
            }
            object.add_component<Agent>(std::move(route), detail::json_float(j.at("speed")),
                                        detail::json_float(j.at("arrival_distance")), j.at("next").get<std::size_t>());
        });
}
} // namespace anima::navigation
