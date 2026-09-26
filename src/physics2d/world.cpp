#include "limits.hpp"
#include <algorithm>
#include <anima/physics2d.hpp>
#include <array>
#include <box2d/box2d.h>
#include <cmath>
#include <exception>
#include <map>
#include <stdexcept>

#include <numbers>

namespace anima::physics2d {
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::invalid_argument(message);
}
void scalar(float value, float bound = 1e6F) {
    require(std::isfinite(value) && std::abs(value) <= bound, "2D physics value outside finite supported range");
}
void vector(Vec2 v) {
    scalar(v.x);
    scalar(v.y);
}
void duration(double seconds) {
    require(std::isfinite(seconds) && seconds >= 1e-6 && seconds <= .1, "2D physics step outside [0.000001, 0.1]");
}
b2Vec2 b(Vec2 v) { return {v.x, v.y}; }
Vec2 a(b2Vec2 v) { return {v.x, v.y}; }
void validate_position(Vec2 v) {
    for (const float component : {v.x, v.y})
        require(std::isfinite(component) && std::abs(component) <= detail::maximum_position,
                "2D physics position outside supported range");
}
b2Transform transform(Pose p) {
    validate_position(p.position);
    scalar(p.angle);
    // b2MakeRot approximates cosine and sine to within about 1.6e-3 radians. Exact values keep the angle a
    // caller set, which fixed-rotation checks compare against.
    return {b(p.position), b2Rot{std::cos(p.angle), std::sin(p.angle)}};
}
void validate(const Collider &c) {
    require(c.shape >= Shape::box && c.shape <= Shape::capsule, "Unknown 2D collider shape");
    for (float v : {c.half_extent.x, c.half_extent.y, c.radius, c.half_height})
        require(std::isfinite(v) && v >= detail::minimum_collider_dimension && v <= detail::maximum_collider_dimension,
                "2D collider dimensions outside [0.01, 10000]");
}
b2ShapeProxy proxy(const Collider &c, Pose p) {
    validate(c);
    const auto t = transform(p);
    if (c.shape == Shape::box) {
        const auto box = b2MakeBox(c.half_extent.x, c.half_extent.y);
        return b2MakeOffsetProxy(box.vertices, box.count, 0, t.p, t.q);
    }
    if (c.shape == Shape::circle) {
        const b2Vec2 center{};
        return b2MakeOffsetProxy(&center, 1, c.radius, t.p, t.q);
    }
    const b2Vec2 points[] = {{0, -c.half_height}, {0, c.half_height}};
    return b2MakeOffsetProxy(points, 2, c.radius, t.p, t.q);
}
// Reserve a category for queries so simulation collision masks never suppress an
// explicitly requested layer. No backend callbacks into application code.
constexpr std::uint64_t query_category = std::uint64_t{1} << 16;
} // namespace
namespace detail {
struct WorldState : std::enable_shared_from_this<WorldState> {
    struct Entry {
        b2BodyId body;
        b2ShapeId shape;
        Motion motion;
        std::uint8_t layer;
        bool sensor, fixed_rotation, enabled = true;
        Vec2 disabled_velocity{};
        float disabled_angular_velocity{};
    };
    using Pair = std::pair<std::uint64_t, std::uint64_t>;
    b2WorldId world{};
    WorldSettings settings;
    std::uint64_t next = 1;
    std::map<std::uint64_t, Entry> entries;
    std::map<std::uint64_t, std::uint64_t> shapes;
    std::array<std::uint16_t, 16> masks;
    std::map<Pair, bool> touching;
    std::vector<ContactEvent> events;
    explicit WorldState(WorldSettings s) : settings(s) {
        vector(s.gravity);
        require(s.max_bodies > 0 && s.max_bodies <= 1000000 && s.substeps >= 1 && s.substeps <= 16,
                "Invalid 2D world capacity/substeps");
        masks.fill(0xffff);
        auto def = b2DefaultWorldDef();
        def.gravity = b(s.gravity);
        def.enableSleep = s.sleeping;
        world = b2CreateWorld(&def);
        if (B2_IS_NULL(world))
            throw std::length_error("Box2D world capacity exhausted");
    }
    ~WorldState() { b2DestroyWorld(world); }
    Body handle(std::uint64_t id) { return Body(weak_from_this(), id); }
    std::uint64_t identity(b2ShapeId id) const {
        const auto found = shapes.find(b2StoreShapeId(id));
        return found == shapes.end() ? 0 : found->second;
    }
    void forget(std::uint64_t id) {
        for (auto it = touching.begin(); it != touching.end();) {
            if (it->first.first == id || it->first.second == id) {
                events.push_back({handle(it->first.first), handle(it->first.second), ContactPhase::end, it->second});
                it = touching.erase(it);
            } else
                ++it;
        }
    }
    void reconcile() {
        auto current = touching;
        const auto apply = [&](b2ShapeId a, b2ShapeId b, bool begin, bool sensor) {
            const auto x = identity(a), y = identity(b);
            if (!x || !y)
                return; // End events can contain destroyed backend identities.
            const Pair pair = std::minmax(x, y);
            if (begin && entries.at(x).enabled && entries.at(y).enabled)
                current[pair] = sensor;
            else
                current.erase(pair);
        };
        const auto contact = b2World_GetContactEvents(world);
        for (int i = 0; i < contact.endCount; ++i)
            apply(contact.endEvents[i].shapeIdA, contact.endEvents[i].shapeIdB, false, false);
        for (int i = 0; i < contact.beginCount; ++i)
            apply(contact.beginEvents[i].shapeIdA, contact.beginEvents[i].shapeIdB, true, false);
        // Box2D retains sensor overlap history across a disable/re-enable between
        // steps, so a buffered begin event alone can miss re-entry after our
        // immediate disable/end notification. Reconcile sensors from the current
        // overlap snapshot; retained solid contacts still use buffered events.
        std::erase_if(current, [](const auto &pair) { return pair.second; });
        std::vector<b2ShapeId> overlaps;
        for (const auto &[id, entry] : entries) {
            (void)id;
            if (!entry.enabled || !entry.sensor)
                continue;
            overlaps.resize(static_cast<std::size_t>(b2Shape_GetSensorCapacity(entry.shape)));
            const int count =
                b2Shape_GetSensorOverlaps(entry.shape, overlaps.data(), static_cast<int>(overlaps.size()));
            for (int i = 0; i < count; ++i) {
                const auto other = overlaps[static_cast<std::size_t>(i)];
                // Box2D reports every overlap, but the 3D module pairs two bodies only when one of them can
                // move, so two stationary bodies never report a sensor overlap in either module.
                const auto found = entries.find(identity(other));
                if (entry.motion == Motion::stationary && found != entries.end() &&
                    found->second.motion == Motion::stationary)
                    continue;
                apply(entry.shape, other, true, true);
            }
        }
        for (const auto &[pair, is_sensor] : touching)
            if (!current.contains(pair))
                events.push_back({handle(pair.first), handle(pair.second), ContactPhase::end, is_sensor});
        for (const auto &[pair, is_sensor] : current)
            if (!touching.contains(pair))
                events.push_back({handle(pair.first), handle(pair.second), ContactPhase::begin, is_sensor});
        touching.swap(current);
    }
};
} // namespace detail
bool Body::valid() const noexcept {
    auto world = world_.lock();
    return world && world->entries.contains(id_);
}
std::shared_ptr<detail::WorldState> Body::lock() const {
    auto world = world_.lock();
    if (!world || !world->entries.contains(id_))
        throw std::out_of_range("Expired 2D physics body");
    return world;
}
bool Body::operator==(const Body &other) const noexcept {
    return id_ == other.id_ && !world_.owner_before(other.world_) && !other.world_.owner_before(world_);
}
Pose Body::pose() const {
    auto world = lock();
    const auto t = b2Body_GetTransform(world->entries.at(id_).body);
    return {a(t.p), std::atan2(t.q.s, t.q.c)};
}
Vec2 Body::velocity() const {
    auto world = lock();
    const auto &e = world->entries.at(id_);
    return e.enabled ? a(b2Body_GetLinearVelocity(e.body)) : e.disabled_velocity;
}
float Body::angular_velocity() const {
    auto world = lock();
    const auto &e = world->entries.at(id_);
    return e.enabled ? b2Body_GetAngularVelocity(e.body) : e.disabled_angular_velocity;
}
bool Body::enabled() const { return lock()->entries.at(id_).enabled; }
bool Body::awake() const { return b2Body_IsAwake(lock()->entries.at(id_).body); }
void Body::teleport(Pose p) {
    const auto t = transform(p);
    const auto id = lock()->entries.at(id_).body;
    b2Body_SetTransform(id, t.p, t.q);
    b2Body_SetAwake(id, true);
}
void Body::set_velocity(Vec2 v) {
    vector(v);
    auto world = lock();
    auto &e = world->entries.at(id_);
    require(e.motion != Motion::stationary, "Static 2D bodies cannot have velocity");
    if (e.enabled)
        b2Body_SetLinearVelocity(e.body, b(v));
    else
        e.disabled_velocity = v;
}
void Body::set_angular_velocity(float v) {
    scalar(v);
    auto world = lock();
    auto &e = world->entries.at(id_);
    require(e.motion != Motion::stationary && (!e.fixed_rotation || v == 0), "2D body cannot rotate");
    if (e.enabled)
        b2Body_SetAngularVelocity(e.body, v);
    else
        e.disabled_angular_velocity = v;
}
void Body::add_impulse(Vec2 v) {
    vector(v);
    auto world = lock();
    const auto &e = world->entries.at(id_);
    require(e.motion == Motion::dynamic && e.enabled, "Only enabled dynamic 2D bodies accept impulses");
    b2Body_ApplyLinearImpulseToCenter(e.body, b(v), true);
}
void Body::move_kinematic(Pose p, double seconds) {
    const auto t = transform(p);
    duration(seconds);
    auto world = lock();
    const auto &e = world->entries.at(id_);
    require(e.motion == Motion::kinematic, "2D body is not kinematic");
    const auto old = b2Body_GetTransform(e.body);
    const float angle = std::remainder(p.angle - std::atan2(old.q.s, old.q.c), 2 * std::numbers::pi_v<float>);
    require(!e.fixed_rotation || std::abs(angle) < 1e-6F,
            "Fixed-rotation 2D body cannot change angle through velocity");
    // Set both velocities explicitly, including a stationary target. Box2D's
    // target helper ignores small target deltas and can retain an old velocity.
    auto &entry = world->entries.at(id_);
    const float inverse = 1 / static_cast<float>(seconds);
    const Vec2 velocity{(t.p.x - old.p.x) * inverse, (t.p.y - old.p.y) * inverse};
    const float angular = e.fixed_rotation ? 0 : angle * inverse;
    if (entry.enabled) {
        b2Body_SetLinearVelocity(entry.body, b(velocity));
        b2Body_SetAngularVelocity(entry.body, angular);
    } else {
        entry.disabled_velocity = velocity;
        entry.disabled_angular_velocity = angular;
    }
}
void Body::set_enabled(bool enabled) {
    auto world = lock();
    auto &e = world->entries.at(id_);
    if (e.enabled == enabled)
        return;
    if (enabled) {
        b2Body_Enable(e.body);
        if (e.motion != Motion::stationary) {
            b2Body_SetLinearVelocity(e.body, b(e.disabled_velocity));
            b2Body_SetAngularVelocity(e.body, e.disabled_angular_velocity);
        }
    } else {
        world->forget(id_);
        e.disabled_velocity = a(b2Body_GetLinearVelocity(e.body));
        e.disabled_angular_velocity = b2Body_GetAngularVelocity(e.body);
        b2Body_Disable(e.body);
    }
    e.enabled = enabled;
}
void Body::remove() {
    if (!valid())
        return;
    auto world = lock();
    const auto e = world->entries.at(id_);
    world->forget(id_);
    world->shapes.erase(b2StoreShapeId(e.shape));
    world->entries.erase(id_);
    b2DestroyBody(e.body);
}
World::World(WorldSettings s) : state_(std::make_shared<detail::WorldState>(s)) {}
World::~World() = default;
std::size_t World::size() const noexcept { return state_->entries.size(); }
bool World::owns(const Body &body) const noexcept { return body.world_.lock() == state_ && body.valid(); }
Body World::create(const BodySettings &s) {
    validate(s.collider);
    const auto pose = transform(s.pose);
    vector(s.velocity);
    scalar(s.angular_velocity);
    require(s.motion >= Motion::stationary && s.motion <= Motion::dynamic && s.layer < 16,
            "Invalid 2D body motion/layer");
    require(std::isfinite(s.density) && s.density >= .001F && s.density <= 10000 && std::isfinite(s.friction) &&
                s.friction >= 0 && s.friction <= 1 && std::isfinite(s.restitution) && s.restitution >= 0 &&
                s.restitution <= 1,
            "Invalid 2D body material/density");
    require(s.motion != Motion::stationary || (s.velocity.x == 0 && s.velocity.y == 0 && s.angular_velocity == 0),
            "Static 2D body has velocity");
    require(!s.fixed_rotation || s.angular_velocity == 0, "Fixed-rotation 2D body has angular velocity");
    if (size() >= state_->settings.max_bodies || state_->next == UINT64_MAX)
        throw std::length_error("2D physics body capacity exhausted");
    auto def = b2DefaultBodyDef();
    def.type = s.motion == Motion::stationary  ? b2_staticBody
               : s.motion == Motion::kinematic ? b2_kinematicBody
                                               : b2_dynamicBody;
    def.position = pose.p;
    def.rotation = pose.q;
    def.linearVelocity = b(s.velocity);
    def.angularVelocity = s.angular_velocity;
    def.isBullet = s.continuous;
    def.fixedRotation = s.fixed_rotation;
    const auto body = b2CreateBody(state_->world, &def);
    b2ShapeId shape{};
    const auto id = state_->next++;
    try {
        auto shape_def = b2DefaultShapeDef();
        shape_def.density = s.density;
        shape_def.material.friction = s.friction;
        shape_def.material.restitution = s.restitution;
        shape_def.filter.categoryBits = std::uint64_t{1} << s.layer;
        shape_def.filter.maskBits = state_->masks[s.layer] | query_category;
        shape_def.isSensor = s.sensor;
        shape_def.enableSensorEvents = true;
        shape_def.enableContactEvents = true;
        if (s.collider.shape == Shape::box) {
            const auto box = b2MakeBox(s.collider.half_extent.x, s.collider.half_extent.y);
            shape = b2CreatePolygonShape(body, &shape_def, &box);
        } else if (s.collider.shape == Shape::circle) {
            const b2Circle circle{{}, s.collider.radius};
            shape = b2CreateCircleShape(body, &shape_def, &circle);
        } else {
            const b2Capsule capsule{{0, -s.collider.half_height}, {0, s.collider.half_height}, s.collider.radius};
            shape = b2CreateCapsuleShape(body, &shape_def, &capsule);
        }
        state_->entries.emplace(id,
                                detail::WorldState::Entry{body, shape, s.motion, s.layer, s.sensor, s.fixed_rotation});
        state_->shapes.emplace(b2StoreShapeId(shape), id);
    } catch (...) {
        state_->entries.erase(id);
        b2DestroyBody(body);
        throw;
    }
    return state_->handle(id);
}
void World::set_layer_collision(std::uint8_t a, std::uint8_t b, bool collide) {
    require(a < 16 && b < 16, "2D physics layer must be in [0,15]");
    if (!state_->entries.empty())
        throw std::logic_error("Configure 2D layers before creating bodies");
    const auto set = [collide](std::uint16_t &mask, unsigned bit) {
        mask = static_cast<std::uint16_t>(collide ? mask | (1u << bit) : mask & ~(1u << bit));
    };
    set(state_->masks[a], b);
    set(state_->masks[b], a);
}
void World::step(double seconds) {
    duration(seconds);
    b2World_Step(state_->world, static_cast<float>(seconds), static_cast<int>(state_->settings.substeps));
    state_->reconcile();
}
std::vector<ContactEvent> World::take_events() {
    std::vector<ContactEvent> events;
    events.swap(state_->events);
    return events;
}
namespace {
struct Query {
    detail::WorldState &world;
    QueryFilter filter;
    std::optional<Hit> hit;
    std::vector<Body> overlaps;
    std::exception_ptr error;
    explicit Query(detail::WorldState &w, QueryFilter f) : world(w), filter(std::move(f)) {}
    b2QueryFilter native_filter() const { return {query_category, filter.layers}; }
    Body accept(b2ShapeId shape) {
        const auto id = world.identity(shape);
        if (!id)
            return {};
        const auto &e = world.entries.at(id);
        auto body = world.handle(id);
        return !e.enabled || (!filter.sensors && e.sensor) || body == filter.ignore ? Body{} : body;
    }
    static float cast(b2ShapeId shape, b2Vec2 point, b2Vec2 normal, float fraction, void *context) noexcept {
        auto &q = *static_cast<Query *>(context);
        try {
            auto body = q.accept(shape);
            if (!body.valid())
                return -1;
            if (!q.hit || fraction < q.hit->fraction)
                q.hit = Hit{body, fraction, a(point), a(normal), false};
            return q.hit->fraction;
        } catch (...) {
            q.error = std::current_exception();
            return 0;
        }
    }
    static bool overlap(b2ShapeId shape, void *context) noexcept {
        auto &q = *static_cast<Query *>(context);
        try {
            auto body = q.accept(shape);
            if (body.valid())
                q.overlaps.push_back(body);
            return true;
        } catch (...) {
            q.error = std::current_exception();
            return false;
        }
    }
    void check() const {
        if (error)
            std::rethrow_exception(error);
    }
    bool initial(const b2ShapeProxy &shape) {
        b2World_OverlapShape(world.world, &shape, native_filter(), overlap, this);
        check();
        if (overlaps.empty())
            return false;
        hit = Hit{overlaps.front(), 0, {}, {}, true};
        return true;
    }
};
void displacement(Vec2 v) {
    vector(v);
    require(v.x * v.x + v.y * v.y > 1e-12F, "Zero 2D cast displacement; use overlap");
}
} // namespace
std::optional<Hit> World::raycast(Vec2 origin, Vec2 delta, QueryFilter filter) const {
    vector(origin);
    displacement(delta);
    if (filter.ignore.valid() && !owns(filter.ignore))
        throw std::invalid_argument("Foreign 2D query body");
    Query q(*state_, std::move(filter));
    const auto point = b(origin);
    const auto shape = b2MakeProxy(&point, 1, 0);
    if (!q.initial(shape))
        b2World_CastRay(state_->world, point, b(delta), q.native_filter(), Query::cast, &q);
    q.check();
    return q.hit;
}
std::optional<Hit> World::sweep(const Collider &collider, Pose start, Vec2 delta, QueryFilter filter) const {
    const auto shape = proxy(collider, start);
    displacement(delta);
    if (filter.ignore.valid() && !owns(filter.ignore))
        throw std::invalid_argument("Foreign 2D query body");
    Query q(*state_, std::move(filter));
    if (!q.initial(shape))
        b2World_CastShape(state_->world, &shape, b(delta), q.native_filter(), Query::cast, &q);
    q.check();
    return q.hit;
}
std::vector<Body> World::overlap(const Collider &collider, Pose pose, QueryFilter filter) const {
    const auto shape = proxy(collider, pose);
    if (filter.ignore.valid() && !owns(filter.ignore))
        throw std::invalid_argument("Foreign 2D query body");
    Query q(*state_, std::move(filter));
    b2World_OverlapShape(state_->world, &shape, q.native_filter(), Query::overlap, &q);
    q.check();
    return std::move(q.overlaps);
}
} // namespace anima::physics2d
