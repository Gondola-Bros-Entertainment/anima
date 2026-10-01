// Placements: copies of one rigid mesh drawn from a single object, their clusters and bounds, how scenes apply and
// move them, and their persistence in scene, prefab and prefab variant documents.
#include <algorithm>
#include <anima/mesh_placements.hpp>
#include <anima/prefab.hpp>
#include <anima/prefab_variant.hpp>
#include <anima/scene.hpp>
#include <doctest/doctest.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace anima;

namespace {
// A rigid mesh of one triangle with corners (0, 0, 0), (1, 0, 0) and (0, 1, 0) on a node translated by @p offset.
std::shared_ptr<const Mesh> triangle(Vec3 offset = {}) {
    Asset asset;
    asset.nodes.resize(1);
    asset.nodes[0].rest.translation = offset;
    asset.materials.push_back({"surface", {1, 1, 1}, -1});
    SourcePrimitive primitive;
    primitive.material = 0;
    for (const auto corner : {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}}) {
        SourceVertex vertex;
        vertex.position = corner;
        vertex.normal = {0, 0, 1};
        primitive.vertices.push_back(vertex);
    }
    asset.primitives.push_back(std::move(primitive));
    return Mesh::compile(asset);
}
// A mesh whose one triangle two joints skin.
std::shared_ptr<const Mesh> skinned() {
    Asset asset;
    asset.nodes.resize(2);
    asset.skins.push_back({{0, 1}, {identity(), identity()}});
    asset.materials.push_back({"surface", {1, 1, 1}, -1});
    SourcePrimitive primitive;
    primitive.skin = 0;
    primitive.material = 0;
    for (const auto corner : {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}}) {
        SourceVertex vertex;
        vertex.position = corner;
        vertex.normal = {0, 0, 1};
        vertex.joints = {0, 1, 0, 0};
        vertex.weights = {.5F, .5F, 0, 0};
        primitive.vertices.push_back(vertex);
    }
    asset.primitives.push_back(std::move(primitive));
    return Mesh::compile(asset);
}
Mat4 translation(Vec3 t) {
    auto m = identity();
    set_translation(m, t);
    return m;
}
// A grid of @p side by @p side placements @p spacing apart in X and Z, each also scaled by 2 in Y.
std::vector<Mat4> grid(int side, float spacing) {
    std::vector<Mat4> result;
    for (int z = 0; z < side; ++z)
        for (int x = 0; x < side; ++x) {
            auto m = translation({float(x) * spacing, 0, float(z) * spacing});
            m[5] = 2;
            result.push_back(m);
        }
    return result;
}
bool contains(const RenderBounds &bounds, Vec3 v) {
    return bounds.valid && v.x >= bounds.minimum.x && v.y >= bounds.minimum.y && v.z >= bounds.minimum.z &&
           v.x <= bounds.maximum.x && v.y <= bounds.maximum.y && v.z <= bounds.maximum.z;
}
bool same(const Mat4 &a, const Mat4 &b) { return std::equal(a.begin(), a.end(), b.begin()); }
// The corners of the triangle mesh's copy at @p placement under @p world, with the mesh node at @p offset.
std::vector<Vec3> copy_corners(const Mat4 &world, const Mat4 &placement, Vec3 offset = {}) {
    std::vector<Vec3> result;
    for (const auto corner : {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}})
        result.push_back(point(world * placement * translation(offset), corner));
    return result;
}
} // namespace

TEST_CASE("Placements reject what they cannot draw") {
    const auto mesh = triangle();
    const std::vector<Mat4> one{identity()};
    CHECK_THROWS_WITH_AS((void)MeshPlacements::create(nullptr, one), "Placements require a mesh",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)MeshPlacements::create(skinned(), one), "Placements draw only rigid meshes",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)MeshPlacements::create(mesh, {}), "Placement count must be from 1 to 1048576",
                         std::invalid_argument);
    const std::vector<Mat4> too_many(MeshPlacements::max_count + 1, identity());
    CHECK_THROWS_WITH_AS((void)MeshPlacements::create(mesh, too_many), "Placement count must be from 1 to 1048576",
                         std::invalid_argument);
    for (const auto element : {3, 7, 11, 15}) {
        auto projective = identity();
        projective[element] += .5F;
        const std::vector<Mat4> value{identity(), projective};
        CHECK_THROWS_WITH_AS((void)MeshPlacements::create(mesh, value),
                             "Placement transforms must be finite and affine", std::invalid_argument);
    }
    auto nonfinite = identity();
    nonfinite[12] = std::numeric_limits<float>::quiet_NaN();
    CHECK_THROWS_WITH_AS((void)MeshPlacements::create(mesh, std::vector<Mat4>{nonfinite}),
                         "Placement transforms must be finite and affine", std::invalid_argument);
    auto huge = identity();
    huge[0] = std::numeric_limits<float>::max();
    huge[12] = std::numeric_limits<float>::max();
    CHECK_THROWS_WITH_AS((void)MeshPlacements::create(mesh, std::vector<Mat4>{huge}),
                         "Placed copies exceed the finite range", std::invalid_argument);
    CHECK(MeshPlacements::create(mesh, std::vector<Mat4>(MeshPlacements::max_count, identity()))->transforms().size() ==
          MeshPlacements::max_count);
}

TEST_CASE("Placements group neighbours into clusters whose bounds hold every copy") {
    const Vec3 offset{0, .5F, 0};
    const auto mesh = triangle(offset);
    const auto given = grid(32, 10);
    const auto placements = MeshPlacements::create(mesh, given);
    CHECK(placements->mesh() == mesh);
    const auto transforms = placements->transforms();
    REQUIRE(transforms.size() == given.size());
    // Every placement appears exactly once.
    for (const auto &m : given)
        CHECK(std::count_if(transforms.begin(), transforms.end(), [&](const Mat4 &t) { return same(t, m); }) == 1);

    std::uint32_t next = 0;
    for (const auto &cluster : placements->clusters()) {
        CHECK(cluster.first == next);
        CHECK(cluster.count >= 1);
        CHECK(cluster.count <= MeshPlacements::cluster_size);
        next += cluster.count;
        for (std::uint32_t i = cluster.first; i < cluster.first + cluster.count; ++i)
            for (const auto corner : copy_corners(identity(), transforms[i], offset)) {
                CHECK(contains(cluster.bounds, corner));
                CHECK(contains(placements->primitive_bounds()[0], corner));
                CHECK(contains(placements->bounds(), corner));
            }
        // A 32 by 32 grid in Morton order fills each cluster of 64 with an 8 by 8 block, 70 m across of 310.
        CHECK(cluster.bounds.maximum.x - cluster.bounds.minimum.x < 72);
        CHECK(cluster.bounds.maximum.z - cluster.bounds.minimum.z < 72);
    }
    CHECK(next == transforms.size());
    CHECK(placements->clusters().size() == 16);

    // Creating placements from transforms() keeps that order, so documents round trip it.
    const auto again = MeshPlacements::create(mesh, std::vector<Mat4>(transforms.begin(), transforms.end()));
    REQUIRE(again->transforms().size() == transforms.size());
    for (std::size_t i = 0; i < transforms.size(); ++i)
        CHECK(same(again->transforms()[i], transforms[i]));
}

TEST_CASE("A renderer with placements draws a copy at each one and bounds them all") {
    const auto mesh = triangle();
    const auto placements = MeshPlacements::create(mesh, grid(4, 3));
    Scene scene;
    auto object = scene.create("grove", mesh);
    const auto world = translation({100, 0, -50});
    object.set_world_matrix(world);
    auto renderer = object.renderer();
    CHECK_FALSE(renderer.placements());
    renderer.set_placements(placements);
    CHECK(renderer.placements() == placements);
    const auto &instance = scene.instance(object.id());
    CHECK(same(instance.world, world));
    for (const auto &placement : placements->transforms())
        for (const auto corner : copy_corners(world, placement)) {
            CHECK(contains(instance.primitive_bounds[0], corner));
            CHECK(contains(instance.bounds, corner));
            CHECK(contains(renderer.bounds(), corner));
            CHECK(contains(scene.bounds(), corner));
        }
    // The bounds follow the object.
    const auto moved = translation({-20, 5, 0});
    object.set_world_matrix(moved);
    CHECK(same(scene.instance(object.id()).world, moved));
    for (const auto &placement : placements->transforms())
        for (const auto corner : copy_corners(moved, placement))
            CHECK(contains(scene.instance(object.id()).bounds, corner));
    CHECK_FALSE(contains(scene.instance(object.id()).bounds, point(world, {9.5F, 0, 9.5F})));

    // Null draws the one copy at the object again.
    renderer.set_placements(nullptr);
    CHECK_FALSE(scene.instance(object.id()).placements);
    CHECK(contains(scene.instance(object.id()).bounds, point(moved, {1, 0, 0})));
    CHECK_FALSE(contains(scene.instance(object.id()).bounds, point(moved, {9, 0, 9})));

    // A new mesh resets the placements, as it resets other renderer settings.
    renderer.set_placements(placements);
    renderer.set_mesh(triangle());
    CHECK_FALSE(renderer.placements());
}

TEST_CASE("Placements keep the rest pose and copy only their own mesh") {
    const auto mesh = triangle();
    const auto placements = MeshPlacements::create(mesh, grid(2, 3));
    Scene scene;
    auto object = scene.create("grove", mesh);
    auto renderer = object.renderer();
    CHECK_THROWS_WITH_AS(renderer.set_placements(MeshPlacements::create(triangle(), grid(2, 3))),
                         "Placements copy another mesh", std::invalid_argument);
    renderer.set_placements(placements);
    CHECK_THROWS_WITH_AS(renderer.set_pose(mesh->rest_pose()), "A renderer that draws placements keeps the rest pose",
                         std::logic_error);
    renderer.set_placements(nullptr);
    renderer.set_pose(mesh->rest_pose());
    CHECK_THROWS_WITH_AS(renderer.set_placements(placements), "A renderer with a pose cannot draw placements",
                         std::logic_error);
    CHECK_FALSE(renderer.placements());
    auto bare = scene.create("bare");
    CHECK_THROWS_AS(scene.set_placements(bare.id(), placements), std::logic_error);
}

TEST_CASE("Snapshots expand every copy") {
    const auto mesh = triangle();
    const std::vector<Mat4> given{translation({0, 0, 0}), translation({5, 0, 0}), translation({0, 0, 7})};
    const auto placements = MeshPlacements::create(mesh, given);
    Scene scene;
    auto object = scene.create("grove", mesh);
    object.set_world_matrix(translation({0, 2, 0}));
    object.renderer().set_placements(placements);
    const auto snapshot = scene.snapshot();
    REQUIRE(snapshot.primitives.size() == 3);
    REQUIRE(snapshot.vertices.size() == 9);
    for (std::size_t copy = 0; copy < 3; ++copy) {
        const auto expected = copy_corners(translation({0, 2, 0}), placements->transforms()[copy]);
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto &v = snapshot.vertices[copy * 3 + corner].position;
            CHECK(v.x == doctest::Approx(expected[corner].x));
            CHECK(v.y == doctest::Approx(expected[corner].y));
            CHECK(v.z == doctest::Approx(expected[corner].z));
        }
    }
    CHECK(snapshot.maximum.x == doctest::Approx(6));
    CHECK(snapshot.maximum.z == doctest::Approx(7));
}

TEST_CASE("Scene documents and prefabs keep placements") {
    const auto mesh = triangle();
    const auto placements = MeshPlacements::create(mesh, grid(5, 2));
    Scene scene;
    auto object = scene.create("grove", mesh);
    object.renderer().set_placements(placements);
    const MeshName name = [](const std::shared_ptr<const Mesh> &) { return std::string("triangle"); };
    const MeshResolver resolve = [&](std::string_view) { return mesh; };
    const auto document = serialize_scene(scene, name);
    CHECK(document.find("\"placements\"") != std::string::npos);
    const auto loaded = load_scene(document, resolve);
    const auto ids = loaded->instances();
    REQUIRE(ids.size() == 1);
    const auto &copied = loaded->instance(ids[0]).placements;
    REQUIRE(copied);
    REQUIRE(copied->transforms().size() == placements->transforms().size());
    for (std::size_t i = 0; i < copied->transforms().size(); ++i)
        CHECK(same(copied->transforms()[i], placements->transforms()[i]));

    // A prefab captures the placements by reference and its instances draw them.
    const auto prefab = Prefab::capture(object);
    CHECK(prefab.nodes()[0].placements == placements);
    Scene target;
    const auto instance = prefab.instantiate(target);
    CHECK(instance.renderer().placements() == placements);

    // A renderer without placements writes null, which reading keeps.
    object.renderer().set_placements(nullptr);
    const auto plain = serialize_scene(scene, name);
    CHECK(plain.find("\"placements\": null") != std::string::npos);
    const auto reloaded = load_scene(plain, resolve);
    CHECK_FALSE(reloaded->instance(reloaded->instances()[0]).placements);
}

TEST_CASE("Documents reject placements that a renderer cannot draw") {
    const auto mesh = triangle();
    const MeshResolver resolve = [&](std::string_view) { return mesh; };
    const std::string identity_matrix = "[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]";
    const auto document = [](const std::string &mesh_key, const std::string &pose, const std::string &placements) {
        return R"({"version":3,"kind":"anima.scene","next_key":"2","objects":[{"key":"1","name":"grove","parent":null,)"
               R"("local":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"mesh":)" +
               mesh_key + R"(,"pose":)" + pose + R"(,"placements":)" + placements + "}]}";
    };
    CHECK_NOTHROW((void)load_scene(document("\"triangle\"", "null", "[" + identity_matrix + "]"), resolve));
    CHECK_THROWS_WITH_AS((void)load_scene(document("null", "null", "[" + identity_matrix + "]"), resolve),
                         "Scene placements must copy the object's mesh, which has no pose", std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        (void)load_scene(document("\"triangle\"", "[" + identity_matrix + "]", "[" + identity_matrix + "]"), resolve),
        "Scene placements must copy the object's mesh, which has no pose", std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)load_scene(document("\"triangle\"", "null", "[]"), resolve),
                         "Placement count must be from 1 to 1048576", std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        (void)load_scene(document("\"triangle\"", "null", "[[1,0,0,1,0,1,0,0,0,0,1,0,0,0,0,1]]"), resolve),
        "Placement transforms must be finite and affine", std::invalid_argument);

    // Prefab nodes follow the same rules.
    Prefab::Node node;
    node.mesh = mesh;
    node.placements = MeshPlacements::create(triangle(), std::vector<Mat4>{identity()});
    CHECK_THROWS_WITH_AS(Prefab(std::vector<Prefab::Node>{node}),
                         "Scene placements must copy the object's mesh, which has no pose", std::invalid_argument);
    node.placements = MeshPlacements::create(mesh, std::vector<Mat4>{identity()});
    node.pose = mesh->rest_pose();
    CHECK_THROWS_WITH_AS(Prefab(std::vector<Prefab::Node>{node}),
                         "Scene placements must copy the object's mesh, which has no pose", std::invalid_argument);
    node.pose.reset();
    CHECK_NOTHROW(Prefab(std::vector<Prefab::Node>{node}));
}

TEST_CASE("Prefab variants replace placements with the rest of a renderer") {
    const auto mesh = triangle();
    const auto placements = MeshPlacements::create(mesh, grid(3, 4));
    Prefab::Node root;
    root.key = ObjectKey{1};
    root.mesh = mesh;
    const auto base = std::make_shared<const Prefab>(std::vector<Prefab::Node>{root});
    PrefabVariant::Renderer renderer;
    renderer.mesh = mesh;
    renderer.placements = placements;
    const PrefabVariant variant("base", {{ObjectKey{1}, {}, {}, {}, renderer, {}, {}}});
    const auto resolved = variant.resolve([&](std::string_view) { return base; }, {});
    CHECK(resolved.nodes()[0].placements == placements);

    const MeshName name = [](const std::shared_ptr<const Mesh> &) { return std::string("triangle"); };
    const auto read = PrefabVariant::deserialize(variant.serialize(name), [&](std::string_view) { return mesh; });
    const auto copied = read.resolve([&](std::string_view) { return base; }, {}).nodes()[0].placements;
    REQUIRE(copied);
    REQUIRE(copied->transforms().size() == placements->transforms().size());
    for (std::size_t i = 0; i < copied->transforms().size(); ++i)
        CHECK(same(copied->transforms()[i], placements->transforms()[i]));

    renderer.placements = MeshPlacements::create(triangle(), grid(1, 1));
    CHECK_THROWS_WITH_AS(PrefabVariant("base", {{ObjectKey{1}, {}, {}, {}, renderer, {}, {}}}),
                         "Prefab variant placements must copy the renderer's mesh, which has no pose",
                         std::invalid_argument);
    PrefabVariant::Renderer empty;
    empty.placements = placements;
    CHECK_THROWS_WITH_AS(PrefabVariant("base", {{ObjectKey{1}, {}, {}, {}, empty, {}, {}}}),
                         "Empty prefab variant renderer has state", std::invalid_argument);
}
