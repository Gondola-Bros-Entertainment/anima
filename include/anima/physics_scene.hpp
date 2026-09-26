#pragma once
#include <anima/physics.hpp>
#include <anima/scene.hpp>

/// @file
/// Scene and prefab integration for anima::physics: a RigidBody component, a fixed-step driver
/// and a persistence codec. Part of the optional `anima::physics_scene` target.

namespace anima::physics {
/// Component that owns one body for its GameObject.
///
/// The body starts at the object's world pose; BodySettings::pose is ignored. That transform
/// must be rigid (unit scale, no shear or reflection), and dynamic bodies must be scene roots;
/// stationary and kinematic bodies may have rigid parents. Size belongs in the Collider, not in
/// object scale. Destroying the component removes its body, including after the world is gone.
///
/// The world reflects the scene as of the last step(): the driver applies the component's active
/// state and the poses of moved stationary and kinematic objects only when it runs. Until then,
/// the body of a component that was disabled, deactivated or restored disabled still takes part
/// in queries and contacts.
class RigidBody {
  public:
    /// Creates the body in @p world. Throws `std::invalid_argument` for a non-rigid transform, a
    /// dynamic child object or invalid settings (see World::create).
    RigidBody(GameObject object, World &world, BodySettings settings = {});
    ~RigidBody();
    RigidBody(const RigidBody &) = delete;
    RigidBody &operator=(const RigidBody &) = delete;
    /// Handle to the owned body, for velocities, impulses and teleports.
    [[nodiscard]] Body body() const { return body_; }
    /// Creation settings, with BodySettings::pose set to the object's starting pose.
    [[nodiscard]] const BodySettings &settings() const { return settings_; }

  private:
    BodySettings settings_;
    Body body_;
};
/// Advances every RigidBody in @p scene by one fixed step of @p seconds, in [0.000001, 0.1].
///
/// Every component is validated first, including inactive ones: each must belong to @p world,
/// or `std::invalid_argument` is thrown before any state changes. The driver then applies each
/// component's active state through Body::set_enabled, moves stationary and kinematic bodies
/// to their objects' poses, steps @p world once and writes dynamic poses back to their objects.
/// Standalone bodies may share the world.
///
/// Component hooks are not called: run Scene::fixed_update first. Call this once per world per
/// tick instead of World::step. Throws `std::logic_error` while the scene is updating, under
/// construction or destroyed.
void step(Scene &scene, World &world, double seconds);
/// Steps every scene of @p scenes as step(Scene &, World &, double) does, validating all of
/// them before any changes. Also throws `std::logic_error` while the set is changing.
void step(SceneSet &scenes, World &world, double seconds);
/// Registers the `anima.rigid-body.v2` component codec, bound weakly to @p world: the codec does
/// not keep the world alive, and restoring after the world is destroyed throws
/// `std::out_of_range`.
///
/// The payload stores collider geometry, motion, material, layer and flags, the optional local
/// center of mass and current velocities; the scene or prefab transform stores the pose. It is
/// a JSON object of at most 16 MiB with exactly these fields: `shape`, `extent`, `radius`,
/// `half_height`, `vertices`, `indices`, `children`, `center_of_mass` (null or three numbers),
/// `motion`, `velocity`, `angular_velocity`, `mass`, `friction`, `restitution`, `layer`,
/// `sensor` and `continuous`. Each child has exactly `position`, `rotation` (four XYZW
/// numbers), `shape`, `extent`, `radius`, `half_height` and `vertices`. `shape` and `motion`
/// store the Shape and Motion enumerator values. Unknown, missing or duplicate fields and
/// invalid geometry are rejected. The payload is scene state, not a deterministic solver
/// snapshot.
void add_component_codec(ComponentCodecs &codecs, World &world);
} // namespace anima::physics
