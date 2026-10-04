#pragma once
#include <anima/navigation.hpp>
#include <anima/scene.hpp>

/// @file
/// Scene integration for anima::navigation: an Agent component, its driver and a persistence codec.
///
/// Part of the `anima::assets` target. Invalid arguments throw `std::invalid_argument`.

namespace anima::detail {
struct NavigationSceneAccess;
}
namespace anima::navigation {
/// Component that follows a route and publishes a desired velocity for the application to apply.
///
/// update_agents() advances the route and computes the velocity; the agent never moves its object. Speeds are in
/// world units per second and distances in world units. A rejected setter leaves the agent unchanged.
class Agent {
  public:
    /// Throws for an invalid route or cursor (see Follower::set_route), or for @p settings that Follower::steer
    /// rejects.
    explicit Agent(std::vector<Vec3> route = {}, SteerSettings settings = {}, std::size_t next = 0);
    /// Validates @p settings as Follower::steer does, then replaces the settings and clears desired_velocity().
    void configure(SteerSettings settings);
    /// Replaces the route and cursor as Follower::set_route does, and clears desired_velocity().
    void set_route(std::vector<Vec3> route, std::size_t next = 0);
    /// Route and cursor; update_agents() advances the cursor.
    [[nodiscard]] const Follower &follower() const { return follower_; }
    /// Settings that update_agents() steers with.
    [[nodiscard]] const SteerSettings &settings() const { return settings_; }
    /// Velocity computed by the last update_agents() call. It is zero until the first call, after configure() or
    /// set_route(), and when the agent was inactive or its route finished at that call.
    [[nodiscard]] Vec3 desired_velocity() const { return velocity_; }

  private:
    friend struct anima::detail::NavigationSceneAccess;
    Follower follower_;
    SteerSettings settings_;
    Vec3 velocity_{};
};
/// Updates every Agent in @p scene for a step of @p seconds, in [0.000001, 0.1].
///
/// An agent whose ComponentRef::active() is true steers from its object's world position with its settings, as
/// Follower::steer does; any other agent publishes zero and keeps its cursor. Every agent is computed before any is
/// published, so a rejected object position changes no agent. The driver moves no objects and calls no component
/// hooks. Throws `std::logic_error` while the scene is updating, constructing a component or being destroyed.
void update_agents(Scene &scene, double seconds);
/// Updates every scene of @p scenes as update_agents(Scene &, double) does, computing all agents before publishing
/// any. Also throws `std::logic_error` while the set is busy, as SceneSet describes.
void update_agents(SceneSet &scenes, double seconds);
/// Registers the `anima.navigation-agent.v2` component codec for Agent. It binds no world or other service. Throws
/// when @p codecs already has an Agent codec or that key.
///
/// The payload stores the route, cursor and settings; component enablement is stored separately, and a restored
/// agent's desired velocity starts at zero. It is a JSON object of at most 16 MiB with exactly the fields `route`
/// (an array of three-number arrays), `next` (an integer), `speed`, `arrival_distance` (numbers) and `plane` (the
/// string `"xyz"` or `"xz"`), validated as Agent validates them. Text that is not JSON, unknown, missing or
/// duplicate fields and invalid values throw `std::invalid_argument`, and capture throws it for a payload over
/// 16 MiB, which a route of a few hundred thousand waypoints can reach.
void add_component_codec(ComponentCodecs &codecs);
} // namespace anima::navigation
