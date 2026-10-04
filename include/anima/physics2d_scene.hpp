#pragma once
#include <anima/physics2d.hpp>
#include <anima/scene.hpp>

/// @file
/// Scene and prefab integration for anima::physics2d: a RigidBody component, a fixed-step driver
/// and a persistence codec. Part of the optional `anima::physics2d_scene` target.

namespace anima::physics2d {
namespace detail {
struct RigidBodyAccess;
}
/// Component that owns one body for its GameObject.
///
/// The body starts at the object's world pose; BodySettings::pose is ignored. That transform must
/// be planar and rigid: unit scale and rotation only about Z, with no tilt, shear or reflection.
/// Dynamic bodies must be scene roots; stationary and kinematic bodies may have parents. Object Z
/// is presentation depth: the simulation ignores it and dynamic updates keep it. Size belongs in
/// the Collider, not in object scale. Destroying the component removes its body, including after
/// the world is gone.
///
/// The world reflects the scene as of the last step(): the driver applies the component's active
/// state and its object's pose only when it runs. Until then, the body of a component that was
/// disabled, deactivated or restored disabled still takes part in queries and contacts.
class RigidBody {
  public:
    /// Creates the body in @p world at the world pose of @p owner's object. Throws
    /// `std::invalid_argument` for a dynamic child object, a transform that is not planar and rigid
    /// or has an out-of-range XY position, or invalid settings (see World::create).
    RigidBody(ComponentOwner owner, World &world, BodySettings settings = {});
    ~RigidBody();
    RigidBody(const RigidBody &) = delete;
    RigidBody &operator=(const RigidBody &) = delete;
    /// Handle to the owned body, for velocities, impulses and teleports. Each step() teleports a
    /// stationary body to its object's pose and sets a kinematic body's velocities to reach its
    /// object's pose as Body::move_kinematic does, so a teleport or velocity written to either
    /// lasts only until then; move those bodies through their objects' transforms. A dynamic
    /// body keeps a teleport unless its object's transform is also edited before the next
    /// step(), which then teleports the body to the object.
    [[nodiscard]] Body body() const { return body_; }
    /// Creation settings, with BodySettings::pose set to the object's starting pose.
    [[nodiscard]] const BodySettings &settings() const { return settings_; }

  private:
    friend struct detail::RigidBodyAccess;
    BodySettings settings_;
    Body body_;
    // The object's world matrix at construction or at step()'s last write-back of a dynamic pose.
    Mat4 published_;
};
/// Advances every RigidBody in @p scene by one fixed step of @p seconds, in [0.000001, 0.1].
///
/// Every component is validated first: its body must be a valid body of @p world, and for an
/// active component, its object must still meet the RigidBody transform and parent rules and a
/// fixed-rotation kinematic object must keep its angle. A failure throws `std::invalid_argument`
/// before any state changes. An inactive component's object is checked only once the component
/// is active again, so a disabled dynamic body's object may meanwhile be parented or scaled.
///
/// The driver then applies each component's active state through Body::set_enabled, teleports
/// stationary bodies whose poses differ from their objects', moves kinematic bodies toward their
/// objects' poses with Body::move_kinematic, and teleports each dynamic body whose object's
/// world matrix, apart from its Z translation, differs from the one at construction or at the
/// last write-back, keeping the body's velocities. It steps @p world once and writes dynamic
/// poses back to their objects. Standalone bodies may share the world.
///
/// Component hooks are not called: run Scene::fixed_update first. Call this once per world per
/// tick instead of World::step. Throws `std::logic_error` while @p scene is running callbacks or a
/// component constructor, or is being destroyed.
void step(Scene &scene, World &world, double seconds);
/// Steps every scene of @p scenes as step(Scene &, World &, double) does, validating all of them
/// before any changes. Also throws `std::logic_error` while @p scenes is busy, as SceneSet describes.
void step(SceneSet &scenes, World &world, double seconds);
/// Registers the `anima.rigid-body-2d.v2` component codec, bound weakly to @p world: the codec
/// does not keep the world alive. Restoring after the world is destroyed, or capturing a component
/// whose body no longer exists, throws `std::out_of_range`. Throws `std::invalid_argument` if
/// @p codecs already has a codec for RigidBody or this key.
///
/// The payload stores the collider, motion, mass, damping, material, layer, flags and current
/// velocities; the scene or prefab transform stores the pose and depth, and the component data
/// stores enablement. It is a JSON object of at most 8,192 bytes with exactly these fields:
/// `shape`, `half_extent`, `radius`, `half_height`, `motion`, `velocity`, `angular_velocity`,
/// `mass`, `linear_damping`, `angular_damping`, `friction`, `restitution`, `layer`, `sensor`,
/// `continuous` and `fixed_rotation`. `half_extent` and `velocity` hold two numbers; `shape`
/// stores the Shape's name, one of `"box"`, `"circle"` and `"capsule"`, and `motion` the
/// Motion's, one of `"stationary"`, `"kinematic"` and `"dynamic"`. Unknown, missing or duplicate
/// fields, other names and invalid values are rejected, and restoring constructs a new RigidBody
/// under its rules. The payload is scene state, not a deterministic solver snapshot.
void add_component_codec(ComponentCodecs &codecs, World &world);
} // namespace anima::physics2d
