#include <anima/scene.hpp>
#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
constexpr float tolerance = 1e-4F; // Reference positions, normals and colors, and bounds margins.
constexpr auto stale_handle = "Stale or foreign GameObject handle";
constexpr auto skin_influence = "Invalid render skin influence";
bool near(anima::Vec3 a, anima::Vec3 b) { return anima::length(a - b) < tolerance; }
// Whether @p point lies in @p bounds, within the tolerance.
bool contains(const anima::RenderBounds &bounds, anima::Vec3 point) {
    return point.x >= bounds.minimum.x - tolerance && point.y >= bounds.minimum.y - tolerance &&
           point.z >= bounds.minimum.z - tolerance && point.x <= bounds.maximum.x + tolerance &&
           point.y <= bounds.maximum.y + tolerance && point.z <= bounds.maximum.z + tolerance;
}
// Whether @p outer is valid and reaches at least as far as @p inner on every side.
bool encloses(const anima::RenderBounds &outer, const anima::RenderBounds &inner) {
    return outer.valid && outer.minimum.x <= inner.minimum.x && outer.minimum.y <= inner.minimum.y &&
           outer.minimum.z <= inner.minimum.z && outer.maximum.x >= inner.maximum.x &&
           outer.maximum.y >= inner.maximum.y && outer.maximum.z >= inner.maximum.z;
}
anima::Asset fixture() {
    anima::Asset asset;
    asset.nodes.resize(2);
    asset.skins.push_back({{0, 1}, {anima::identity(), anima::identity()}});
    asset.materials.push_back({"surface", {.3F, .6F, .9F}, -1});
    anima::SourcePrimitive p;
    p.skin = 0;
    p.material = 0;
    for (const auto position : {anima::Vec3{0, 0, 0}, anima::Vec3{1, 0, 0}, anima::Vec3{0, 1, 0}}) {
        anima::SourceVertex v;
        v.position = position;
        v.normal = anima::normalized({.2F, .4F, 1});
        v.color = {.4F, .5F, .6F};
        v.joints = {0, 1, 0, 0};
        v.weights = {.25F, .75F, 0, 0};
        v.uv = {position.x, position.y};
        p.vertices.push_back(v);
    }
    const auto copy = p.vertices;
    p.vertices.insert(p.vertices.end(), copy.begin(), copy.end());
    asset.primitives.push_back(p);
    p.skin = anima::no_index;
    p.node = 1;
    asset.primitives.push_back(p);
    return asset;
}
// The fixture after @p edit.
template <class Edit> anima::Asset edited(Edit edit) {
    auto asset = fixture();
    edit(asset);
    return asset;
}
// Scale and translation shared by the instance's world transform and its second node's pose.
anima::Transform placement() {
    anima::Transform transform;
    transform.scale = {.7F, 1.2F, 1.7F};
    transform.translation = {2, 1, -3};
    return transform;
}
anima::Mat4 instance_world() { return anima::matrix(placement()); }
// The fixture's first node moved @p time along X, and its second placed and turned @p time radians about Z.
anima::Pose pose_at(const anima::Asset &source, float time) {
    auto pose = anima::sample_pose(source);
    pose.world[0] = anima::identity();
    pose.world[0][12] = time;
    auto transform = placement();
    transform.rotation = {0, 0, std::sin(time * .5F), std::cos(time * .5F)};
    pose.world[1] = anima::matrix(transform);
    return pose;
}
// Checks every drawn corner of @p instance against the independent CPU deformation of @p source.
void parity(const anima::Asset &source, const anima::Scene::Instance &instance, const anima::Pose &pose,
            const anima::Mat4 &world) {
    auto reference = anima::make_mesh_snapshot(source, pose);
    anima::pose_mesh_snapshot(source, pose, reference, 0, world);
    std::size_t corner = 0, primitive = 0;
    for (const auto &draw : instance.asset->draws()) {
        CAPTURE(primitive);
        for (std::size_t i = draw.first_index; i < draw.first_index + draw.index_count; ++i, ++corner) {
            CAPTURE(corner);
            const auto &vertex = instance.asset->vertices()[instance.asset->indices()[i]];
            auto matrix = instance.palette[draw.palette_offset];
            if (draw.skinned) {
                matrix = {};
                for (unsigned j = 0; j < 4; ++j)
                    if (vertex.weights[j] != 0)
                        for (unsigned k = 0; k < 16; ++k)
                            matrix[k] +=
                                instance.palette[draw.palette_offset + vertex.joints[j]][k] * vertex.weights[j];
            }
            const auto position = anima::point(matrix, vertex.position);
            const auto &expected = reference.vertices[corner];
            const auto factor = instance.factors[draw.material];
            CHECK(near(position, expected.position));
            CHECK(near(anima::normal(matrix, vertex.normal), expected.normal));
            CHECK(near({vertex.color.x * factor.x, vertex.color.y * factor.y, vertex.color.z * factor.z},
                       expected.color));
            CHECK(vertex.uv == expected.uv);
            // Animated influence bounds hold the corner, and the instance's broad-phase bounds hold those.
            CHECK(contains(instance.primitive_bounds[primitive], position));
            CHECK(encloses(instance.bounds, instance.primitive_bounds[primitive]));
        }
        ++primitive;
    }
}
} // namespace

TEST_CASE("Posed instances share one indexed mesh and match the reference deformation") {
    const auto source = fixture();
    const auto asset = anima::Mesh::compile(source);
    CHECK(asset->indices().size() == 12);
    CHECK(asset->vertices().size() == 6); // Exact duplicates share one vertex.
    anima::Scene scene;
    const auto a = scene.add(asset), b = scene.add(asset);
    CHECK(scene.instance(a).asset == scene.instance(b).asset);
    for (float time : {0.F, .2F, .7F, 1.F}) {
        CAPTURE(time);
        const auto pose = pose_at(source, time);
        scene.set_pose(a, pose, instance_world());
        parity(source, scene.instance(a), pose, instance_world());
        parity(source, scene.instance(b), asset->rest_pose(), anima::identity());
    }
}

TEST_CASE("A rejected pose or snapshot budget leaves the accepted palette") {
    const auto source = fixture();
    const auto asset = anima::Mesh::compile(source);
    anima::Scene scene;
    const auto a = scene.add(asset);
    (void)scene.add(asset);
    const auto pose = pose_at(source, 1);
    scene.set_pose(a, pose, instance_world());
    const auto accepted = scene.instance(a).palette;
    auto invalid = pose;
    invalid.world.pop_back();
    CHECK_THROWS_WITH_AS(scene.set_pose(a, invalid), "Pose does not match render asset", std::invalid_argument);
    invalid = pose;
    invalid.world[0][0] = std::numeric_limits<float>::quiet_NaN();
    CHECK_THROWS_WITH_AS(scene.set_pose(a, invalid), "Non-finite instance transform", std::invalid_argument);
    CHECK(scene.instance(a).palette == accepted);
    const auto snapshot = scene.snapshot();
    CHECK(snapshot.vertices.size() == 24);
    CHECK(snapshot.primitives.size() == 4);
    const auto bytes = snapshot.vertices.size() * sizeof(anima::MeshVertex);
    const auto budget = "MeshSnapshot geometry needs " + std::to_string(bytes) + " bytes; budget is " +
                        std::to_string(bytes - 1) + " bytes";
    CHECK_THROWS_WITH_AS(scene.snapshot({bytes - 1}), budget.c_str(), anima::SceneCapacityError);
    CHECK(scene.instance(a).palette == accepted);
}

TEST_CASE("Material and visibility edits stay with their instance") {
    const auto asset = anima::Mesh::compile(fixture());
    anima::Scene scene;
    const auto a = scene.add(asset), b = scene.add(asset);
    scene.set_material_factor(a, 0, {1, 0, 0});
    CHECK(scene.instance(b).factors[0].y == .6F);
    CHECK(asset->materials()->material_data[0].factor.y == .6F);
    CHECK_THROWS_WITH_AS(scene.set_material_factor(a, 0, {2, 0, 0}), "Invalid render material factor",
                         std::invalid_argument);
    scene.clear_material_factor(a, 0);
    scene.set_primitive_visible(a, 0, false);
    CHECK(scene.instance(b).primitive_visible[0]);
}

TEST_CASE("Stale and foreign handles are rejected, and a reused slot has a new generation") {
    const auto asset = anima::Mesh::compile(fixture());
    anima::Scene scene, other;
    const auto a = scene.add(asset);
    (void)scene.add(asset); // Occupies the next slot, so the reused one is a's.
    CHECK_THROWS_WITH_AS(other.set_visible(a, false), stale_handle, std::out_of_range);
    scene.remove(a);
    CHECK_THROWS_WITH_AS(scene.set_visible(a, false), stale_handle, std::out_of_range);
    const auto c = scene.add(asset);
    CHECK(c.slot == a.slot);
    CHECK(c.generation != a.generation);
    CHECK_THROWS_WITH_AS(scene.remove(a), stale_handle, std::out_of_range);
}

TEST_CASE("Compiled meshes do not depend on mutable source storage") {
    auto source = fixture();
    const auto asset = anima::Mesh::compile(source);
    source.primitives[0].vertices[0].position = {99, 99, 99};
    source.materials[0].factor = {0, 0, 0};
    CHECK(asset->vertices()[0].position.x == 0);
    CHECK(asset->materials()->material_data[0].factor.y == .6F);
}

TEST_CASE("Indexing keeps UV seams and different skin influences apart") {
    auto seam = fixture();
    seam.primitives[0].vertices[3].uv[0] = .5F;
    CHECK(anima::Mesh::compile(seam)->vertices().size() == 7);
    seam = fixture();
    seam.primitives[0].vertices[3].weights = {.5F, .5F, 0, 0};
    CHECK(anima::Mesh::compile(seam)->vertices().size() == 7);
}

TEST_CASE("Rounded skin weights far from the origin keep reference parity and bounds") {
    auto rounded = fixture();
    for (auto &v : rounded.primitives[0].vertices)
        v.weights = {.25008F, .75F, 0, 0};
    const auto asset = anima::Mesh::compile(rounded);
    anima::Scene scene;
    const auto id = scene.add(asset);
    auto distant = anima::identity();
    distant[12] = 100000;
    scene.set_pose(id, asset->rest_pose(), distant);
    parity(rounded, scene.instance(id), asset->rest_pose(), distant);
}

TEST_CASE("Invalid skins, primitives and influences are rejected when compiling") {
    using anima::Mesh;
    CHECK_THROWS_WITH_AS(Mesh::compile(edited([](auto &s) { s.primitives[0].vertices[0].weights[0] = -1; })),
                         skin_influence, std::invalid_argument);
    CHECK_THROWS_WITH_AS(Mesh::compile(edited([](auto &s) { s.primitives[0].vertices[0].joints[0] = 2; })),
                         skin_influence, std::invalid_argument);
    CHECK_THROWS_WITH_AS(Mesh::compile(edited([](auto &s) { s.primitives[0].vertices.pop_back(); })),
                         "Invalid render triangle count", std::invalid_argument);
    CHECK_THROWS_WITH_AS(Mesh::compile(edited([](auto &s) { s.primitives[0].skin = 3; })),
                         "Invalid render primitive skin", std::invalid_argument);
    CHECK_THROWS_WITH_AS(Mesh::compile(edited([](auto &s) { s.primitives[0].node = 3; })),
                         "Invalid render primitive node", std::invalid_argument);
    CHECK_THROWS_WITH_AS(Mesh::compile(edited([](auto &s) { s.primitives[0].material = 3; })),
                         "Invalid render primitive material", std::invalid_argument);
    CHECK_THROWS_WITH_AS(Mesh::compile(edited([](auto &s) { s.skins[0].inverse_bind.pop_back(); })),
                         "Invalid render skin palette", std::invalid_argument);
}

TEST_CASE("An empty scene has no bounds or instances") {
    const anima::Scene empty;
    CHECK_FALSE(empty.bounds().valid);
    CHECK(empty.instances().empty());
}
