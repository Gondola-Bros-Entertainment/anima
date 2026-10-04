#pragma once
#include <anima/physics.hpp>
#include <anima/scene.hpp>

/// @file
/// Scene and prefab integration for anima::physics: a RigidBody component, a fixed-step driver
/// and a persistence codec. Part of the optional `anima::physics_scene` target.

namespace anima::physics {
namespace detail {
struct RigidBodyAccess;
}
/// Component that owns one body for its GameObject.
///
/// The body starts at the object's world pose; BodySettings::pose is ignored. That transform
/// must be rigid (unit scale, no shear or reflection), and dynamic bodies must be scene roots;
/// stationary and kinematic bodies may have rigid parents. Size belongs in the Collider, not in
/// object scale. Destroying the component removes its body, including after the world is gone.
///
/// The world reflects the scene as of the last step(): the driver applies the component's active
/// state and its object's pose only when it runs. Until then, the body of a component that was
/// disabled, deactivated or restored disabled still takes part in queries and contacts.
class RigidBody {
  public:
    /// Creates the body in @p world at the world pose of @p owner's object. Throws
    /// `std::invalid_argument` for a non-rigid transform, a dynamic child object or invalid settings
    /// (see World::create).
    RigidBody(ComponentOwner owner, World &world, BodySettings settings = {});
    /// Removes the body, as Body::remove does, which ends its contacts; nothing remains to remove once its world is
    /// destroyed.
    ~RigidBody();
    RigidBody(const RigidBody &) = delete;
    RigidBody &operator=(const RigidBody &) = delete;
    /// Handle to the owned body, for velocities, impulses and teleports. Each step() moves a
    /// stationary or kinematic body to its object's pose, so a teleport or velocity written to
    /// either lasts only until then; move those bodies through their objects' transforms. A
    /// dynamic body keeps a teleport unless its object's transform is also edited before the
    /// next step(), which then teleports the body to the object.
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
/// Every component is validated first: each must belong to @p world, and the object of each
/// active one must still meet the RigidBody transform and parent rules. A failure throws
/// `std::invalid_argument` before any state changes. An inactive component's object is checked
/// only once the component is active again, so a disabled dynamic body's object may meanwhile be
/// parented or scaled.
///
/// The driver then applies each component's active state through Body::set_enabled, teleports
/// stationary bodies and moves kinematic bodies with Body::move_kinematic to their objects'
/// poses, and teleports each dynamic body whose object's world matrix differs from the one at
/// construction or at the last write-back, keeping the body's velocities. It steps @p world once
/// and writes dynamic poses back to their objects. Standalone bodies may share the world.
///
/// Component hooks are not called: run Scene::fixed_update first. Call this once per world per
/// tick instead of World::step. Throws `std::logic_error` while the scene is updating, under
/// construction or destroyed. Throws `std::runtime_error` as World::step does when contacts
/// exceed capacity: the active states, teleports and kinematic moves have then been applied and
/// the world partly advanced, but no dynamic pose is written back, so an edited dynamic object
/// moves its body again at the next step.
void step(Scene &scene, World &world, double seconds);
/// Steps every scene of @p scenes as step(Scene &, World &, double) does, validating all of
/// them before any changes. Also throws `std::logic_error` while the set is busy, as SceneSet describes.
void step(SceneSet &scenes, World &world, double seconds);
/// The RigidBody of @p scene that owns @p body, such as a ContactEvent's or a Hit's, or an invalid
/// handle when there is none: for a standalone body, another scene's body or a default-constructed
/// Body, and once the owning component is destroyed, as it is for the end events its destruction
/// records. It compares identities without reading body state, so it still finds the owner of a
/// body removed through Body::remove(), and it includes disabled components and those on inactive
/// objects.
///
/// It scans what Scene::components returns for RigidBody, so it costs that query plus time linear
/// in the scene's RigidBody components. To look up many bodies, such as every event of a step, key
/// an `std::unordered_map` by RigidBody::body() once instead.
[[nodiscard]] ComponentRef<RigidBody> find_rigid_body(Scene &scene, const Body &body);
/// Searches every member of @p scenes as find_rigid_body(Scene &, const Body &) searches one scene.
[[nodiscard]] ComponentRef<RigidBody> find_rigid_body(SceneSet &scenes, const Body &body);
/// Registers the `anima.rigid-body.v3` component codec, bound weakly to @p world: the codec does
/// not keep the world alive. Restoring after the world is destroyed, or capturing a component
/// whose body no longer exists, throws `std::out_of_range`. Throws `std::invalid_argument` if
/// @p codecs already has a codec for RigidBody or this key.
///
/// The payload stores collider geometry, motion, mass, damping, material, layer and flags, the
/// optional local center of mass and current velocities; the scene or prefab transform stores the
/// pose. It is a JSON object of at most 16 MiB with exactly these fields: `shape`,
/// `half_extent`, `radius`, `half_height`, `vertices`, `indices`, `children`, `center_of_mass`
/// (null or three numbers), `motion`, `velocity`, `angular_velocity`, `mass`, `linear_damping`,
/// `angular_damping`, `friction`, `restitution`, `layer`, `sensor` and `continuous`. Each child
/// has exactly `position`, `rotation` (four XYZW numbers), `shape`, `half_extent`, `radius`,
/// `half_height` and `vertices`. `shape` stores the Shape's name, one of `"box"`, `"sphere"`,
/// `"capsule"`, `"mesh"`, `"convex_hull"` and `"compound"`, and `motion` the Motion's, one of
/// `"stationary"`, `"kinematic"` and `"dynamic"`. Unknown, missing or duplicate fields, other
/// names and invalid geometry are rejected. The payload is scene state, not a deterministic
/// solver snapshot.
///
/// Capturing a payload over 16 MiB throws `std::invalid_argument`. Vertices and indices are
/// stored as text, so a mesh collider of a few hundred thousand vertices can reach that, and a
/// scene document is itself limited to 16 MiB.
void add_component_codec(ComponentCodecs &codecs, World &world);
} // namespace anima::physics
