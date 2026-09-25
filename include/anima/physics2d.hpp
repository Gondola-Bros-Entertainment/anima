#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace anima {
class ComponentCodecs;
}
namespace anima::physics2d {
namespace detail {
struct WorldState;
}
struct Vec2 {
    float x{}, y{};
};
struct Pose {
    Vec2 position{};
    float angle{};
}; // Radians, counterclockwise in XY.
enum class Motion { stationary, kinematic, dynamic };
enum class Shape { box, circle, capsule };
struct Collider {
    Shape shape = Shape::box;
    Vec2 half_extent{.5F, .5F};
    float radius = .5F, half_height = .5F; // Capsule straight section half-height, Y axis.
};
struct BodySettings {
    Collider collider;
    Motion motion = Motion::stationary;
    Pose pose;
    Vec2 velocity{};
    float angular_velocity{}, density = 1, friction = .5F, restitution{};
    std::uint8_t layer{}; // 0..15; collision matrix configured before body creation.
    bool sensor{}, continuous{}, fixed_rotation{};
};
struct WorldSettings {
    Vec2 gravity{0, -9.81F};
    std::uint32_t max_bodies = 4096;
    unsigned substeps = 4; // 1..16 solver substeps per fixed tick.
    bool sleeping = true;
};
// Checked non-owning identity. Removing a body or destroying its world invalidates
// all copies, even after backend slot reuse. Use all worlds on one caller thread.
class Body {
  public:
    Body() = default;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] Pose pose() const;
    [[nodiscard]] Vec2 velocity() const;
    [[nodiscard]] float angular_velocity() const;
    void teleport(Pose pose);
    void set_velocity(Vec2 velocity);
    void set_angular_velocity(float velocity);
    void add_impulse(Vec2 impulse);
    void move_kinematic(Pose target, double seconds);
    void set_enabled(bool enabled);
    [[nodiscard]] bool enabled() const;
    [[nodiscard]] bool awake() const;
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
    bool sensors{};
    Body ignore;
};
struct Hit {
    Body body;
    float fraction{};       // Fraction of full displacement [0,1].
    Vec2 point{}, normal{}; // World-space point and outward surface normal.
    bool initial_overlap{}; // At fraction zero: point/normal unavailable (zero).
};
enum class ContactPhase { begin, end };
struct ContactEvent {
    Body first, second; // End identities may be stale after removal.
    ContactPhase phase{};
    bool sensor{};
};
// Optional Box2D runtime, no assets/SDL/Vulkan/Jolt and no backend types in this API.
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
    void step(double seconds); // [0.000001, 0.1], caller supplies fixed cadence.
    [[nodiscard]] std::vector<ContactEvent> take_events();
    [[nodiscard]] std::optional<Hit> raycast(Vec2 origin, Vec2 displacement, QueryFilter filter = {}) const;
    [[nodiscard]] std::optional<Hit> sweep(const Collider &collider, Pose start, Vec2 displacement,
                                           QueryFilter filter = {}) const;
    [[nodiscard]] std::vector<Body> overlap(const Collider &collider, Pose pose, QueryFilter filter = {}) const;

  private:
    friend void add_component_codec(ComponentCodecs &, World &);
    std::shared_ptr<detail::WorldState> state_;
    std::shared_ptr<int> lifetime_ = std::make_shared<int>(0);
};
} // namespace anima::physics2d
