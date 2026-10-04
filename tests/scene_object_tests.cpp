#include "consumer/scene_objects.hpp"
#include <anima/camera.hpp>
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
using namespace anima;
constexpr auto parenting_cycle = "GameObject parenting would create a cycle";

// Runtime ids of @p objects, in order.
std::vector<Scene::Id> ids_of(const std::vector<GameObject> &objects) {
    std::vector<Scene::Id> result;
    for (const auto &object : objects)
        result.push_back(object.id());
    return result;
}

struct CleanupState {
    Scene *scene{};
    std::vector<GameObject> retired, created;
    bool failed{};
};
struct CreateOnDisable {
    CleanupState *state;
    void on_disable() noexcept {
        try {
            for (auto object : state->retired)
                if (object.valid())
                    state->failed = true;
            state->created.push_back(state->scene->create("Cleanup first"));
            state->created.push_back(state->scene->create("Cleanup second"));
        } catch (...) {
            state->failed = true;
        }
    }
};
struct Tag {};
struct Spawner {
    explicit Spawner(ComponentOwner attached) : owner(attached.object) {}
    GameObject owner;
    std::vector<GameObject> spawned;
    void on_update(double) { spawned.push_back(owner.scene().create("Spawned")); }
};
// An aggregate whose first member is a link, not its owner.
struct Pair {
    GameObject a;
    int n;
};
// A component constructed from the object it follows, which is not its owner.
struct Follow {
    explicit Follow(GameObject linked) : target(linked) {}
    GameObject target;
};
// An aggregate that receives its owner, then data.
struct Owned {
    ComponentOwner owner;
    int n;
};
// A component constructed from its owner and a link.
struct Tether {
    Tether(ComponentOwner attached, GameObject linked) : owner(attached.object), target(linked) {}
    GameObject owner, target;
};
template <class T, class... Args>
concept list_initializable = requires(Args &&...args) { T{std::forward<Args>(args)...}; };

// Whether any of Scene's Id mutators can be called here, outside the scene and its friends. Each call matches its
// member's parameters, so only access can reject it.
template <class S>
concept mutates_by_id =
    requires(S &scene, std::shared_ptr<const Mesh> mesh) { scene.add(mesh); } ||
    requires(S &scene, Scene::Id id) { scene.remove(id); } ||
    requires(S &scene, Scene::Id id, const Pose &pose, const Mat4 &world) { scene.set_pose(id, pose, world); } ||
    requires(S &scene, Scene::Id id, const Mat4 &world) { scene.set_transform(id, world); } ||
    requires(S &scene, Scene::Id id, Vec3 factor) { scene.set_material_factor(id, 0, factor); } ||
    requires(S &scene, Scene::Id id) { scene.clear_material_factor(id, 0); } ||
    requires(S &scene, Scene::Id id, std::shared_ptr<const CustomMaterial> custom) {
        scene.set_custom_material(id, 0, custom);
    } || requires(S &scene, Scene::Id id) { scene.set_visible(id, false); } ||
    requires(S &scene, Scene::Id id) { scene.set_primitive_visible(id, 0, false); } ||
    requires(S &scene, Scene::Id id) { scene.set_casts_shadows(id, false); } ||
    requires(S &scene, Scene::Id id, std::shared_ptr<const MeshPlacements> placements) {
        scene.set_placements(id, placements);
    } || requires(S &scene, Scene::Id id, const VisibilityRange &range) { scene.set_visibility_range(id, range); };
// The same check accepts the public read by Id, so a rejection above comes from access alone.
template <class S>
concept reads_by_id = requires(const S &scene, Scene::Id id) { scene.instance(id); };
static_assert(reads_by_id<Scene>);
static_assert(!mutates_by_id<Scene>);
} // namespace

TEST_CASE("A component reaches its scene through its object, and a stale handle has no scene") {
    Scene scene;
    auto object = scene.create("Spawner");
    CHECK(&object.scene() == &scene);
    auto spawner = object.add_component<Spawner>();
    scene.update(0);
    REQUIRE(spawner->spawned.size() == 1);
    const auto spawned = spawner->spawned.front();
    CHECK(spawned.valid());
    CHECK(&spawned.scene() == &scene);
    CHECK(spawned.name() == "Spawned");
    CHECK_FALSE(spawned.parent());
    CHECK(scene.size() == 2);

    object.destroy();
    CHECK_THROWS_WITH_AS((void)object.scene(), "Expired GameObject handle", std::out_of_range);
    CHECK_THROWS_WITH_AS((void)GameObject{}.scene(), "Expired GameObject handle", std::out_of_range);
    auto orphan = std::make_unique<Scene>();
    const auto left = orphan->create();
    orphan.reset();
    CHECK_THROWS_WITH_AS((void)left.scene(), "Expired GameObject handle", std::out_of_range);
}

TEST_CASE("add_component passes the owner only as a ComponentOwner, never to a GameObject parameter or member") {
    // Neither type converts to the other, even by brace elision into an aggregate member.
    static_assert(!std::convertible_to<GameObject, ComponentOwner>);
    static_assert(!std::constructible_from<GameObject, ComponentOwner>);
    static_assert(!list_initializable<Owned, GameObject, int>);
    static_assert(!list_initializable<Pair, ComponentOwner, int>);
    static_assert(!std::constructible_from<Follow, ComponentOwner>);

    Scene scene;
    auto object = scene.create("owner");
    auto other = scene.create("other");
    const auto pair = object.add_component<Pair>(other, 1);
    CHECK(pair->a == other);
    CHECK(pair->n == 1);
    const auto unlinked = other.add_component<Pair>();
    CHECK(unlinked->a == GameObject{});
    CHECK(unlinked->n == 0);
    const auto follow = object.add_component<Follow>(other);
    CHECK(follow->target == other);

    const auto owned = object.add_component<Owned>(2);
    CHECK(owned->owner.object == object);
    CHECK(owned->n == 2);
    const auto tether = object.add_component<Tether>(other);
    CHECK(tether->owner == object);
    CHECK(tether->target == other);
    const auto view = object.add_component<CameraView>(other);
    CHECK(view->camera == other);
    CHECK(other.add_component<CameraView>()->camera == GameObject{});
}

TEST_CASE("Object handles compare and hash by identity, and a root's parent is an invalid handle") {
    Scene scene, other;
    const auto root = scene.create("Root");
    auto child = scene.create("Child");
    child.set_parent(root);
    const auto stranger = other.create("Root");

    const auto found = scene.find(child.key());
    CHECK(found == child);
    CHECK(std::hash<GameObject>{}(found) == std::hash<GameObject>{}(child));
    CHECK(std::hash<Scene::Id>{}(found.id()) == std::hash<GameObject>{}(child));
    CHECK(root != child);
    // Objects of different scenes differ even where their slots and generations match.
    REQUIRE(stranger.id().slot == root.id().slot);
    REQUIRE(stranger.id().generation == root.id().generation);
    CHECK(stranger != root);
    CHECK(GameObject{} == GameObject{});
    CHECK(GameObject{} != root);
    const std::unordered_set<GameObject> handles{root, child, found, stranger};
    CHECK(handles.size() == 3);
    CHECK(handles.contains(scene.find(root.key())));

    CHECK(child.parent() == root);
    CHECK_FALSE(root.parent());
    CHECK(root.parent() == GameObject{});
    int parents = 0;
    if (const auto parent = child.parent()) {
        CHECK(parent == root);
        ++parents;
    }
    if (root.parent())
        ++parents;
    CHECK(parents == 1);

    // A destroyed object's handles stay equal to each other, and its reused slot names another object.
    const auto copy = child;
    child.destroy();
    CHECK_FALSE(copy);
    CHECK(copy == found);
    const auto recycled = scene.create("Recycled");
    REQUIRE(recycled.id().slot == copy.id().slot);
    CHECK(recycled != copy);
    CHECK_THROWS_WITH_AS((void)copy.parent(), "Expired GameObject handle", std::out_of_range);
}

TEST_CASE("Component handles are equal when they refer to the same attachment, or both to none") {
    Scene scene;
    auto object = scene.create();
    const auto added = object.add_component<Tag>();
    CHECK(object.get_component<Tag>() == added);
    CHECK(object.get_component<ObjectTransform>() == object.get_component<ObjectTransform>());
    CHECK(object.get_component<ObjectTransform>() != scene.create().get_component<ObjectTransform>());
    CHECK(object.get_component<Spawner>() == ComponentRef<Spawner>{});
    CHECK(added != ComponentRef<Tag>{});
    CHECK(scene.create().add_component<Tag>() != added);

    const auto copy = added;
    CHECK(object.remove_component<Tag>());
    CHECK_FALSE(copy);
    CHECK(copy == added);
    CHECK(added != ComponentRef<Tag>{});
    const auto readded = object.add_component<Tag>();
    CHECK(readded != added);
    CHECK(object.get_component<Tag>() == readded);
}

TEST_CASE("Objects append in slot order, and recreated objects fill holes first-free with new generations") {
    Scene scene;
    std::vector<GameObject> objects;
    for (std::size_t i = 0; i < 256; ++i) {
        CAPTURE(i);
        auto object = scene.create();
        CHECK(object.id().slot == i);
        CHECK(scene.find(object.key()).id() == object.id());
        objects.push_back(object);
    }
    const std::array<std::size_t, 3> holes{23, 70, 89};
    const std::array retired_ids{objects[23].id(), objects[70].id(), objects[89].id()};
    const std::array retired_keys{objects[23].key(), objects[70].key(), objects[89].key()};
    for (auto index : {89U, 23U, 70U})
        objects[index].destroy();
    for (std::size_t i = 0; i < holes.size(); ++i) {
        CAPTURE(i);
        auto replacement = scene.create();
        CHECK(replacement.id().slot == holes[i]);
        CHECK(replacement.id().generation == retired_ids[i].generation + 1);
        CHECK_FALSE(objects[holes[i]].valid());
        CHECK_FALSE(scene.find(retired_keys[i]).valid());
    }
    CHECK(scene.create().id().slot == 256);
    CHECK(scene.size() == 257);
}

TEST_CASE("Creation reuses the lowest free slot through interleaved creation and subtree removal") {
    Scene scene;
    std::vector<GameObject> live;
    std::set<std::size_t> vacant; // The free slots, as a model of the scene's reuse order.
    std::size_t created_slots = 0;
    std::uint32_t sequence = 12345;
    const auto below = [&](std::size_t bound) {
        sequence = sequence * 1664525U + 1013904223U; // A fixed linear congruential sequence.
        return static_cast<std::size_t>(sequence >> 8) % bound;
    };
    for (unsigned round = 0; round < 2000; ++round) {
        CAPTURE(round);
        if (live.empty() || below(4) < 2) {
            const auto expected = vacant.empty() ? created_slots++ : vacant.extract(vacant.begin()).value();
            auto object = scene.create();
            CHECK(object.id().slot == expected);
            // Attach half of them, so removal frees whole subtrees.
            if (!live.empty() && below(2))
                object.set_parent(live[below(live.size())], ReparentMode::keep_local);
            live.push_back(object);
        } else {
            auto removed = live[below(live.size())];
            removed.destroy();
            std::erase_if(live, [&](const GameObject &object) {
                if (object.valid())
                    return false;
                vacant.insert(object.id().slot);
                return true;
            });
        }
    }
    CHECK(scene.size() == live.size());
    CHECK(created_slots > live.size()); // Some slots were free at the end, so both paths ran.
}

TEST_CASE("Cleanup callbacks see a fully retired subtree, and objects they create reuse its first slots") {
    CleanupState cleanup; // Application state outlives scene component teardown.
    Scene scene;
    cleanup.scene = &scene;
    std::vector<GameObject> objects;
    for (unsigned i = 0; i < 12; ++i)
        objects.push_back(scene.create());
    objects[2].set_parent(objects[7], ReparentMode::keep_local);
    objects[9].set_parent(objects[7], ReparentMode::keep_local);
    objects[5].set_parent(objects[2], ReparentMode::keep_local);
    cleanup.retired = {objects[7], objects[2], objects[9], objects[5]};
    objects[7].add_component<CreateOnDisable>(&cleanup);
    scene.synchronize_lifecycle();
    objects[7].destroy();
    CHECK_FALSE(cleanup.failed);
    REQUIRE(cleanup.created.size() == 2);
    CHECK(cleanup.created[0].id().slot == 2);
    CHECK(cleanup.created[1].id().slot == 5);
    // The remaining holes, then growth.
    CHECK(scene.create().id().slot == 7);
    CHECK(scene.create().id().slot == 9);
    CHECK(scene.create().id().slot == 12);
    for (auto object : cleanup.retired)
        CHECK_FALSE(object.valid());
}

TEST_CASE("A failed creation publishes nothing, keeps its free slot and consumes its identity") {
    auto source = std::make_shared<Asset>(*scene_objects_test::source());
    // Compiling accepts finite authored geometry; adding the renderer rejects
    // overflow when its conservative world bounds are padded.
    source->primitives[0].vertices[0].position.x = std::numeric_limits<float>::max();
    const auto oversized_mesh = Mesh::compile(*source);
    Scene scene;
    std::vector<GameObject> objects;
    for (unsigned i = 0; i < 4; ++i)
        objects.push_back(scene.create());
    const auto retired = objects[1].id();
    objects[1].destroy();
    const ObjectKey failed_key{objects.back().key().value + 1};
    CHECK_THROWS_WITH_AS(scene.create("Overflow", oversized_mesh), "Non-finite render bounds", std::invalid_argument);
    CHECK(scene.size() == 3);
    CHECK(scene.instances().empty());
    CHECK_FALSE(scene.find(failed_key).valid());
    const auto replacement = scene.create();
    CHECK(replacement.id().slot == retired.slot);
    CHECK(replacement.id().generation == retired.generation + 2);
    CHECK(replacement.key().value == failed_key.value + 1);
    CHECK_FALSE(objects[1].valid());
    CHECK(scene.create().id().slot == 4);
}

TEST_CASE("Children keep their attachment order through removal, reparenting and slot reuse") {
    Scene scene;
    auto parent = scene.create("Parent"), other = scene.create("Other");
    std::vector<GameObject> children;
    for (unsigned i = 0; i < 5; ++i) {
        children.push_back(scene.create());
        children.back().set_parent(parent);
    }
    const auto expect = [&](const GameObject &owner, std::vector<GameObject> expected) {
        CHECK(ids_of(owner.children()) == ids_of(expected));
    };
    expect(parent, children);
    children[2].destroy(); // Middle.
    expect(parent, {children[0], children[1], children[3], children[4]});
    children[0].set_parent(other); // First.
    children[4].clear_parent();    // Last.
    expect(parent, {children[1], children[3]});
    expect(other, {children[0]});
    children[0].set_parent(parent); // Reattached children go last.
    children[4].set_parent(parent);
    expect(parent, {children[1], children[3], children[0], children[4]});
    expect(other, {});
    children[3].destroy();
    children[1].destroy();
    expect(parent, {children[0], children[4]});
    auto reused = scene.create(); // Reuses the lowest destroyed child's slot without its links.
    CHECK(reused.id().slot == children[1].id().slot);
    CHECK(reused.children().empty());
    CHECK_FALSE(reused.parent());
    reused.set_parent(parent);
    expect(parent, {children[0], children[4], reused});
    parent.destroy();
    CHECK(scene.size() == 1);
    CHECK_FALSE(children[0].valid());
    CHECK_FALSE(reused.valid());
    expect(other, {});
}

TEST_CASE("Reparenting rejects a parent inside the moved subtree and changes nothing") {
    Scene scene;
    auto root = scene.create("Root"), branch = scene.create("Branch"), sibling = scene.create("Sibling");
    branch.set_parent(root);
    sibling.set_parent(root);
    std::vector<GameObject> chain{branch};
    for (unsigned i = 0; i < 64; ++i) {
        auto next = scene.create();
        next.set_parent(chain.back());
        chain.push_back(next);
    }
    auto offshoot = scene.create("Offshoot");
    offshoot.set_parent(chain[10]);
    for (std::size_t i = 0; i < chain.size(); ++i) {
        CAPTURE(i);
        CHECK_THROWS_WITH_AS(branch.set_parent(chain[i]), parenting_cycle, std::invalid_argument);
    }
    CHECK_THROWS_WITH_AS(branch.set_parent(offshoot), parenting_cycle, std::invalid_argument);
    CHECK_THROWS_WITH_AS(root.set_parent(chain.back()), parenting_cycle, std::invalid_argument);
    CHECK_THROWS_WITH_AS(offshoot.set_parent(offshoot), parenting_cycle, std::invalid_argument);
    CHECK(ids_of(root.children()) == ids_of({branch, sibling}));
    CHECK(ids_of(chain[10].children()) == ids_of({chain[11], offshoot}));
    CHECK_FALSE(root.parent());
    branch.set_parent(sibling, ReparentMode::keep_local);
    CHECK(ids_of(root.children()) == ids_of({sibling}));
    CHECK(ids_of(sibling.children()) == ids_of({branch}));
    CHECK(chain.back().parent() == chain[chain.size() - 2]);
}

TEST_CASE("Instances and snapshots keep the order renderers were added through removal and replacement") {
    const auto mesh = Mesh::compile(*scene_objects_test::source());
    Scene scene;
    std::vector<GameObject> objects;
    for (unsigned i = 0; i < 12; ++i) {
        objects.push_back(scene.create({}, mesh));
        objects.back().set_position({static_cast<float>(i), 0, 0}); // Identifies it in snapshots.
    }
    auto group = scene.create("Group");
    objects[9].set_parent(group);
    objects[10].set_parent(group);
    const auto expect = [&](const std::vector<std::size_t> &order) {
        std::vector<Scene::Id> expected;
        for (auto index : order)
            expected.push_back(objects[index].id());
        const auto listed = scene.instances();
        CHECK(std::vector<Scene::Id>(listed.begin(), listed.end()) == expected);
        const auto snapshot = scene.snapshot();
        REQUIRE(snapshot.primitives.size() == order.size());
        for (std::size_t i = 0; i < order.size(); ++i) {
            CAPTURE(i);
            CHECK(snapshot.primitives[i].node_world[12] == static_cast<float>(order[i]));
        }
    };
    objects[2].destroy();
    expect({0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11});
    objects[4].remove_mesh();
    expect({0, 1, 3, 5, 6, 7, 8, 9, 10, 11});
    (void)objects[4].add_mesh(mesh); // A renderer added again goes last.
    expect({0, 1, 3, 5, 6, 7, 8, 9, 10, 11, 4});
    objects[0].renderer().set_mesh(mesh); // Replacing a mesh keeps its place.
    expect({0, 1, 3, 5, 6, 7, 8, 9, 10, 11, 4});
    group.destroy();
    // Most of the rest, removed one at a time without reading instances() in between.
    for (auto index : {1U, 3U, 5U, 6U, 7U})
        objects[index].destroy();
    expect({0, 8, 11, 4});
    objects.push_back(scene.create({}, mesh)); // Reuses a slot but is added last.
    objects.back().set_position({12, 0, 0});
    expect({0, 8, 11, 4, 12});
}

TEST_CASE("A renderer reads back its visibility, shadows, material factors, primitives and pose") {
    const auto asset = scene_objects_test::source();
    const auto mesh = Mesh::compile(*asset);
    Scene scene;
    auto object = scene.create("Body", mesh);
    auto renderer = object.renderer();
    CHECK(renderer.visible());
    CHECK(renderer.casts_shadows());
    CHECK(renderer.material_factor(0) == Vec3{1, 1, 1});
    CHECK_FALSE(renderer.custom_material(0));
    CHECK(renderer.primitive_visible(0));
    CHECK(&renderer.pose() == &mesh->rest_pose()); // An unposed renderer reads its mesh's rest pose.

    renderer.set_visible(false);
    renderer.set_casts_shadows(false);
    renderer.set_material_factor(0, {.2F, .3F, .4F});
    renderer.set_primitive_visible(0, false);
    const auto moved = sample_pose(*asset, &asset->animations[0], .5);
    REQUIRE(moved != mesh->rest_pose());
    renderer.set_pose(moved);
    CHECK_FALSE(renderer.visible());
    CHECK_FALSE(renderer.casts_shadows());
    CHECK(renderer.material_factor(0) == Vec3{.2F, .3F, .4F});
    CHECK_FALSE(renderer.primitive_visible(0));
    CHECK(renderer.pose() == moved);
    renderer.clear_material_factor(0);
    CHECK(renderer.material_factor(0) == Vec3{1, 1, 1});

    // A bad index throws the same std::out_of_range from a getter and its setters.
    constexpr auto material_slot = "Material factor slot is outside the mesh's materials";
    constexpr auto custom_slot = "Custom material slot is outside the mesh's materials";
    constexpr auto primitive_slot = "Primitive is outside the mesh's primitives";
    CHECK_THROWS_WITH_AS((void)renderer.material_factor(1), material_slot, std::out_of_range);
    CHECK_THROWS_WITH_AS(renderer.set_material_factor(1, {}), material_slot, std::out_of_range);
    CHECK_THROWS_WITH_AS(renderer.clear_material_factor(1), material_slot, std::out_of_range);
    CHECK_THROWS_WITH_AS((void)renderer.custom_material(1), custom_slot, std::out_of_range);
    CHECK_THROWS_WITH_AS(renderer.set_custom_material(1, nullptr), custom_slot, std::out_of_range);
    CHECK_THROWS_WITH_AS((void)renderer.primitive_visible(1), primitive_slot, std::out_of_range);
    CHECK_THROWS_WITH_AS(renderer.set_primitive_visible(1, true), primitive_slot, std::out_of_range);

    // Replacing the mesh resets what each getter reads.
    renderer.set_mesh(mesh);
    CHECK(renderer.visible());
    CHECK(renderer.casts_shadows());
    CHECK(renderer.primitive_visible(0));
    CHECK(&renderer.pose() == &mesh->rest_pose());

    object.remove_mesh();
    constexpr auto no_renderer = "GameObject has no MeshRenderer";
    CHECK_THROWS_WITH_AS((void)renderer.visible(), no_renderer, std::logic_error);
    CHECK_THROWS_WITH_AS((void)renderer.casts_shadows(), no_renderer, std::logic_error);
    CHECK_THROWS_WITH_AS((void)renderer.material_factor(0), no_renderer, std::logic_error);
    CHECK_THROWS_WITH_AS((void)renderer.custom_material(0), no_renderer, std::logic_error);
    CHECK_THROWS_WITH_AS((void)renderer.primitive_visible(0), no_renderer, std::logic_error);
    CHECK_THROWS_WITH_AS((void)renderer.pose(), no_renderer, std::logic_error);
}

TEST_CASE("A renderer's pose keeps the object's world matrix unless the call also sets it") {
    const auto source = scene_objects_test::source();
    const auto mesh = Mesh::compile(*source);
    const auto pose = sample_pose(*source, &source->animations[0], .5);
    const auto &rest = mesh->rest_pose();
    Scene scene;
    auto object = scene.create("Posed", mesh);
    auto renderer = object.renderer();
    const auto moved = matrix(Transform{.translation = {3, 4, 5}, .scale = {2, 2, 2}});
    object.set_world_matrix(moved);
    // The pose alone poses the object where it is.
    renderer.set_pose(pose);
    CHECK(object.world_matrix() == moved);
    CHECK(scene.instance(object.id()).world == moved);
    CHECK(scene.instance(object.id()).palette[0] == moved * pose.world[0]);

    // With a world matrix the call sets both, and the local matrix follows the parent.
    auto parent = scene.create("Parent");
    parent.set_world_matrix(matrix(Transform{.translation = {1, 0, 0}}));
    object.set_parent(parent);
    const auto placed = matrix(Transform{.translation = {-2, 1, 0}, .scale = {1, 3, 1}});
    renderer.set_pose(rest, placed);
    CHECK(object.world_matrix() == placed);
    CHECK(object.local_matrix() == inverse(parent.world_matrix()) * placed);
    CHECK(scene.instance(object.id()).palette[0] == placed * rest.world[0]);

    // A pose rejected with a world matrix leaves the object where it was.
    auto mismatched = pose;
    mismatched.world.pop_back();
    CHECK_THROWS_WITH_AS(renderer.set_pose(mismatched, moved), "Pose does not match render asset",
                         std::invalid_argument);
    CHECK(object.world_matrix() == placed);

    // Under a collapsed parent only the call that sets a world matrix needs the parent's inverse.
    parent.set_local_matrix(matrix(Transform{.scale = {1, 0, 1}}));
    const auto collapsed = object.world_matrix();
    CHECK_THROWS_WITH_AS(renderer.set_pose(pose, moved), math_error_message(MathErrorCode::singular_matrix), MathError);
    CHECK(object.world_matrix() == collapsed);
    CHECK(scene.instance(object.id()).palette[0] == collapsed * rest.world[0]);
    renderer.set_pose(pose);
    CHECK(object.world_matrix() == collapsed);
    CHECK(scene.instance(object.id()).palette[0] == collapsed * pose.world[0]);
}

namespace {
// Checks that @p actual is @p expected within doctest's default tolerance, component by component.
void check_near(Vec3 actual, Vec3 expected) {
    CHECK(actual.x == doctest::Approx(expected.x));
    CHECK(actual.y == doctest::Approx(expected.y));
    CHECK(actual.z == doctest::Approx(expected.z));
}
// Checks that @p actual and @p expected are the same rotation; q and -q are.
void check_rotation(const Quat &actual, const Quat &expected) {
    float cosine = 0;
    for (std::size_t i = 0; i < 4; ++i)
        cosine += actual[i] * expected[i];
    CHECK(std::abs(cosine) == doctest::Approx(1));
}
} // namespace

TEST_CASE("Objects read and replace rotation and scale, report their facing and look at targets") {
    constexpr float quarter_turn = std::numbers::pi_v<float> / 2;
    Scene scene;
    auto object = scene.create("Turned");
    const auto yaw = axis_angle(world_up, .5F);
    object.set_local_transform({{1, 2, 3}, yaw, {2, 3, 4}});
    check_rotation(object.local_rotation(), yaw);
    check_near(object.local_scale(), {2, 3, 4});
    REQUIRE(object.local_transform());
    check_rotation(object.local_transform()->rotation, yaw);

    // Replacing the rotation keeps the translation exactly and the scale.
    const auto pitch = axis_angle({1, 0, 0}, .25F);
    object.set_rotation(pitch);
    CHECK(object.position().x == 1);
    CHECK(object.position().y == 2);
    CHECK(object.position().z == 3);
    check_near(object.local_scale(), {2, 3, 4});
    check_rotation(object.rotation(), pitch);
    object.transform().set_local_scale({1, 1, 1});
    check_rotation(object.transform().local_rotation(), pitch);
    check_near(object.transform().local_scale(), {1, 1, 1});

    // Directions follow the world matrix: a quarter turn left faces -X.
    object.set_rotation(axis_angle(world_up, quarter_turn));
    check_near(object.forward(), {-1, 0, 0});
    check_near(object.right(), {0, 0, -1});
    check_near(object.up(), {0, 1, 0});

    // A child's world rotation includes its parent's; setting it derives the local rotation.
    auto child = scene.create("Child");
    child.set_parent(object, ReparentMode::keep_local);
    check_rotation(child.rotation(), object.rotation());
    check_near(child.transform().forward(), {-1, 0, 0});
    child.set_rotation(identity_rotation);
    check_rotation(child.rotation(), identity_rotation);
    check_rotation(child.local_rotation(), conjugate(object.rotation()));

    // Looking straight down is parallel to world_up, which another up resolves; the rejected call changes nothing.
    object.set_local_scale({2, 2, 2});
    const auto before = object.world_matrix();
    CHECK_THROWS_WITH_AS(object.look_at({1, -5, 3}), math_error_message(MathErrorCode::invalid_look), MathError);
    CHECK(object.world_matrix() == before);
    object.transform().look_at({1, -5, 3}, {0, 0, -1});
    check_near(object.forward(), {0, -1, 0});
    check_near(object.up(), {0, 0, -1});
    check_near(object.local_scale(), {2, 2, 2});
    CHECK(object.position().y == 2);
    object.look_at({4, 2, 7});
    check_near(object.forward(), {.6F, 0, .8F});
}

TEST_CASE("Rotation and scale setters keep a reflection, drop shear and need a rotation to keep") {
    Scene scene;
    auto object = scene.create("Mirrored");
    auto mirrored = identity();
    mirrored[0] = -1;
    object.set_local_matrix(mirrored);
    check_near(object.local_scale(), {-1, 1, 1});
    check_rotation(object.local_rotation(), identity_rotation);
    const auto turn = axis_angle({0, 0, 1}, .4F);
    object.set_local_rotation(turn);
    check_near(object.local_scale(), {-1, 1, 1});
    check_rotation(object.local_rotation(), turn);

    // Shear has no T * R * S form, but its nearest rotation does; a rotation setter rebuilds without the shear.
    auto sheared = identity();
    sheared[4] = .5F;
    object.set_local_matrix(sheared);
    CHECK_FALSE(object.transform().local_transform());
    const auto nearest = object.local_rotation();
    object.set_local_rotation(nearest);
    REQUIRE(object.local_transform());
    check_rotation(object.local_transform()->rotation, nearest);

    // A collapsed object keeps its zero scale through a rotation, but has no rotation to read or keep.
    const auto collapsed = math_error_message(MathErrorCode::collapsed_transform);
    object.set_local_scale({0, 0, 0});
    check_near(object.local_scale(), {0, 0, 0});
    CHECK_FALSE(object.local_transform());
    CHECK_THROWS_WITH_AS((void)object.local_rotation(), collapsed, MathError);
    CHECK_THROWS_WITH_AS((void)object.rotation(), collapsed, MathError);
    CHECK_THROWS_WITH_AS(object.set_local_scale({1, 1, 1}), collapsed, MathError);
    object.set_local_rotation(turn);
    check_near(object.local_scale(), {0, 0, 0});
    // A collapsed axis has no direction, so it falls back to world_up as normalized() does.
    check_near(object.forward(), world_up);

    object.set_local_transform({});
    CHECK_THROWS_WITH_AS(object.set_local_scale({std::numeric_limits<float>::infinity(), 1, 1}),
                         "Non-finite instance transform", std::invalid_argument);
    CHECK_THROWS_WITH_AS(object.set_rotation({}), math_error_message(MathErrorCode::zero_quaternion), MathError);
    CHECK(object.local_matrix() == identity());
}

TEST_CASE("The shared consumer scenario for objects, lifetime, terrain, mesh preparation and animation passes") {
    // It reports a failed check by throwing std::runtime_error, which fails this test case.
    scene_objects_test::run();
}
