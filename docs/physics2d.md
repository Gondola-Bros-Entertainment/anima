# Headless 2D physics

`ANIMA_BUILD_PHYSICS2D=ON` adds `anima::physics2d`, an optional C++20 API over
[Box2D 3.1.1](https://github.com/erincatto/box2d/releases/tag/v3.1.1). It is off by
default, independently of Jolt. Box2D requires C17; applications consuming this
target should declare `project(MyGame LANGUAGES C CXX)`. Core-only applications
remain C++ only and neither configure nor fetch either physics backend.
`anima::physics2d` requires no assets, SDL, Vulkan, RmlUi or Jolt. With assets
enabled, `anima::physics2d_scene` adds Scene/component and prefab integration.
Backend types, IDs, headers and compile definitions remain private.

```cpp
#include <anima/physics2d.hpp>
namespace p = anima::physics2d;
p::World world;
p::BodySettings floor;
floor.collider.half_extent = {10, .5F};
floor.pose.position = {0, -.5F};
auto ground = world.create(floor);
p::BodySettings falling;
falling.motion = p::Motion::dynamic;
falling.collider.shape = p::Shape::circle;
falling.pose.position = {0, 3};
auto body = world.create(falling);
world.step(1.0 / 60);
const auto position = body.pose().position;
```

## Geometry, simulation and ownership

Coordinates are XY, default gravity points down Y, angles are counterclockwise
radians. Use consistent metres/kilograms/seconds. Each body owns one centered box,
circle or Y-axis capsule. Capsule half-height excludes its round ends. Density
sets dynamic mass from area. Primitive dimensions must be in [0.01, 10000] and
vector/angle inputs within ±1,000,000 and finite. These are input-validation
bounds, **not** a large-world precision or stability guarantee. Box2D's solver
also limits linear/angular speed; a distant kinematic target may not be reached
in one step. Kinematic rotation uses the shortest angular displacement.

All worlds/handles are used on one caller thread; no worker threads, application
callbacks or global Box2D configuration hooks are installed. `step` accepts
[0.000001, 0.1] seconds. Use the core fixed-step accumulator for frame deltas.
A world performs 1–16 substeps (default 4). Sleeping defaults on and can be disabled
at world construction; sleeping bodies retain contact state. This wrapper does
not promise deterministic replay across platforms or serialize solver state.
Box2D has a finite process-wide world capacity; exhaustion throws.

Worlds own bodies. `Body` is a checked non-owning identity; dropping a handle does
not destroy anything. `remove` is idempotent. Removal/world destruction invalidates
all copies even if backend slots are reused. Stale operations throw. Disabled
bodies leave simulation and queries but retain their pose and linear/angular
velocity, which can still be edited. Re-enabling restores that state. Impulses
require an enabled dynamic body. Stationary bodies reject nonzero velocity;
fixed-rotation bodies reject nonzero angular velocity. Kinematic target updates
set both velocities, including zero when the target has stopped.

## Contacts and queries

Configure the symmetric 16-layer matrix before body creation. All pairs initially
collide. Query filters independently select layers, sensors and one ignored body;
a foreign live body is rejected. A stale ignored body has no effect. Sensors are
excluded by default. Queries never require application movement or network rules.

`take_events()` drains body-pair begin/end snapshots. No application code runs
inside Box2D. Removal/disable emits an immediate end for known contacts; the next
step reconciles participation. End handles can be stale and remain comparable.
Solid contacts use buffered transitions, so sleeping does not invent exits.
Sensors reconcile current backend overlaps to handle disable/re-enable between
ticks correctly. Normal contact requires a dynamic participant; sensors can
report static/kinematic overlaps. Sensors are discrete: a fast body can cross a
sensor entirely between ticks. Use casts where continuous detection matters.
`continuous` enables Box2D bullet behavior, not continuous sensor events or a
guarantee for every fast-body pair.

Ray and shape sweep displacements are full segments, with closest-hit fractions
in [0,1] and outward world-space normals. Shapes may be rotated. Initial overlap
is explicitly checked first: it returns fraction zero and `initial_overlap=true`,
with point and normal left zero because no unique surface is selected. When
multiple bodies initially overlap, the selected body is unspecified. Use
`overlap` to obtain all matching body identities; it is a clearance query and
provides no penetration depth/contact manifold. Result order is unspecified.
Zero-length casts are rejected; use `overlap`. Support/slope/landing and movement
policy belong in the application.

## Scene and prefab contract

```cpp
#include <anima/physics2d_scene.hpp>
anima::Scene scene;
auto object = scene.create("falling token");
object.set_position({0, 3, 7}); // Z is presentation depth, not simulation.
object.add_component<p::RigidBody>(world, falling);
anima::ComponentCodecs codecs;
p::add_component_codec(codecs, world);
scene.fixed_update(1.0 / 60);
p::step(scene, world, 1.0 / 60);
```

A `RigidBody` component owns one body's lifetime, derives its initial pose from
the object's world transform, and ignores `BodySettings::pose`. Transforms require
unit scale, XY motion and Z-axis rotation with no tilt, shear or reflection.
Dynamic bodies must be roots; stationary/kinematic objects may have planar rigid
parents. Dimensions belong in the collider. Dynamic synchronization preserves the
object's Z presentation depth.

The `step` driver accepts a Scene or SceneSet and never calls component hooks.
After the caller runs fixed callbacks once, it validates the full body snapshot
across the selection before moving/enabling bodies, applies static/kinematic poses,
steps the world, then copies dynamic poses back to objects. Removal during a
component callback is safe; bodies are collected after callbacks. Teleport
dynamic objects via `Body::teleport`. A disabled dynamic component resumes its
saved physics pose when enabled. Destruction of the component or scene removes
its body. Destroying a world first is safe; subsequent simulation/restoration
rejects the expired binding. Driver entry during selected-scene callbacks or
construction rejects.

Every selected 2D component must belong to the supplied world, including inactive
ones. Call the driver once per world per tick; do not also call `World::step`.
Standalone bodies can share it. Run `Scene::fixed_update` or `SceneSet::fixed_update`
once before stepping the optional 2D and 3D worlds. Use distinct objects for each
backend to avoid competing transform writes. See
[runtime coordination](scene-objects.md#additive-ownership-and-explicit-transitions) for ordering and
failure boundaries.

The strict `anima.rigid-body-2d.v1` codec stores geometry, motion, density/material,
layers/flags and current linear/angular velocity. Object transforms store pose
and depth; component data stores enablement. Restoration creates independent
bodies, rejects unknown/malformed fields, and participates in prefab rollback.
The codec holds a weak destination-world binding. It never stores backend IDs.
It is an authored scene state format, not a simulation checkpoint.

## Qualification and scope

```sh
cmake --preset headless -DANIMA_BUILD_PHYSICS=ON -DANIMA_BUILD_PHYSICS2D=ON
cmake --build --preset headless
ctest --preset headless --output-on-failure
```

`physics2d_world` covers falling/sleep/wake/contact continuity, shape queries,
initial overlap, filtering, sensors, disable/re-enable, stale slot reuse, capacity,
kinematic stopping, continuous collision and invalid inputs. `physics2d_scene`
covers callbacks, synchronization, retained depth, teardown, invalid-transform
atomic validation and prefab restoration/rollback. `consumer_physics2d` and
`consumer_physics2d_scene` copy only application files and build public targets;
other consumers reject accidental backend imports. Qualify on Linux, macOS and
Windows using the [local workflow](local-qualification.md). Sanitizers are
separate local checks.

Colliders are centered boxes, circles or capsules. Joints, compound colliders,
polygons/chains and character controllers are not exposed by this API.
