#pragma once
#include <anima/core/math.hpp>
#include <anima/physics_layers.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
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
/// Two-component vector in the XY plane: core's anima::Vec2, with its arithmetic, length and normalized().
using Vec2 = anima::Vec2;
/// Rigid placement in the XY plane. Poses carry no scale.
struct Pose {
    /// Position in meters; each coordinate within 79,999 of zero.
    Vec2 position{};
    /// Counterclockwise rotation in radians.
    float angle{};
};
/// How the solver moves a body.
enum class Motion {
    stationary, ///< Moved only by Body::teleport; rejects velocities, forces and impulses.
    kinematic,  ///< Moved only by its velocity, Body::move_kinematic or Body::teleport; ignores gravity and contacts.
    dynamic     ///< Simulated under gravity, contacts, forces and impulses.
};
/// Collider geometry kind; selects which Collider fields describe the geometry.
enum class Shape {
    box,    ///< Collider::half_extent.
    circle, ///< Collider::radius.
    capsule ///< Collider::radius and Collider::half_height, along the local Y axis.
};
/// Collider geometry, centered on the body or query origin.
///
/// The dimension fields must each be in [0.01, 10,000] even when #shape does not use them. The
/// static factories set #shape and its fields and leave every other field at its default, which
/// meets this rule.
struct Collider {
    /// Box with @p half_extent, its half size on each axis.
    ///
    /// Like the other factories, it validates nothing; World::create, World::sweep and
    /// World::overlap reject invalid dimensions as they would any other Collider.
    [[nodiscard]] static Collider box(Vec2 half_extent);
    /// Circle of @p radius.
    [[nodiscard]] static Collider circle(float radius);
    /// Capsule along local Y of @p radius, whose straight part is 2 @p half_height long; see
    /// #half_height.
    [[nodiscard]] static Collider capsule(float radius, float half_height);
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
    /// Total dynamic mass, in [0.001, 1,000,000], spread uniformly over the collider's area, which
    /// sets the rotational inertia.
    float mass = 1;
    /// Linear damping c per second, in [0, 60]: a dynamic body's linear velocity decays about as
    /// exp(-c t). Each solver substep of h seconds, World::step's duration divided by
    /// WorldSettings::substeps, scales it by 1 / (1 + c h). Stationary and kinematic bodies are not
    /// damped.
    float linear_damping{};
    /// Angular damping per second, in [0, 60], applied to a dynamic body's angular velocity as
    /// #linear_damping is to its linear velocity.
    float angular_damping{};
    /// Friction coefficient in [0, 1].
    float friction = .5F;
    /// Restitution in [0, 1].
    float restitution{};
    /// Collision layer in [0, 15], below anima::collision_layer_count; see
    /// World::set_layer_collision.
    std::uint8_t layer{};
    /// A sensor reports overlaps as contact events and never collides. An overlap with any body,
    /// another sensor included, is reported when at least one of the two bodies is kinematic or
    /// dynamic; two stationary bodies never report one, as in the 3D module. Overlap is tested once
    /// per step, so a fast body can pass through a sensor unreported.
    bool sensor{};
    /// Makes a dynamic body a Box2D bullet. Every dynamic body has continuous collision against
    /// stationary bodies; bullets also have it against kinematic and non-bullet dynamic bodies,
    /// but not against other bullets or sensors.
    bool continuous{};
    /// Prevents rotation: nonzero angular velocities and kinematic angle changes are rejected, and
    /// torques and angular impulses, including those of forces and impulses applied off the center,
    /// leave the angular velocity unchanged.
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
    /// Mass of a dynamic body, enabled or disabled: BodySettings::mass, up to float rounding.
    /// Throws `std::invalid_argument` for stationary and kinematic bodies.
    [[nodiscard]] float mass() const;
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
    /// Applies @p impulse, in newton seconds, at @p world_point, a world-space position, and wakes
    /// the body. The velocity changes by @p impulse / mass(), and the angular velocity by the
    /// scalar cross product (@p world_point - c) x @p impulse divided by the body's rotational
    /// inertia, except on a fixed-rotation body. The center of mass c is pose().position, up to
    /// float rounding, since every collider is centered on its body. Throws
    /// `std::invalid_argument` unless the body is dynamic and enabled.
    void add_impulse_at(Vec2 impulse, Vec2 world_point);
    /// Applies a counterclockwise angular impulse, in newton meter seconds, and wakes the body: the
    /// angular velocity changes by @p impulse divided by the body's rotational inertia, except on a
    /// fixed-rotation body. Throws `std::invalid_argument` unless the body is dynamic and enabled.
    void add_angular_impulse(float impulse);
    /// Adds a world-space force through the center of mass, in newtons, and wakes the body. Forces
    /// and torques add up until the next World::step, which applies their sums over its whole
    /// duration and then clears them; set_enabled(false) clears them too. Throws
    /// `std::invalid_argument` unless the body is dynamic and enabled.
    void add_force(Vec2 force);
    /// Adds @p force, as add_force() does, and the torque given by the scalar cross product
    /// (@p world_point - c) x @p force, as add_torque() does, where c is the center of mass at the
    /// time of the call, as in add_impulse_at(). Throws `std::invalid_argument` unless the body is
    /// dynamic and enabled.
    void add_force_at(Vec2 force, Vec2 world_point);
    /// Adds a counterclockwise torque, in newton meters, which the next World::step applies as
    /// add_force() describes, and wakes the body. A fixed-rotation body's angular velocity does
    /// not change. Throws `std::invalid_argument` unless the body is dynamic and enabled.
    void add_torque(float torque);
    /// Sets both velocities so a kinematic body reaches @p target after @p seconds, in
    /// [0.000001, 0.1], turning the shorter way; a body already at @p target stops. Box2D's
    /// speed caps can leave a distant target unreached. Throws for other motion types and for a
    /// fixed-rotation body whose angle would change.
    void move_kinematic(Pose target, double seconds);
    /// Disabled bodies leave simulation and queries, end their contacts immediately and reject
    /// forces, torques and impulses, but keep their pose and velocities, which stay editable.
    /// Disabling discards the forces and torques added since the last step. Reenabling resumes
    /// from that state.
    void set_enabled(bool enabled);
    [[nodiscard]] bool enabled() const;
    /// Whether the body is awake: false while it sleeps or is disabled, and for stationary bodies.
    [[nodiscard]] bool awake() const;
    /// Destroys the body and ends its contacts. Idempotent, including after world destruction.
    void remove();
    /// Identity comparison; safe on invalid handles.
    bool operator==(const Body &other) const noexcept;
    /// Total order consistent with operator==, for ordered containers; safe on invalid handles.
    /// Handles order first by their body's position in its world's creation sequence, so a body
    /// created earlier in a world orders first and a default-constructed handle orders before every
    /// body, then by world, in an unspecified order that stays fixed while both handles exist.
    [[nodiscard]] std::strong_ordering operator<=>(const Body &other) const noexcept;

  private:
    friend class World;
    friend struct detail::WorldState;
    friend struct std::hash<Body>;
    Body(std::weak_ptr<detail::WorldState> world, std::uint64_t id) : world_(std::move(world)), id_(id) {}
    std::shared_ptr<detail::WorldState> lock() const;
    std::weak_ptr<detail::WorldState> world_;
    std::uint64_t id_{};
};
/// Selects which bodies a query considers. Queries never report disabled bodies.
struct QueryFilter {
    /// Bit mask of collision layers to include, regardless of World::set_layer_collision; build
    /// one with anima::collision_layer_mask.
    std::uint16_t layers = anima::all_collision_layers;
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
/// Where a solid contact began and how fast its bodies met, as Box2D found the contact, before its
/// solver resolved it.
struct ContactPoint {
    /// World-space point in meters: the mean of Box2D's contact points, each midway between the
    /// two surfaces.
    Vec2 point{};
    /// Unit normal pointing from ContactEvent::first toward ContactEvent::second.
    Vec2 normal{};
    /// Speed at which the bodies approached along #normal at the start of the step, in meters per
    /// second and never negative: the largest at any contact point the solver pushed on. Box2D
    /// measures it only for a contact its solver pushes on in the step the contact begins, so a
    /// contact that begins while its bodies are still apart, by less than Box2D's speculative
    /// distance of 0.02 meters, and that they do not close within that step reports zero.
    float approach_speed{};
};
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
    /// Set on the begin event of a solid contact and empty on end events and sensor overlaps.
    std::optional<ContactPoint> contact;
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
    /// symmetrically. All pairs collide initially. Throws `std::invalid_argument` ("2D physics
    /// layer must be in [0,15]") for a layer out of range and `std::logic_error` while the world
    /// has bodies.
    void set_layer_collision(std::uint8_t a, std::uint8_t b, bool collide);
    /// Whether bodies of layers @p a and @p b, in [0, 15], make contacts and sensor overlaps, as
    /// set by set_layer_collision; the answer is symmetric. Throws `std::invalid_argument` ("2D
    /// physics layer must be in [0,15]") for a layer out of range.
    [[nodiscard]] bool layers_collide(std::uint8_t a, std::uint8_t b) const;
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
namespace std {
/// Hashes body handles consistently with anima::physics2d::Body::operator==, so equal handles,
/// invalid ones included, hash alike and handles can key unordered containers. Bodies of different
/// worlds can share a hash.
template <> struct hash<anima::physics2d::Body> {
    [[nodiscard]] size_t operator()(const anima::physics2d::Body &body) const noexcept {
        return hash<uint64_t>{}(body.id_);
    }
};
} // namespace std
