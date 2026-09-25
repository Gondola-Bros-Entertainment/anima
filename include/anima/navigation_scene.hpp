#pragma once
#include <anima/navigation.hpp>
#include <anima/scene.hpp>
namespace anima::navigation {
class Agent {
  public:
    Agent(std::vector<Vec3> route = {}, float speed = 1, float arrival_distance = .05F, std::size_t next = 0);
    void set_speed(float speed);
    void set_arrival_distance(float distance);
    void set_route(std::vector<Vec3> route, std::size_t next = 0);
    [[nodiscard]] const Follower &follower() const { return follower_; }
    [[nodiscard]] float speed() const { return speed_; }
    [[nodiscard]] float arrival_distance() const { return arrival_distance_; }
    [[nodiscard]] Vec3 desired_velocity() const { return velocity_; }

  private:
    friend void update_agents(Scene &, double);
    Follower follower_;
    float speed_, arrival_distance_;
    Vec3 velocity_{};
};
// Derives desired velocities from world positions. Does not move objects, call
// Scene updates, advance physics or run callbacks. Disabled agents output zero.
void update_agents(Scene &scene, double seconds);
// Route points/cursor and steering parameters; no backend or world binding.
void add_component_codec(ComponentCodecs &codecs);
} // namespace anima::navigation
