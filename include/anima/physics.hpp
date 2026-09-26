#pragma once
#include <anima/core/transform.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

/// @file
/// Rigid-body simulation and queries over a private Jolt Physics backend.
///
/// Part of the optional `anima::physics` target (`ANIMA_BUILD_PHYSICS=ON`); no Jolt type
/// appears in the public API. Coordinates are right-handed and Y-up, in caller-consistent
/// units (normally meters and kilograms). Rotations are XYZW quaternions, normalized on
/// input by anima::unit_quaternion.
///
/// Inputs are validated before they reach the backend. Vector components must be finite and
/// within 1,000,000 of zero; this is a validation bound, not a precision guarantee. Invalid
/// arguments throw `std::invalid_argument`, or anima::MathError (derived from it) for a
/// rotation that cannot be normalized, unless a member states otherwise.
///
/// Use all worlds and their bodies from one application thread. Anima owns Jolt's process
/// registration, so other Jolt users cannot share the process. There are no joints, character
/// controller, per-child materials, runtime collider edits or debug drawing.

namespace anima {
class ComponentCodecs;
}
namespace anima::physics {
namespace detail {
struct WorldState;
}
/// How the solver moves a body.
enum class Motion {
    stationary, ///< Never moves. Mesh colliders require this mode.
    kinematic,  ///< Moved only by its velocity, Body::move_kinematic or Body::teleport; ignores forces and contacts.
                ///< Contacts stationary and kinematic bodies only when either body is a sensor.
    dynamic     ///< Simulated under gravity, contacts and impulses.
};
/// Collider geometry kind; selects which Collider fields describe the geometry.
enum class Shape {
    box,         ///< Collider::half_extent.
    sphere,      ///< Collider::radius.
    capsule,     ///< Collider::radius and Collider::half_height, along the local Y axis.
    mesh,        ///< Triangles from Collider::vertices and Collider::indices; stationary bodies only.
    convex_hull, ///< Convex hull of Collider::vertices.
    compound     ///< Collider::children.
};
struct ColliderChild;
/// Collider geometry. A body compiles its own copy at creation, so these arrays may change
/// or be destroyed afterwards; the body's shape is then immutable.
///
/// The dimension fields must each be in [0.001, 1,000,000] even when #shape does not use
/// them, so direct calls and persisted data share one contract.
struct Collider {
    Shape shape = Shape::box;
    /// Box half size on each axis.
    Vec3 half_extent{.5F, .5F, .5F};
    /// Sphere or capsule radius.
    float radius = .5F;
    /// Half the capsule cylinder's length along local Y, excluding the hemispheres.
    float half_height = .5F;
    /// Mesh: 3 to 1,000,000 vertices. Hull: 4 to 256 points in any order, which may include
    /// interior or duplicate points but must span a volume (line, plane and depth spans of at
    /// least 0.001). A hull does not decompose concave input. Empty for other shapes.
    std::vector<Vec3> vertices;
    /// Mesh only: counterclockwise front-facing triangles, at most 3,000,000 indices, each in
    /// range and none degenerate. Empty for other shapes.
    std::vector<std::uint32_t> indices;
    /// Compound only: 1 to 64 primitive or hull children; meshes and nested compounds are
    /// rejected. Empty for other shapes.
    std::vector<ColliderChild> children;
};
/// Rigid placement of a body's authored origin. Poses carry no scale.
struct Pose {
    Vec3 position{};
    Quat rotation{0, 0, 0, 1};
};
/// One part of a Shape::compound collider.
struct ColliderChild {
    /// Body-local placement of the child's authored geometry; not the child's center of mass.
    Pose pose;
    /// Primitive or hull geometry.
    Collider collider;
};
/// Everything World::create needs to build one body.
struct BodySettings {
    Collider collider;
    Motion motion = Motion::stationary;
    /// Initial authored origin in world space. anima::physics::RigidBody ignores it and uses
    /// its object's world pose.
    Pose pose;
    /// Linear velocity at the center of mass; must be zero for stationary bodies.
    Vec3 velocity{};
    /// Angular velocity; must be zero for stationary bodies.
    Vec3 angular_velocity{};
    /// Total dynamic mass, in [0.001, 1,000,000]. Automatic centers of mass and inertia assume
    /// uniform density; compound children contribute by volume, overlapping volumes separately.
    float mass = 1;
    /// Friction coefficient in [0, 1].
    float friction = .5F;
    /// Restitution in [0, 1].
    float restitution = 0;
    /// Collision layer in [0, 15]; see World::set_layer_collision.
    std::uint8_t layer = 0;
    /// Sensors report contact events but never resolve penetration. An overlap with any body,
    /// another sensor included, is reported when at least one of the two bodies is kinematic or
    /// dynamic; two stationary bodies never report one, as in the 2D module.
    bool sensor = false;
    /// Uses continuous collision detection (linear cast) instead of discrete steps.
    bool continuous = false;
    /// Absolute body-local center of mass; empty computes it from the collider volume.
    ///
    /// Moves the rotation center and shifts inertia by the parallel-axis rule, scaled to
    /// #mass. It does not move the collider, the object or any visible mesh, and it cannot
    /// change after creation. Mesh colliders reject an override.
    std::optional<Vec3> center_of_mass;
};
/// World construction options.
struct WorldSettings {
    Vec3 gravity{0, -9.81F, 0};
    /// Body capacity, in [1, 65,536]. World::create throws `std::length_error` beyond it.
    std::uint32_t max_bodies = 4096;
};
/// Checked, non-owning identity of a body in a World.
///
/// Copies share one identity, and dropping them leaves the body alive. Removing the body or
/// destroying its world invalidates every copy; any later access except valid(), remove()
/// and comparison throws `std::out_of_range`.
class Body {
  public:
    Body() = default;
    /// Whether the body still exists.
    [[nodiscard]] bool valid() const noexcept;
    /// World pose of the authored origin.
    [[nodiscard]] Pose pose() const;
    /// Center of mass in body-local space.
    [[nodiscard]] Vec3 local_center_of_mass() const;
    /// Center of mass in world space, including the body's rotation and translation.
    [[nodiscard]] Vec3 world_center_of_mass() const;
    /// Linear velocity at the center of mass, not the authored origin.
    [[nodiscard]] Vec3 velocity() const;
    [[nodiscard]] Vec3 angular_velocity() const;
    /// Moves the authored origin to @p pose immediately and wakes the body.
    void teleport(Pose pose);
    /// Sets linear velocity at the center of mass. Throws for stationary bodies.
    void set_velocity(Vec3 velocity);
    /// Throws for stationary bodies.
    void set_angular_velocity(Vec3 velocity);
    /// Applies an impulse at the center of mass. Throws unless the body is dynamic and enabled.
    void add_impulse(Vec3 impulse);
    /// Moves a kinematic body so its authored origin reaches @p target after @p seconds, in
    /// [0.000001, 0.1]. Throws for other motion types.
    void move_kinematic(Pose target, double seconds);
    /// Disabled bodies keep their pose and velocities and accept writes to them, but leave
    /// collisions and queries, end their contacts and reject impulses. Reenabling resumes from
    /// that state and reports contacts that still touch as new begin events.
    void set_enabled(bool enabled);
    [[nodiscard]] bool enabled() const;
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
/// Selects which bodies a query considers.
struct QueryFilter {
    /// Bit mask of collision layers to include.
    std::uint16_t layers = 0xffff;
    /// Whether sensor bodies can be hit.
    bool sensors = false;
    /// A body of the queried world to skip; an invalid handle skips nothing.
    Body ignore;
};
/// One query result.
struct Hit {
    Body body;
    /// Position along the complete displacement, in [0, 1]; not a distance.
    float fraction{};
    /// World-space contact point.
    Vec3 point{};
    /// World-space normal pointing from the hit surface toward the query: back along a ray or
    /// sweep, including on mesh undersides.
    Vec3 normal{};
    /// Overlap depth for World::overlap results.
    float penetration{};
};
enum class ContactPhase { begin, end };
/// A change in contact between two bodies, aggregated across compound children and mesh
/// triangles: a compound's contact ends only when its last touching child separates.
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
/// Worlds are independent and may coexist on the one physics thread. Destroying a world
/// destroys its bodies and invalidates their handles. The backend uses a single-threaded job
/// system and disables sleeping so contact transitions stay stable; it makes no throughput or
/// cross-platform determinism guarantee.
class World {
  public:
    /// Throws `std::invalid_argument` for invalid settings and `std::logic_error` when
    /// something other than Anima initialized Jolt in this process.
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
    /// Enables or disables contacts between two layers, symmetrically. All pairs collide
    /// initially. Throws `std::logic_error` once the world has bodies.
    void set_layer_collision(std::uint8_t a, std::uint8_t b, bool collide);
    /// Advances one simulation step of @p seconds, in [0.000001, 0.1]. Drive it from a fixed
    /// cadence such as anima::FixedStepClock. Throws `std::runtime_error` when contacts exceed
    /// capacity; the partially advanced step is not rolled back.
    void step(double seconds);
    /// Drains contact events recorded by step(), Body::remove() and Body::set_enabled().
    [[nodiscard]] std::vector<ContactEvent> take_events();
    /// Casts a ray along the complete @p displacement, which must be nonzero. Meshes are hit
    /// from both sides.
    [[nodiscard]] std::optional<Hit> raycast(Vec3 origin, Vec3 displacement, QueryFilter filter = {}) const;
    /// Translates @p shape along @p displacement, which must be nonzero, without rotating it.
    /// @p start is the shape's authored origin. The shape cannot be a mesh; meshes are hit from
    /// both sides. Use overlap() for zero-displacement clearance checks.
    [[nodiscard]] std::optional<Hit> sweep(const Collider &shape, Pose start, Vec3 displacement,
                                           QueryFilter filter = {}) const;
    /// Returns every body contact of @p shape at @p pose, with penetration depths; a body can
    /// appear more than once. The shape cannot be a mesh.
    [[nodiscard]] std::vector<Hit> overlap(const Collider &shape, Pose pose, QueryFilter filter = {}) const;

  private:
    friend void add_component_codec(ComponentCodecs &, World &);
    std::shared_ptr<detail::WorldState> state_;
    std::shared_ptr<int> lifetime_ = std::make_shared<int>(0);
};
} // namespace anima::physics
