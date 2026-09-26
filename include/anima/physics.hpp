#pragma once
#include <anima/core/transform.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace anima {
class ComponentCodecs;
}
namespace anima::physics {
namespace detail {
struct WorldState;
}
enum class Motion { stationary, kinematic, dynamic };
enum class Shape { box, sphere, capsule, mesh, convex_hull, compound };
struct ColliderChild;
struct Collider {
    Shape shape = Shape::box;
    Vec3 half_extent{.5F, .5F, .5F};
    float radius = .5F, half_height = .5F; // Capsule cylinder half-height, Y axis.
    std::vector<Vec3> vertices;            // Mesh: CCW indexed triangles. Hull: 4..256 points spanning a volume.
    std::vector<std::uint32_t> indices;
    std::vector<ColliderChild> children; // Compound: 1..64 primitive/hull children, no nesting.
};
struct Pose {
    Vec3 position{};
    Quat rotation{0, 0, 0, 1};
};
struct ColliderChild {
    Pose pose; // Local authored origin/rotation; no scale. Not the child's center of mass.
    Collider collider;
};
struct BodySettings {
    Collider collider;
    Motion motion = Motion::stationary;
    Pose pose;
    Vec3 velocity{}, angular_velocity{}; // Linear velocity is at the center of mass.
    float mass = 1, friction = .5F, restitution = 0;
    std::uint8_t layer = 0; // 0..15. World collision matrix defaults to all pairs.
    bool sensor = false, continuous = false;
    // Absolute body-local center of mass. Empty computes it from collider volume.
    // Does not move collider geometry; immutable after creation. Not supported for mesh.
    std::optional<Vec3> center_of_mass;
};
struct WorldSettings {
    Vec3 gravity{0, -9.81F, 0};
    std::uint32_t max_bodies = 4096;
};
// Checked, non-owning identity. All operations use the world's calling thread.
// Removing a body or destroying its world invalidates every copy immediately.
class Body {
  public:
    Body() = default;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] Pose pose() const;
    [[nodiscard]] Vec3 local_center_of_mass() const;
    [[nodiscard]] Vec3 world_center_of_mass() const;
    // Linear velocity and impulse are at the center of mass, not the authored origin.
    [[nodiscard]] Vec3 velocity() const;
    [[nodiscard]] Vec3 angular_velocity() const;
    void teleport(Pose pose);
    void set_velocity(Vec3 velocity);
    void set_angular_velocity(Vec3 velocity);
    void add_impulse(Vec3 impulse); // Enabled dynamic bodies only.
    void move_kinematic(Pose target, double seconds);
    void set_enabled(bool enabled);
    [[nodiscard]] bool enabled() const;
    void remove(); // Idempotent, including after world destruction.
    bool operator==(const Body &other) const noexcept;

  private:
    friend class World;
    friend struct detail::WorldState;
    Body(std::weak_ptr<detail::WorldState> world, std::uint64_t id) : world_(std::move(world)), id_(id) {}
    std::shared_ptr<detail::WorldState> lock() const;
    std::weak_ptr<detail::WorldState> world_;
    std::uint64_t id_{};
};
struct QueryFilter {
    std::uint16_t layers = 0xffff;
    bool sensors = false;
    Body ignore;
};
struct Hit {
    Body body;
    float fraction{};       // [0,1] of the complete displacement, not a distance.
    Vec3 point{}, normal{}; // World space; normal points out of the hit surface.
    float penetration{};
};
enum class ContactPhase { begin, end };
struct ContactEvent {
    Body first, second; // May be stale for end events after explicit removal.
    ContactPhase phase{};
    bool sensor{};
};
// Optional anima::physics, no assets/SDL/Vulkan. One caller thread; no application
// callbacks inside Jolt. Multiple independent worlds may coexist on that thread.
class World {
  public:
    explicit World(WorldSettings settings = {});
    ~World();
    World(const World &) = delete;
    World &operator=(const World &) = delete;
    [[nodiscard]] Body create(const BodySettings &settings);
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool owns(const Body &body) const noexcept;
    void set_layer_collision(std::uint8_t a, std::uint8_t b, bool collide);
    void step(double seconds); // [0.000001, 0.1] seconds; caller supplies fixed cadence.
    [[nodiscard]] std::vector<ContactEvent> take_events();
    [[nodiscard]] std::optional<Hit> raycast(Vec3 origin, Vec3 displacement, QueryFilter filter = {}) const;
    [[nodiscard]] std::optional<Hit> sweep(const Collider &shape, Pose start, Vec3 displacement,
                                           QueryFilter filter = {}) const;
    [[nodiscard]] std::vector<Hit> overlap(const Collider &shape, Pose pose, QueryFilter filter = {}) const;

  private:
    friend void add_component_codec(ComponentCodecs &, World &);
    std::shared_ptr<detail::WorldState> state_;
    std::shared_ptr<int> lifetime_ = std::make_shared<int>(0);
};
} // namespace anima::physics
