#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

/// @file
/// 2D rigid-body simulation and queries over a private Box2D backend.
///
/// Part of the optional `anima::physics2d` target (`ANIMA_BUILD_PHYSICS2D=ON`); no Box2D type
/// appears in the public API. Coordinates are XY with +Y up, and angles are counterclockwise
/// radians. Lengths are meters, which Box2D's tolerances assume, and times are seconds.
///
/// Inputs are validated before they reach the backend. Vector components and angles must be
/// finite and within 1,000,000 of zero, and each coordinate of a Pose position within 79,999:
/// Box2D keeps every bounding box within 100,000 meters of the origin, and a collider reaches up
/// to 20,000 meters from its body. These are validation bounds, not precision guarantees.
/// Invalid arguments throw `std::invalid_argument` unless a member states otherwise. Box2D caps
/// linear speed at 400 meters per second and rotation at pi/4 radians per step.
///
/// Use all worlds and their bodies from one application thread; Box2D runs on it and calls no
/// application code. Each body has one centered box, circle or capsule collider. There are no
/// joints, polygons, chains, compound colliders or character controllers.

namespace anima {
class ComponentCodecs;
}
namespace anima::physics2d {
namespace detail {
struct WorldState;
}
/// Two-component vector in the XY plane.
struct Vec2 {
    float x{};
    float y{};
};
/// Rigid placement in the XY plane. Poses carry no scale.
struct Pose {
    /// Position in meters; each coordinate within 79,999 of zero.
    Vec2 position{};
    /// Counterclockwise rotation in radians.
    float angle{};
};
/// How the solver moves a body.
enum class Motion {
    stationary, ///< Moved only by Body::teleport; rejects velocities and impulses.
    kinematic,  ///< Moved only by its velocity, Body::move_kinematic or Body::teleport; ignores gravity and contacts.
    dynamic     ///< Simulated under gravity, contacts and impulses.
};
/// Collider geometry kind; selects which Collider fields describe the geometry.
enum class Shape {
    box,    ///< Collider::half_extent.
    circle, ///< Collider::radius.
    capsule ///< Collider::radius and Collider::half_height, along the local Y axis.
};
/// Collider geometry, centered on the body or query origin.
///
/// The dimension fields must each be in [0.01, 10,000] even when #shape does not use them.
struct Collider {
    Shape shape = Shape::box;
    /// Box half size on each axis.
    Vec2 half_extent{.5F, .5F};
    /// Circle or capsule radius.
    float radius = .5F;
    /// Half the capsule's straight length along local Y, excluding the rounded ends.
    float half_height = .5F;
};
/// Everything World::create needs to build one body.
struct BodySettings {
    Collider collider;
    Motion motion = Motion::stationary;
    /// Initial world pose. anima::physics2d::RigidBody ignores it and uses its object's pose.
    Pose pose;
    /// Initial linear velocity; must be zero for stationary bodies.
    Vec2 velocity{};
    /// Initial counterclockwise angular velocity in radians per second; must be zero for
    /// stationary and fixed-rotation bodies.
    float angular_velocity{};
    /// Mass per unit area, in [0.001, 10,000]; a dynamic body's mass is density times area.
    float density = 1;
    /// Friction coefficient in [0, 1].
    float friction = .5F;
    /// Restitution in [0, 1].
    float restitution{};
    /// Collision layer in [0, 15]; see World::set_layer_collision.
    std::uint8_t layer{};
    /// A sensor detects bodies of every motion type, including other sensors, and reports
    /// overlaps as contact events; it never collides. Overlap is tested once per step, so a fast
    /// body can pass through a sensor unreported.
    bool sensor{};
    /// Makes a dynamic body a Box2D bullet. Every dynamic body has continuous collision against
    /// stationary bodies; bullets also have it against kinematic and non-bullet dynamic bodies,
    /// but not against other bullets or sensors.
    bool continuous{};
    /// Prevents rotation: nonzero angular velocities and kinematic angle changes are rejected.
    bool fixed_rotation{};
};
/// World construction options.
struct WorldSettings {
    Vec2 gravity{0, -9.81F};
    /// Body capacity, in [1, 1,000,000]. World::create throws `std::length_error` beyond it.
    std::uint32_t max_bodies = 4096;
    /// Solver substeps per World::step, in [1, 16].
    unsigned substeps = 4;
    /// Whether resting bodies may sleep. Sleep ends no contacts or sensor overlaps.
    bool sleeping = true;
};
/// Checked, non-owning identity of a body in a World.
///
/// Copies share one identity, and dropping them leaves the body alive. Removing the body or
/// destroying its world invalidates every copy, and identities are never reused; any later
/// access except valid(), remove() and comparison throws `std::out_of_range`.
class Body {
  public:
    Body() = default;
    /// Whether the body still exists.
    [[nodiscard]] bool valid() const noexcept;
    /// Current pose, with the angle in [-pi, pi].
    [[nodiscard]] Pose pose() const;
    /// Linear velocity, or the saved value while disabled.
    [[nodiscard]] Vec2 velocity() const;
    /// Angular velocity in radians per second, or the saved value while disabled.
    [[nodiscard]] float angular_velocity() const;
    /// Moves the body to @p pose immediately and wakes it.
    void teleport(Pose pose);
    /// Sets linear velocity, or the saved value while disabled. Throws for stationary bodies.
    void set_velocity(Vec2 velocity);
    /// Sets angular velocity, or the saved value while disabled. Throws for stationary bodies,
    /// and for a nonzero @p velocity on fixed-rotation bodies.
    void set_angular_velocity(float velocity);
    /// Applies a linear impulse at the center of mass and wakes the body. Throws unless the body
    /// is dynamic and enabled.
    void add_impulse(Vec2 impulse);
    /// Sets both velocities so a kinematic body reaches @p target after @p seconds, in
    /// [0.000001, 0.1], turning the shorter way; a body already at @p target stops. Box2D's
    /// speed caps can leave a distant target unreached. Throws for other motion types and for a
    /// fixed-rotation body whose angle would change.
    void move_kinematic(Pose target, double seconds);
    /// Disabled bodies leave simulation and queries, end their contacts immediately and reject
    /// impulses, but keep their pose and velocities, which stay editable. Reenabling resumes
    /// from that state.
    void set_enabled(bool enabled);
    [[nodiscard]] bool enabled() const;
    /// Whether the body is awake: false while it sleeps or is disabled, and for stationary bodies.
    [[nodiscard]] bool awake() const;
    /// Destroys the body and ends its contacts. Idempotent, including after world destruction.
    void remove();
    /// Identity comparison; safe on invalid handles.
    bool operator==(const Body &other) const noexcept;

  private:
    friend class World;
    friend struct detail::WorldState;
    Body(std::weak_ptr<detail::WorldState> world, std::uint64_t id) : world_(std::move(world)), id_(id) {}
    std::shared_ptr<detail::WorldState> lock() const;
    std::weak_ptr<detail::WorldState> world_;
    std::uint64_t id_{};
};
/// Selects which bodies a query considers. Queries never report disabled bodies.
struct QueryFilter {
    /// Bit mask of collision layers to include, regardless of World::set_layer_collision.
    std::uint16_t layers = 0xffff;
    /// Whether sensor bodies can be hit.
    bool sensors{};
    /// A body of the queried world to skip; an invalid handle skips nothing, and the query
    /// throws for another world's body.
    Body ignore;
};
/// One query result.
struct Hit {
    Body body;
    /// Position along the complete displacement, in [0, 1]; not a distance.
    float fraction{};
    /// World-space contact point; zero for an initial overlap.
    Vec2 point{};
    /// World-space normal pointing out of the hit surface; zero for an initial overlap.
    Vec2 normal{};
    /// The cast started overlapping #body, or within 0.0005 of it, so #fraction is zero. When
    /// several bodies qualify, which one is reported is unspecified.
    bool initial_overlap{};
};
enum class ContactPhase { begin, end };
/// A change in contact between two bodies, either a solid contact or a sensor overlap. Solid
/// contacts need at least one dynamic body.
struct ContactEvent {
    /// For end events after an explicit removal these may already be invalid; compare their
    /// identity without reading body state.
    Body first;
    Body second;
    ContactPhase phase{};
    /// Whether either body is a sensor.
    bool sensor{};
};
/// Owns bodies and steps their simulation.
///
/// Worlds are independent and may coexist on the one physics thread, up to Box2D's limit of 128
/// live worlds. Destroying a world destroys its bodies and invalidates their handles. It makes no
/// cross-platform determinism guarantee.
class World {
  public:
    /// Throws `std::invalid_argument` for invalid settings and `std::length_error` when 128
    /// worlds already exist.
    explicit World(WorldSettings settings = {});
    ~World();
    World(const World &) = delete;
    World &operator=(const World &) = delete;
    /// Creates a body. Throws `std::invalid_argument` for invalid settings and
    /// `std::length_error` at WorldSettings::max_bodies.
    [[nodiscard]] Body create(const BodySettings &settings);
    /// Number of existing bodies.
    [[nodiscard]] std::size_t size() const noexcept;
    /// Whether @p body is a valid body of this world.
    [[nodiscard]] bool owns(const Body &body) const noexcept;
    /// Enables or disables contacts and sensor overlaps between layers @p a and @p b, in [0, 15],
    /// symmetrically. All pairs collide initially. Throws `std::logic_error` while the world has
    /// bodies.
    void set_layer_collision(std::uint8_t a, std::uint8_t b, bool collide);
    /// Advances one simulation step of @p seconds, in [0.000001, 0.1]. Drive it from a fixed
    /// cadence such as anima::FixedStepClock.
    void step(double seconds);
    /// Drains contact events recorded by step(), Body::remove() and Body::set_enabled().
    [[nodiscard]] std::vector<ContactEvent> take_events();
    /// Casts a ray from @p origin along the complete @p displacement, which must be longer than
    /// 0.000001. Returns the closest hit, or a Hit::initial_overlap result when @p origin is
    /// inside a body.
    [[nodiscard]] std::optional<Hit> raycast(Vec2 origin, Vec2 displacement, QueryFilter filter = {}) const;
    /// Translates @p collider from @p start along @p displacement, which must be longer than
    /// 0.000001, without rotating it. Returns the closest hit, or a Hit::initial_overlap result
    /// when the collider starts overlapping a body. Use overlap() for zero-displacement checks.
    [[nodiscard]] std::optional<Hit> sweep(const Collider &collider, Pose start, Vec2 displacement,
                                           QueryFilter filter = {}) const;
    /// Returns each body that overlaps @p collider at @p pose or lies within 0.0005 of it, once
    /// and in unspecified order, without penetration depths.
    [[nodiscard]] std::vector<Body> overlap(const Collider &collider, Pose pose, QueryFilter filter = {}) const;

  private:
    friend void add_component_codec(ComponentCodecs &, World &);
    std::shared_ptr<detail::WorldState> state_;
    std::shared_ptr<int> lifetime_ = std::make_shared<int>(0);
};
} // namespace anima::physics2d
