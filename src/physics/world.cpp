// Jolt requires its configuration header before every other backend header.
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include "validation.hpp"
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#include <algorithm>
#include <anima/physics.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <mutex>
#include <numbers>
#include <utility>

namespace anima::physics {
namespace {
constexpr float minimum_collider_dimension = .001F;
constexpr float maximum_collider_dimension = 1'000'000.F;
constexpr double minimum_hull_span = .001;
constexpr float hull_construction_tolerance = .0001F;
constexpr std::uint32_t maximum_world_bodies = 65'536;
// A dynamic body's velocity caps, equal to Jolt's defaults. Jolt's clamping setters, impulses and steps shorten a
// longer velocity to its cap, keeping its direction.
constexpr float maximum_dynamic_speed = 500;
constexpr float maximum_dynamic_angular_speed = 15 * std::numbers::pi_v<float>;
// Steps never clamp a kinematic body's velocity, and MoveKinematic sets it without the cap. A kinematic cap
// longer than any validated vector, whose length is at most sqrt(3) * 1,000,000, lets World::create and the
// setters keep every velocity they accept too.
constexpr float maximum_kinematic_speed = 2 * detail::maximum_vector_component;
static_assert(maximum_kinematic_speed * maximum_kinematic_speed >
              3 * detail::maximum_vector_component * detail::maximum_vector_component);
// Jolt is stable at one collision step per 1/60 s and asks for one per started 1/60 s beyond that
// (HelloWorld.cpp). The tolerance keeps a 1/60 s step rounded up to whole nanoseconds, such as
// FixedStepClock's default, at one collision step.
constexpr double collision_steps_per_second = 60;
constexpr double collision_step_tolerance = 1e-6;
static_assert(detail::maximum_damping == collision_steps_per_second,
              "The damping bound assumes 1/60 s collision steps");
// A mesh triangle whose unnormalized normal, the cross product of two edges, has a squared length at or
// below this is degenerate.
constexpr float minimum_squared_triangle_normal = 1e-12F;
constexpr auto accepts_impulses = "Only enabled dynamic bodies accept impulses";
constexpr auto accepts_forces = "Only enabled dynamic bodies accept forces and torques";

void require(bool value, const char *message) {
    if (!value)
        throw std::invalid_argument(message);
}
void vector(Vec3 v) {
    require(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
                std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)}) <= detail::maximum_vector_component,
            "Physics vector outside finite supported range");
}
void duration(double seconds) {
    require(std::isfinite(seconds) && seconds >= detail::minimum_step_seconds &&
                seconds <= detail::maximum_step_seconds,
            "Physics step must be in [0.000001, 0.1] seconds");
}
JPH::Vec3 j(Vec3 v) { return {v.x, v.y, v.z}; }
Vec3 a(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
JPH::Quat j(Quat q) {
    q = unit_quaternion(q);
    return {q[0], q[1], q[2], q[3]};
}
JPH::RMat44 transform(Pose p) {
    vector(p.position);
    return JPH::RMat44::sRotationTranslation(j(p.rotation), j(p.position));
}
struct Runtime {
    Runtime() {
        if (JPH::Factory::sInstance)
            throw std::logic_error("Anima must own Jolt process initialization");
        JPH::RegisterDefaultAllocator();
#ifdef JPH_ENABLE_ASSERTS
        JPH::AssertFailed = [](const char *expression, const char *message, const char *file, JPH::uint line) {
            std::fprintf(stderr, "Jolt assertion %s:%u: %s (%s)\n", file, line, expression, message ? message : "");
            return true;
        };
#endif
        JPH::Factory::sInstance = new JPH::Factory;
        JPH::RegisterTypes();
    }
    ~Runtime() {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
};
std::shared_ptr<Runtime> runtime() {
    static std::mutex mutex;
    static std::weak_ptr<Runtime> instance;
    std::lock_guard lock(mutex);
    auto result = instance.lock();
    if (!result) {
        result = std::make_shared<Runtime>();
        instance = result;
    }
    return result;
}
void hull_points(const std::vector<Vec3> &points) {
    require(points.size() >= 4 && points.size() <= detail::maximum_hull_points, "Hull requires 4..256 points");
    for (const auto point : points)
        vector(point);
    // Require volume, including for stationary/query hulls: Jolt also accepts flat hulls.
    // Double intermediates keep this check meaningful within the public coordinate bound.
    using D = std::array<double, 3>;
    const auto difference = [](Vec3 a, Vec3 b) -> D {
        return {double(a.x) - b.x, double(a.y) - b.y, double(a.z) - b.z};
    };
    const auto dot = [](D a, D b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const auto cross = [](D a, D b) -> D {
        return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    D line{}, normal{};
    double longest = 0, widest = 0, deepest = 0;
    for (const auto point : points) {
        const auto delta = difference(point, points.front());
        if (const auto length = dot(delta, delta); length > longest) {
            longest = length;
            line = delta;
        }
    }
    require(longest >= minimum_hull_span * minimum_hull_span, "Hull points do not span a line");
    for (const auto point : points) {
        const auto candidate = cross(line, difference(point, points.front()));
        if (const auto area = dot(candidate, candidate); area > widest) {
            widest = area;
            normal = candidate;
        }
    }
    require(widest / longest >= minimum_hull_span * minimum_hull_span, "Hull points are collinear or too thin");
    for (const auto point : points)
        deepest = std::max(deepest, std::abs(dot(normal, difference(point, points.front()))));
    require(deepest / std::sqrt(widest) >= minimum_hull_span, "Hull points are coplanar or too thin");
}
JPH::RefConst<JPH::Shape> shape(const Collider &c, bool query = false, bool child = false) {
    JPH::ShapeSettings::ShapeResult result;
    // Validate unused scalar fields too: direct calls and persisted data share one contract.
    for (const float dimension : {c.half_extent.x, c.half_extent.y, c.half_extent.z, c.radius, c.half_height})
        require(std::isfinite(dimension) && dimension >= minimum_collider_dimension &&
                    dimension <= maximum_collider_dimension,
                "Invalid collider dimensions");
    require(c.shape >= Shape::box && c.shape <= Shape::compound, "Unknown collider shape");
    require(!query || c.shape != Shape::mesh, "Query shape cannot be a mesh");
    require(!child || (c.shape != Shape::mesh && c.shape != Shape::compound),
            "Compound children must be primitives or hulls");
    require(c.shape == Shape::compound || c.children.empty(), "Noncompound collider contains children");
    if (c.shape != Shape::mesh && c.shape != Shape::convex_hull)
        require(c.vertices.empty() && c.indices.empty(), "Primitive collider contains mesh data");
    switch (c.shape) {
    case Shape::box:
        result = JPH::BoxShapeSettings(j(c.half_extent), 0).Create();
        break;
    case Shape::sphere:
    case Shape::capsule:
        if (c.shape == Shape::sphere)
            result = JPH::SphereShapeSettings(c.radius).Create();
        else
            result = JPH::CapsuleShapeSettings(c.half_height, c.radius).Create();
        break;
    case Shape::mesh: {
        require(c.vertices.size() >= 3 && c.vertices.size() <= detail::maximum_mesh_vertices && !c.indices.empty() &&
                    c.indices.size() % 3 == 0 && c.indices.size() <= detail::maximum_mesh_indices,
                "Invalid mesh collider size");
        JPH::VertexList vertices;
        for (auto v : c.vertices) {
            vector(v);
            vertices.emplace_back(v.x, v.y, v.z);
        }
        JPH::IndexedTriangleList triangles;
        for (std::size_t i = 0; i < c.indices.size(); i += 3) {
            const auto x = c.indices[i], y = c.indices[i + 1], z = c.indices[i + 2];
            require(x < vertices.size() && y < vertices.size() && z < vertices.size(), "Mesh index out of range");
            const auto area = cross(c.vertices[y] - c.vertices[x], c.vertices[z] - c.vertices[x]);
            require(std::isfinite(dot(area, area)) && dot(area, area) > minimum_squared_triangle_normal,
                    "Degenerate collider triangle");
            triangles.emplace_back(x, y, z);
        }
        result = JPH::MeshShapeSettings(vertices, triangles).Create();
        break;
    }
    case Shape::convex_hull: {
        require(c.indices.empty(), "Hull collider cannot contain triangle indices");
        hull_points(c.vertices);
        JPH::Array<JPH::Vec3> points;
        for (const auto point : c.vertices)
            points.push_back(j(point));
        JPH::ConvexHullShapeSettings settings(points, 0);
        settings.mHullTolerance = hull_construction_tolerance;
        result = settings.Create();
        break;
    }
    case Shape::compound: {
        require(!c.children.empty() && c.children.size() <= detail::maximum_compound_children,
                "Compound requires 1..64 children");
        JPH::StaticCompoundShapeSettings settings;
        for (const auto &part : c.children) {
            vector(part.pose.position);
            const auto rotation = j(part.pose.rotation);
            const auto geometry = shape(part.collider, false, true);
            settings.AddShape(j(part.pose.position), rotation, geometry);
        }
        result = settings.Create();
        break;
    }
    }
    if (result.HasError())
        throw std::invalid_argument(std::string("Invalid collider: ") + result.GetError().c_str());
    if (!result.IsValid())
        throw std::runtime_error("Jolt returned an empty collider result");
    const auto geometry = result.Get();
    if (c.shape == Shape::convex_hull || c.shape == Shape::compound) {
        const auto mass = geometry->GetMassProperties();
        require(std::isfinite(mass.mMass) && mass.mMass > 0, "Collider has invalid mass properties");
        for (unsigned row = 0; row < 3; ++row)
            for (unsigned column = 0; column < 3; ++column)
                require(std::isfinite(mass.mInertia(row, column)), "Collider has invalid inertia");
    }
    return geometry;
}
struct Layers final : JPH::BroadPhaseLayerInterface, JPH::ObjectVsBroadPhaseLayerFilter, JPH::ObjectLayerPairFilter {
    std::array<std::uint16_t, detail::collision_layer_count> masks;
    Layers() { masks.fill(detail::all_collision_layers); }
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return JPH::BroadPhaseLayer(static_cast<JPH::BroadPhaseLayer::Type>(layer / detail::collision_layer_count));
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char *GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer.GetValue() ? "moving" : "stationary";
    }
#endif
    bool ShouldCollide(JPH::ObjectLayer a, JPH::BroadPhaseLayer b) const override {
        return a >= detail::collision_layer_count || b.GetValue() != 0;
    }
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return (a >= detail::collision_layer_count || b >= detail::collision_layer_count) &&
               (masks[a % detail::collision_layer_count] & (1u << (b % detail::collision_layer_count)));
    }
};
// The contact Jolt found between @p body1 and @p body2 before solving it, from velocities that already include the
// collision step's gravity. Jolt's normal points from body 1 toward body 2, so @p reversed flips it for an event
// that names body 2 first; the approach speed is the same either way.
ContactPoint measure(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &m, bool reversed) {
    const auto count = m.mRelativeContactPointsOn1.size(); // Jolt reports at least one point.
    auto sum = JPH::Vec3::sZero();
    float approach = 0;
    for (JPH::uint i = 0; i < count; ++i) {
        const auto offset = .5F * (m.mRelativeContactPointsOn1[i] + m.mRelativeContactPointsOn2[i]);
        const auto point = m.mBaseOffset + offset;
        approach = std::max(approach,
                            (body1.GetPointVelocity(point) - body2.GetPointVelocity(point)).Dot(m.mWorldSpaceNormal));
        sum += offset;
    }
    return {a(m.mBaseOffset + sum / static_cast<float>(count)),
            a(reversed ? -m.mWorldSpaceNormal : m.mWorldSpaceNormal), approach};
}
} // namespace
namespace detail {
struct WorldState final : std::enable_shared_from_this<WorldState>, JPH::ContactListener {
    std::shared_ptr<Runtime> globals = runtime(); // Destroyed after all Jolt objects.
    Layers layers;
    JPH::TempAllocatorMalloc allocator;
    JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
    JPH::PhysicsSystem system;
    struct Entry {
        JPH::BodyID id;
        Motion motion;
        bool enabled = true;
    };
    std::map<std::uint64_t, Entry> entries;
    std::uint64_t next = 1;
    struct Pair {
        std::uint64_t first, second;
        bool sensor;
    };
    std::map<JPH::SubShapeIDPair, Pair> contacts;
    using Key = std::pair<std::uint64_t, std::uint64_t>;
    std::map<Key, bool> reported;
    // The fastest solid sub-shape contact of each body pair recorded during the step, which reconcile() reports on
    // the pair's begin event and then clears.
    std::map<Key, ContactPoint> began;
    std::vector<ContactEvent> events;
    explicit WorldState(WorldSettings settings) {
        vector(settings.gravity);
        require(settings.max_bodies > 0 && settings.max_bodies <= maximum_world_bodies,
                "Invalid physics body capacity");
        system.Init(settings.max_bodies, 0, settings.max_bodies * 4, settings.max_bodies * 4, layers, layers, layers);
        system.SetGravity(j(settings.gravity));
        system.SetContactListener(this);
    }
    ~WorldState() {
        system.SetContactListener(nullptr);
        for (const auto &[key, entry] : entries) {
            (void)key;
            if (entry.enabled)
                system.GetBodyInterface().RemoveBody(entry.id);
            system.GetBodyInterface().DestroyBody(entry.id);
        }
    }
    Body handle(std::uint64_t id) { return Body(weak_from_this(), id); }
    Body handle(JPH::BodyID id) { return handle(system.GetBodyInterface().GetUserData(id)); }
    // The Jolt body of an existing body that accepts forces and impulses, or `std::invalid_argument` with
    // @p message. Disabled bodies are outside Jolt's broadphase, and Jolt's AddImpulse, AddAngularImpulse,
    // AddForce and AddTorque activate a body without checking membership.
    JPH::BodyID accepting(std::uint64_t id, const char *message) const {
        const auto &e = entries.at(id);
        require(e.motion == Motion::dynamic && e.enabled, message);
        return e.id;
    }
    // A persisted contact keeps its entry, whose bodies and sensor flag cannot change, so a solid contact is measured
    // only when it is recorded first, before the solver has resolved it.
    void record(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &m) {
        auto first = body1.GetUserData(), second = body2.GetUserData();
        const bool reversed = first > second;
        if (reversed)
            std::swap(first, second);
        const bool sensor = body1.IsSensor() || body2.IsSensor();
        const JPH::SubShapeIDPair key{body1.GetID(), m.mSubShapeID1, body2.GetID(), m.mSubShapeID2};
        if (!contacts.try_emplace(key, Pair{first, second, sensor}).second || sensor)
            return;
        const auto contact = measure(body1, body2, m, reversed);
        const auto [fastest, inserted] = began.try_emplace({first, second}, contact);
        if (!inserted && contact.approach_speed > fastest->second.approach_speed)
            fastest->second = contact;
    }
    void OnContactAdded(const JPH::Body &a, const JPH::Body &b, const JPH::ContactManifold &m,
                        JPH::ContactSettings &) override {
        record(a, b, m);
    }
    // forget() drops contacts that Jolt still considers continuous, for example when a disabled body is
    // reenabled before the next step; recording persisted contacts restores them and reports a new begin.
    void OnContactPersisted(const JPH::Body &a, const JPH::Body &b, const JPH::ContactManifold &m,
                            JPH::ContactSettings &) override {
        record(a, b, m);
    }
    void OnContactRemoved(const JPH::SubShapeIDPair &pair) override { contacts.erase(pair); }
    void reconcile() {
        const auto measured = std::exchange(began, {});
        std::map<Key, bool> current;
        for (const auto &[key, pair] : contacts) {
            (void)key;
            current[{pair.first, pair.second}] = pair.sensor;
        }
        for (auto [key, sensor] : reported)
            if (!current.contains(key))
                events.push_back({handle(key.first), handle(key.second), ContactPhase::end, sensor, std::nullopt});
        // A body pair begins only through sub-shape contacts recorded during this step, so each solid one was measured.
        for (auto [key, sensor] : current)
            if (!reported.contains(key)) {
                const auto contact = measured.find(key);
                events.push_back({handle(key.first), handle(key.second), ContactPhase::begin, sensor,
                                  contact == measured.end() ? std::optional<ContactPoint>{} : contact->second});
            }
        reported = std::move(current);
    }
    // Contacts change only in Jolt's callbacks during step(), which reconciles right after, so between steps
    // `reported` holds exactly the body pairs of `contacts`, and forgetting a body changes only its own pairs.
    // Ending them in place, in key order as reconcile() would, avoids rebuilding the map of every contact for
    // each removed body. Events are recorded before `reported` changes, so if recording one throws, the
    // next step still ends every pair this left reported.
    void forget(std::uint64_t id) {
        std::erase_if(contacts,
                      [id](const auto &entry) { return entry.second.first == id || entry.second.second == id; });
        const auto names = [id](const Key &key) { return key.first == id || key.second == id; };
        for (auto [key, sensor] : reported)
            if (names(key))
                events.push_back({handle(key.first), handle(key.second), ContactPhase::end, sensor, std::nullopt});
        std::erase_if(reported, [&](const auto &entry) { return names(entry.first); });
    }
};
} // namespace detail
Collider Collider::box(Vec3 half_extent) {
    Collider collider;
    collider.half_extent = half_extent;
    return collider;
}
Collider Collider::sphere(float radius) {
    Collider collider;
    collider.shape = Shape::sphere;
    collider.radius = radius;
    return collider;
}
Collider Collider::capsule(float radius, float half_height) {
    Collider collider;
    collider.shape = Shape::capsule;
    collider.radius = radius;
    collider.half_height = half_height;
    return collider;
}
Collider Collider::convex_hull(std::vector<Vec3> points) {
    Collider collider;
    collider.shape = Shape::convex_hull;
    collider.vertices = std::move(points);
    return collider;
}
Collider Collider::mesh(std::vector<Vec3> vertices, std::vector<std::uint32_t> indices) {
    Collider collider;
    collider.shape = Shape::mesh;
    collider.vertices = std::move(vertices);
    collider.indices = std::move(indices);
    return collider;
}
Collider Collider::compound(std::vector<ColliderChild> children) {
    Collider collider;
    collider.shape = Shape::compound;
    collider.children = std::move(children);
    return collider;
}
std::shared_ptr<detail::WorldState> Body::lock() const {
    auto world = world_.lock();
    if (!world || !world->entries.contains(id_))
        throw std::out_of_range("Expired physics body");
    return world;
}
bool Body::valid() const noexcept {
    auto w = world_.lock();
    return w && w->entries.contains(id_);
}
bool Body::operator==(const Body &other) const noexcept {
    return id_ == other.id_ && !world_.owner_before(other.world_) && !other.world_.owner_before(world_);
}
std::strong_ordering Body::operator<=>(const Body &other) const noexcept {
    if (const auto order = id_ <=> other.id_; order != 0)
        return order;
    if (world_.owner_before(other.world_))
        return std::strong_ordering::less;
    return other.world_.owner_before(world_) ? std::strong_ordering::greater : std::strong_ordering::equal;
}
Pose Body::pose() const {
    auto w = lock();
    const auto id = w->entries.at(id_).id;
    const auto q = w->system.GetBodyInterface().GetRotation(id);
    return {a(w->system.GetBodyInterface().GetPosition(id)), {q.GetX(), q.GetY(), q.GetZ(), q.GetW()}};
}
Vec3 Body::local_center_of_mass() const {
    auto w = lock();
    return a(w->system.GetBodyInterface().GetShape(w->entries.at(id_).id)->GetCenterOfMass());
}
Vec3 Body::world_center_of_mass() const {
    auto w = lock();
    return a(w->system.GetBodyInterface().GetCenterOfMassPosition(w->entries.at(id_).id));
}
Vec3 Body::velocity() const {
    auto w = lock();
    return a(w->system.GetBodyInterface().GetLinearVelocity(w->entries.at(id_).id));
}
Vec3 Body::angular_velocity() const {
    auto w = lock();
    return a(w->system.GetBodyInterface().GetAngularVelocity(w->entries.at(id_).id));
}
float Body::mass() const {
    auto w = lock();
    const auto &e = w->entries.at(id_);
    require(e.motion == Motion::dynamic, "Only dynamic bodies have mass");
    // A disabled body is outside the broadphase but still exists, so the lock succeeds.
    const JPH::BodyLockRead body(w->system.GetBodyLockInterface(), e.id);
    return 1 / body.GetBody().GetMotionProperties()->GetInverseMass();
}
void Body::set_angular_velocity(Vec3 velocity) {
    vector(velocity);
    auto w = lock();
    const auto &e = w->entries.at(id_);
    require(e.motion != Motion::stationary, "Static bodies cannot have angular velocity");
    w->system.GetBodyInterface().SetAngularVelocity(e.id, j(velocity));
}
void Body::teleport(Pose pose) {
    vector(pose.position);
    const auto rotation = j(pose.rotation);
    auto w = lock();
    const auto &e = w->entries.at(id_);
    w->system.GetBodyInterface().SetPositionAndRotation(e.id, j(pose.position), rotation, JPH::EActivation::Activate);
}
void Body::set_velocity(Vec3 velocity) {
    vector(velocity);
    auto w = lock();
    const auto &e = w->entries.at(id_);
    require(e.motion != Motion::stationary, "Static bodies cannot have velocity");
    w->system.GetBodyInterface().SetLinearVelocity(e.id, j(velocity));
}
void Body::add_impulse(Vec3 impulse) {
    vector(impulse);
    auto w = lock();
    w->system.GetBodyInterface().AddImpulse(w->accepting(id_, accepts_impulses), j(impulse));
}
void Body::add_impulse_at(Vec3 impulse, Vec3 world_point) {
    vector(impulse);
    vector(world_point);
    auto w = lock();
    w->system.GetBodyInterface().AddImpulse(w->accepting(id_, accepts_impulses), j(impulse), j(world_point));
}
void Body::add_angular_impulse(Vec3 impulse) {
    vector(impulse);
    auto w = lock();
    w->system.GetBodyInterface().AddAngularImpulse(w->accepting(id_, accepts_impulses), j(impulse));
}
// Jolt accumulates forces and torques on the body. Each collision step of PhysicsSystem::Update applies them for
// its share of the step, and the last one clears them for every active body, which every enabled dynamic body is
// while sleeping is disabled.
void Body::add_force(Vec3 force) {
    vector(force);
    auto w = lock();
    w->system.GetBodyInterface().AddForce(w->accepting(id_, accepts_forces), j(force));
}
void Body::add_force_at(Vec3 force, Vec3 world_point) {
    vector(force);
    vector(world_point);
    auto w = lock();
    w->system.GetBodyInterface().AddForce(w->accepting(id_, accepts_forces), j(force), j(world_point));
}
void Body::add_torque(Vec3 torque) {
    vector(torque);
    auto w = lock();
    w->system.GetBodyInterface().AddTorque(w->accepting(id_, accepts_forces), j(torque));
}
void Body::move_kinematic(Pose target, double seconds) {
    vector(target.position);
    const auto rotation = j(target.rotation);
    duration(seconds);
    auto w = lock();
    const auto &e = w->entries.at(id_);
    require(e.motion == Motion::kinematic, "Body is not kinematic");
    w->system.GetBodyInterface().MoveKinematic(e.id, j(target.position), rotation, static_cast<float>(seconds));
}
bool Body::enabled() const { return lock()->entries.at(id_).enabled; }
void Body::set_enabled(bool enabled) {
    auto w = lock();
    auto &e = w->entries.at(id_);
    if (enabled == e.enabled)
        return;
    auto &bodies = w->system.GetBodyInterface();
    if (enabled) {
        bodies.AddBody(e.id, JPH::EActivation::Activate);
    } else {
        // Removal deactivates the body, which zeroes its velocities. Restoring them afterwards does not
        // reactivate it, because Jolt only activates bodies that are in the broadphase.
        const auto linear = bodies.GetLinearVelocity(e.id);
        const auto angular = bodies.GetAngularVelocity(e.id);
        bodies.RemoveBody(e.id);
        bodies.SetLinearVelocity(e.id, linear);
        bodies.SetAngularVelocity(e.id, angular);
        // Only a step clears accumulated forces, and only on active bodies, so a removed body would keep them
        // until it is reenabled.
        if (e.motion == Motion::dynamic) {
            const JPH::BodyLockWrite body(w->system.GetBodyLockInterface(), e.id);
            body.GetBody().ResetForce();
            body.GetBody().ResetTorque();
        }
        w->forget(id_);
    }
    e.enabled = enabled;
}
void Body::remove() {
    if (!valid())
        return;
    auto w = lock();
    const auto e = w->entries.at(id_);
    if (e.enabled)
        w->system.GetBodyInterface().RemoveBody(e.id);
    w->system.GetBodyInterface().DestroyBody(e.id);
    w->entries.erase(id_);
    w->forget(id_);
}
World::World(WorldSettings settings) : state_(std::make_shared<detail::WorldState>(settings)) {}
World::~World() = default;
std::size_t World::size() const noexcept { return state_->entries.size(); }
bool World::owns(const Body &body) const noexcept { return body.world_.lock() == state_ && body.valid(); }
Body World::create(const BodySettings &s) {
    vector(s.pose.position);
    vector(s.velocity);
    vector(s.angular_velocity);
    const auto rotation = j(s.pose.rotation);
    require(s.motion >= Motion::stationary && s.motion <= Motion::dynamic && s.layer < detail::collision_layer_count,
            "Invalid body motion/layer");
    require(s.collider.shape != Shape::mesh || s.motion == Motion::stationary, "Mesh bodies must be stationary");
    require(std::isfinite(s.mass) && s.mass >= detail::minimum_body_mass && s.mass <= detail::maximum_body_mass &&
                std::isfinite(s.friction) && s.friction >= 0 && s.friction <= 1 && std::isfinite(s.restitution) &&
                s.restitution >= 0 && s.restitution <= 1,
            "Invalid body mass/material");
    require(std::isfinite(s.linear_damping) && s.linear_damping >= 0 && s.linear_damping <= detail::maximum_damping &&
                std::isfinite(s.angular_damping) && s.angular_damping >= 0 &&
                s.angular_damping <= detail::maximum_damping,
            "Invalid body damping");
    require(s.motion != Motion::stationary ||
                (dot(s.velocity, s.velocity) == 0 && dot(s.angular_velocity, s.angular_velocity) == 0),
            "Static body has velocity");
    if (s.center_of_mass) {
        vector(*s.center_of_mass);
        require(s.collider.shape != Shape::mesh, "Triangle meshes have no authored center of mass override");
    }
    auto collider = shape(s.collider);
    if (s.center_of_mass) {
        const auto result =
            JPH::OffsetCenterOfMassShapeSettings(j(*s.center_of_mass) - collider->GetCenterOfMass(), collider).Create();
        if (result.HasError())
            throw std::invalid_argument(std::string("Invalid center of mass: ") + result.GetError().c_str());
        if (!result.IsValid())
            throw std::runtime_error("Jolt returned an empty center of mass result");
        collider = result.Get();
        auto mass = collider->GetMassProperties();
        require(std::isfinite(mass.mMass) && mass.mMass > 0, "Invalid center of mass properties");
        mass.ScaleToMass(s.mass);
        for (unsigned row = 0; row < 3; ++row) {
            require(mass.mInertia(row, row) > 0, "Invalid center of mass inertia");
            for (unsigned column = 0; column < 3; ++column)
                require(std::isfinite(mass.mInertia(row, column)), "Invalid center of mass inertia");
        }
    }
    const auto motion = s.motion == Motion::stationary  ? JPH::EMotionType::Static
                        : s.motion == Motion::kinematic ? JPH::EMotionType::Kinematic
                                                        : JPH::EMotionType::Dynamic;
    JPH::BodyCreationSettings settings(
        collider, j(s.pose.position), rotation, motion,
        static_cast<JPH::ObjectLayer>(s.layer + (s.motion == Motion::stationary ? 0 : detail::collision_layer_count)));
    // Jolt keeps caps only for bodies that move; a stationary body ignores them.
    const bool dynamic = s.motion == Motion::dynamic;
    settings.mMaxLinearVelocity = dynamic ? maximum_dynamic_speed : maximum_kinematic_speed;
    settings.mMaxAngularVelocity = dynamic ? maximum_dynamic_angular_speed : maximum_kinematic_speed;
    // Only kinematic sensors pair with stationary and kinematic bodies, as in Box2D. Jolt reserves this costly
    // pairing for sensors.
    settings.mCollideKinematicVsNonDynamic = s.motion == Motion::kinematic && s.sensor;
    // Jolt damps only dynamic bodies, and would otherwise apply its own default of 0.05 per second.
    settings.mLinearDamping = s.linear_damping;
    settings.mAngularDamping = s.angular_damping;
    settings.mFriction = s.friction;
    settings.mRestitution = s.restitution;
    settings.mIsSensor = s.sensor;
    settings.mAllowSleeping = false; // Stable contact transitions; sleeping policy is a future capability.
    settings.mMotionQuality = s.continuous ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;
    settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    settings.mMassPropertiesOverride.mMass = s.mass;
    require(state_->next != std::numeric_limits<std::uint64_t>::max(), "Body identity exhausted");
    settings.mUserData = state_->next++;
    auto *body = state_->system.GetBodyInterface().CreateBody(settings);
    if (!body)
        throw std::length_error("Physics body capacity exhausted");
    // BodyCreationSettings' velocities must already be within the caps: Jolt asserts that when NDEBUG is unset, as
    // in Debug builds, and keeps a longer one otherwise. The clamping setters apply the caps before the body steps.
    if (s.motion != Motion::stationary) {
        body->SetLinearVelocityClamped(j(s.velocity));
        body->SetAngularVelocityClamped(j(s.angular_velocity));
    }
    const auto id = body->GetID();
    try {
        state_->entries.emplace(settings.mUserData, detail::WorldState::Entry{id, s.motion});
    } catch (...) {
        state_->system.GetBodyInterface().DestroyBody(id);
        throw;
    }
    state_->system.GetBodyInterface().AddBody(id, JPH::EActivation::Activate);
    return state_->handle(settings.mUserData);
}
void World::set_layer_collision(std::uint8_t a, std::uint8_t b, bool collide) {
    require(a < detail::collision_layer_count && b < detail::collision_layer_count, "Physics layer must be in [0,15]");
    if (!state_->entries.empty())
        throw std::logic_error("Configure collision layers before creating bodies");
    const auto set = [collide](std::uint16_t &mask, unsigned bit) {
        mask = static_cast<std::uint16_t>(collide ? mask | (1u << bit) : mask & ~(1u << bit));
    };
    set(state_->layers.masks[a], b);
    set(state_->layers.masks[b], a);
}
bool World::layers_collide(std::uint8_t a, std::uint8_t b) const {
    require(a < detail::collision_layer_count && b < detail::collision_layer_count, "Physics layer must be in [0,15]");
    return (state_->layers.masks[a] & (1u << b)) != 0;
}
void World::step(double seconds) {
    duration(seconds);
    const int collision_steps =
        std::max(1, static_cast<int>(std::ceil(seconds * collision_steps_per_second - collision_step_tolerance)));
    const auto error =
        state_->system.Update(static_cast<float>(seconds), collision_steps, &state_->allocator, &state_->jobs);
    state_->reconcile();
    if (error != JPH::EPhysicsUpdateError::None)
        throw std::runtime_error("Physics update exceeded contact/pair capacity");
}
std::vector<ContactEvent> World::take_events() {
    std::vector<ContactEvent> result;
    result.swap(state_->events);
    return result;
}
namespace {
struct Filter final : JPH::ObjectLayerFilter, JPH::BodyFilter {
    QueryFilter filter;
    JPH::BodyID ignored;
    explicit Filter(QueryFilter f, JPH::BodyID id) : filter(std::move(f)), ignored(id) {}
    bool ShouldCollide(JPH::ObjectLayer layer) const override {
        return (filter.layers & (1u << (layer % detail::collision_layer_count))) != 0;
    }
    bool ShouldCollide(const JPH::BodyID &id) const override { return id != ignored; }
    bool ShouldCollideLocked(const JPH::Body &body) const override { return filter.sensors || !body.IsSensor(); }
};
Hit hit(detail::WorldState &world, const JPH::CollideShapeResult &result, float fraction) {
    return {world.handle(result.mBodyID2),
            fraction,
            a(result.mContactPointOn2),
            a(-result.mPenetrationAxis.NormalizedOr(JPH::Vec3::sAxisY())),
            fraction == 0 ? std::max(0.F, result.mPenetrationDepth) : 0.F,
            fraction == 0};
}
} // namespace
std::optional<Hit> World::raycast(Vec3 origin, Vec3 displacement, QueryFilter filter) const {
    vector(origin);
    vector(displacement);
    require(dot(displacement, displacement) > detail::minimum_squared_displacement, "Zero ray displacement");
    if (filter.ignore.valid() && !owns(filter.ignore))
        throw std::invalid_argument("Foreign query body");
    Filter filters(filter, owns(filter.ignore) ? state_->entries.at(filter.ignore.id_).id : JPH::BodyID{});
    const JPH::RRayCast ray(j(origin), j(displacement));
    JPH::RayCastResult result;
    if (!state_->system.GetNarrowPhaseQuery().CastRay(ray, result, {}, filters, filters))
        return {};
    const auto body = state_->handle(result.mBodyID);
    JPH::BodyLockRead lock(state_->system.GetBodyLockInterface(), result.mBodyID);
    const auto p = ray.GetPointOnRay(result.mFraction);
    auto normal = lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, p);
    // Mesh hits report the triangle's winding normal; face it back along the ray, as sweeps do.
    if (normal.Dot(ray.mDirection) > 0)
        normal = -normal;
    return Hit{body, result.mFraction, a(p), a(normal), 0, result.mFraction == 0};
}
std::optional<Hit> World::sweep(const Collider &collider, Pose start, Vec3 displacement, QueryFilter filter) const {
    vector(displacement);
    require(dot(displacement, displacement) > detail::minimum_squared_displacement,
            "Zero sweep displacement; use overlap");
    if (filter.ignore.valid() && !owns(filter.ignore))
        throw std::invalid_argument("Foreign query body");
    Filter filters(filter, owns(filter.ignore) ? state_->entries.at(filter.ignore.id_).id : JPH::BodyID{});
    const auto query = shape(collider, true);
    const auto pose = transform(start);
    JPH::ShapeCastSettings settings;
    settings.SetBackFaceMode(JPH::EBackFaceMode::CollideWithBackFaces);
    settings.mReturnDeepestPoint = true;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    state_->system.GetNarrowPhaseQuery().CastShape(
        JPH::RShapeCast::sFromWorldTransform(query, JPH::Vec3::sReplicate(1), pose, j(displacement)), settings,
        JPH::RVec3::sZero(), collector, {}, filters, filters);
    if (!collector.HadHit())
        return {};
    return hit(*state_, collector.mHit, collector.mHit.mFraction);
}
std::vector<Hit> World::overlap(const Collider &collider, Pose pose, QueryFilter filter) const {
    if (filter.ignore.valid() && !owns(filter.ignore))
        throw std::invalid_argument("Foreign query body");
    Filter filters(filter, owns(filter.ignore) ? state_->entries.at(filter.ignore.id_).id : JPH::BodyID{});
    const auto query = shape(collider, true);
    const auto placement = transform(pose).PreTranslated(query->GetCenterOfMass());
    JPH::CollideShapeSettings settings;
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    state_->system.GetNarrowPhaseQuery().CollideShape(query, JPH::Vec3::sReplicate(1), placement, settings,
                                                      JPH::RVec3::sZero(), collector, {}, filters, filters);
    std::vector<Hit> result;
    for (const auto &entry : collector.mHits)
        result.push_back(hit(*state_, entry, 0));
    return result;
}
} // namespace anima::physics
