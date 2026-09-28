// This suite supplies its own doctest main, which takes an optional exported GLB as its first argument.
#define DOCTEST_CONFIG_IMPLEMENT
#include "near.hpp"
#include <anima/prefab.hpp>
#include <anima/scene.hpp>
#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace {
// anima::Mat4 is a std::array, so argument-dependent lookup does not find its product.
using anima::operator*;
constexpr float tolerance = 1e-4F; // Snapshot vertices and material factors against their independent sources.
// Runs only when the first argument names an exported model, as CTest's exported_instances passes it.
constexpr auto exported_case = "Snapshots of an exported model match its independent deformation";
std::string exported_model;
anima::Mat4 translation(float x) {
    auto m = anima::identity();
    m[12] = x;
    return m;
}
// A scale of (@p x, @p y, @p z); a negative one mirrors and a zero one collapses its axis.
anima::Mat4 scaling(float x, float y, float z) {
    auto m = anima::identity();
    m[0] = x;
    m[5] = y;
    m[10] = z;
    return m;
}
bool same_point(anima::Vec3 a, anima::Vec3 b) { return anima::length(a - b) < tolerance; }
// Whether every triangle of @p snapshot winds counterclockwise when seen from the side its first corner's normal
// faces, as the fixtures' triangles do before posing.
bool winds_with_normals(const anima::MeshSnapshot &snapshot) {
    for (std::size_t i = 0; i + 2 < snapshot.vertices.size(); i += 3) {
        const auto &a = snapshot.vertices[i], &b = snapshot.vertices[i + 1], &c = snapshot.vertices[i + 2];
        if (anima::dot(anima::cross(b.position - a.position, c.position - a.position), a.normal) <= 0)
            return false;
    }
    return true;
}
// Whether @p actual holds @p expected's corners in the same order.
bool same_corners(const anima::MeshSnapshot &actual, const anima::MeshSnapshot &expected) {
    if (actual.vertices.size() != expected.vertices.size())
        return false;
    for (std::size_t i = 0; i < actual.vertices.size(); ++i)
        if (!same_point(actual.vertices[i].position, expected.vertices[i].position))
            return false;
    return true;
}
std::shared_ptr<anima::Asset> asset() {
    auto source = std::make_shared<anima::Asset>();
    source->nodes.resize(3);
    source->nodes[0].name = "root";
    source->nodes[1].name = "joint";
    source->nodes[1].parent = 0;
    source->nodes[1].rest.translation = {0, 1, 0};
    source->nodes[2].name = "mesh";
    source->nodes[2].rest.translation = {99, 0, 0};
    auto bind = anima::identity();
    bind[13] = -1;
    source->skins.push_back({{0, 1}, {anima::identity(), bind}});
    source->textures = {{std::make_shared<anima::Image>(anima::Image{1, 1, {255, 0, 0, 255}}), {}},
                        {std::make_shared<anima::Image>(anima::Image{1, 1, {0, 255, 0, 255}}), {}}};
    source->materials = {{"hair", {0, 0, 0}, 1}, {"clothes", {.5F, 1, .25F}, -1}, {"iris", {1, .25F, .5F}, 0}};
    source->materials[1].metallic = .8F;
    source->materials[1].roughness = .27F;
    for (int material : {0, 1, -1, 2}) {
        anima::SourcePrimitive p;
        p.node = 2;
        p.skin = 0;
        p.material = material;
        for (auto position : {anima::Vec3{0, 2, 0}, anima::Vec3{1, 2, 0}, anima::Vec3{0, 3, 0}}) {
            anima::SourceVertex v;
            v.position = position;
            v.normal = {0, 0, 1};
            v.color = {.5F, .75F, 1};
            v.joints = {1, 0, 0, 0};
            v.weights = {1, 0, 0, 0};
            v.uv = {.25F, .75F};
            p.vertices.push_back(v);
        }
        source->primitives.push_back(p);
    }
    anima::Animation clip;
    clip.name = "turn";
    clip.duration = 1;
    clip.channels.push_back(
        {0, anima::ChannelPath::translation, anima::Interpolation::linear, {0, 1}, {{0, 0, 0, 0}, {2, 0, 0, 0}}});
    clip.channels.push_back(
        {1, anima::ChannelPath::rotation, anima::Interpolation::linear, {0, 1}, {{0, 0, 0, 1}, {0, 0, 1, 0}}});
    source->animations.push_back(clip);
    return source;
}
// Checks snapshots of two instances of @p source against its independent CPU deformation.
void snapshots(const std::shared_ptr<const anima::Asset> &source) {
    const auto compiled = anima::Mesh::compile(*source);
    anima::Scene instances;
    const auto a = instances.add(compiled), b = instances.add(compiled);
    const auto reference = anima::make_mesh_snapshot(*source, anima::sample_pose(*source));
    const auto count = reference.vertices.size();
    const auto initial = instances.snapshot();
    REQUIRE(initial.vertices.size() == count * 2);
    REQUIRE(initial.primitives.size() == source->primitives.size() * 2);
    REQUIRE(initial.material_data.size() == source->materials.size() * 2);
    CHECK(initial.textures.size() == source->textures.size() * 2);
    // The second instance's materials refer to its own copy of the textures.
    for (std::size_t i = 0; i < source->materials.size(); ++i) {
        CAPTURE(i);
        const auto texture = source->materials[i].texture;
        CHECK(initial.material_data[source->materials.size() + i].texture ==
              (texture < 0 ? -1 : texture + int(source->textures.size())));
    }
    auto world = translation(10);
    auto pose = anima::sample_pose(*source);
    if (!source->animations.empty())
        pose = anima::sample_pose(*source, &source->animations.front(), .5);
    instances.set_pose(a, pose, world);
    auto expected = reference;
    anima::pose_mesh_snapshot(*source, pose, expected, 0, world);
    const auto posed = instances.snapshot();
    REQUIRE(posed.vertices.size() == initial.vertices.size());
    for (std::size_t i = 0; i < count; ++i) {
        CAPTURE(i);
        CHECK(anima::length(posed.vertices[i].position - expected.vertices[i].position) < tolerance);
        CHECK(anima::length(posed.vertices[i].normal - expected.vertices[i].normal) < tolerance);
        CHECK(posed.vertices[i].uv == expected.vertices[i].uv);
        // Posing one instance leaves the other at rest.
        CHECK(anima::length(posed.vertices[count + i].position - reference.vertices[i].position) < tolerance);
    }
    const auto bytes = posed.vertices.size() * sizeof(anima::MeshVertex);
    CHECK(instances.snapshot({bytes}).vertices.size() == count * 2);
    const auto accepted = instances.instance(a).palette;
    const auto budget = "MeshSnapshot geometry needs " + std::to_string(bytes) + " bytes; budget is " +
                        std::to_string(bytes - 1) + " bytes";
    CHECK_THROWS_WITH_AS(instances.snapshot({bytes - 1}), budget.c_str(), anima::SceneCapacityError);
    CHECK(instances.instance(a).palette == accepted);
    instances.set_visible(a, false);
    instances.set_primitive_visible(b, 0, false);
    const auto hidden = instances.snapshot();
    CHECK_FALSE(hidden.primitives.front().visible);
    CHECK_FALSE(hidden.primitives[source->primitives.size()].visible);
    instances.set_visible(a, true);
    if (!source->materials.empty()) {
        instances.set_material_factor(a, 0, {.2F, .4F, .8F});
        const auto colored = instances.snapshot();
        CHECK(colored.material_data[0].factor.x == Near{.2F, tolerance});
        // The other instance keeps the source factor.
        CHECK(colored.material_data[source->materials.size()].factor.x ==
              Near{source->materials[0].factor.x, tolerance});
        for (std::size_t p = 0; p < source->primitives.size(); ++p)
            if (source->primitives[p].material == 0) {
                CAPTURE(p);
                // Vertex colors are multiplied by the override.
                const auto offset = colored.primitives[p].first_vertex;
                CHECK(colored.vertices[offset].color.x ==
                      Near{source->primitives[p].vertices[0].color.x * .2F, tolerance});
            }
        instances.clear_material_factor(a, 0);
        CHECK(instances.snapshot().material_data[0].factor.x == Near{source->materials[0].factor.x, tolerance});
    }
    instances.remove(a);
    instances.remove(b);
    CHECK(instances.snapshot({0}).vertices.empty()); // A zero budget admits an empty scene.
    CHECK(initial.vertices.size() == count * 2);     // The first snapshot owns its storage.
}
} // namespace

TEST_CASE("Instance snapshots match the independent deformation and keep instances apart") { snapshots(asset()); }

TEST_CASE("Snapshots keep seams, skin influences, colors and rigid transforms") {
    auto source = asset();
    for (auto &primitive : source->primitives) {
        const auto triangle = primitive.vertices;
        for (int copy = 0; copy < 8; ++copy)
            primitive.vertices.insert(primitive.vertices.end(), triangle.begin(), triangle.end());
        primitive.vertices[3].normal = {1, 0, 0};
        primitive.vertices[6].uv = {.9F, .1F};
        primitive.vertices[9].color = {.1F, .2F, .3F};
        primitive.vertices[12].weights = {.3F, .7F, 0, 0};
        primitive.vertices[15].joints = {0, 1, 0, 0};
    }
    auto rigid = source->primitives.back();
    rigid.skin = -1;
    source->primitives.push_back(rigid);
    anima::Scene instances;
    const auto id = instances.add(anima::Mesh::compile(*source));
    auto reference = anima::make_mesh_snapshot(*source, anima::sample_pose(*source));
    for (double t : {.0, .17, .31, .8, 1.}) {
        CAPTURE(t);
        const auto pose = anima::sample_pose(*source, &source->animations[0], t);
        anima::Transform transform;
        transform.translation = {2, float(t), -1};
        transform.rotation = {0, std::sin(float(t)), 0, std::cos(float(t))};
        transform.scale = {1, 1.1F, .9F};
        const auto world = anima::matrix(transform);
        anima::pose_mesh_snapshot(*source, pose, reference, 0, world);
        instances.set_pose(id, pose, world);
        const auto actual = instances.snapshot();
        REQUIRE(actual.vertices.size() == reference.vertices.size());
        for (std::size_t i = 0; i < reference.vertices.size(); ++i) {
            CAPTURE(i);
            const auto &a = actual.vertices[i], &b = reference.vertices[i];
            CHECK(anima::length(a.position - b.position) < tolerance);
            CHECK(anima::length(a.normal - b.normal) < tolerance);
            CHECK(anima::length(a.color - b.color) < tolerance);
            CHECK(a.uv == b.uv);
        }
    }
}

TEST_CASE("Snapshots keep each triangle's source winding against its normals under a mirroring transform") {
    auto source = asset();
    auto rigid = source->primitives.front();
    rigid.skin = -1;
    source->primitives.push_back(rigid);
    const auto rest = anima::sample_pose(*source);
    const auto unmirrored = anima::make_mesh_snapshot(*source, rest);
    REQUIRE(winds_with_normals(unmirrored));

    // Mirroring every node mirrors the joints that skin the fixture and the rigid primitive's node.
    auto mirrored_pose = rest;
    for (auto &world : mirrored_pose.world)
        world = scaling(-1, 1, 1) * world;
    const auto mirrored = anima::make_mesh_snapshot(*source, mirrored_pose);
    CHECK(winds_with_normals(mirrored));
    REQUIRE(mirrored.vertices.size() == unmirrored.vertices.size());
    // Each triangle keeps its first corner and swaps its last two.
    const auto reflect = [](anima::Vec3 v) { return anima::Vec3{-v.x, v.y, v.z}; };
    for (std::size_t i = 0; i < mirrored.vertices.size(); i += 3) {
        CAPTURE(i);
        CHECK(same_point(mirrored.vertices[i].position, reflect(unmirrored.vertices[i].position)));
        CHECK(same_point(mirrored.vertices[i + 1].position, reflect(unmirrored.vertices[i + 2].position)));
        CHECK(same_point(mirrored.vertices[i + 2].position, reflect(unmirrored.vertices[i + 1].position)));
    }

    // A mirroring attachment and a mirrored instance follow the same rule.
    auto attached = unmirrored;
    anima::pose_mesh_snapshot(*source, rest, attached, 0, scaling(-1, 1, 1));
    CHECK(same_corners(attached, mirrored));
    anima::Scene scene;
    const auto id = scene.add(anima::Mesh::compile(*source));
    scene.set_pose(id, rest, scaling(-1, 1, 1));
    CHECK(same_corners(scene.snapshot(), mirrored));

    // A collapsed axis has a zero determinant and keeps the source order.
    auto flattened = unmirrored;
    anima::pose_mesh_snapshot(*source, rest, flattened, 0, scaling(1, 1, 0));
    for (std::size_t i = 0; i < flattened.vertices.size(); ++i) {
        CAPTURE(i);
        const auto source_position = unmirrored.vertices[i].position;
        CHECK(same_point(flattened.vertices[i].position, {source_position.x, source_position.y, 0}));
    }
}

TEST_CASE("A renderer casts shadows until it is told not to, and a new mesh restores casting") {
    const auto mesh = anima::Mesh::compile(*asset());
    anima::Scene scene;
    const auto id = scene.add(mesh);
    CHECK(scene.instance(id).casts_shadows);
    scene.set_casts_shadows(id, false);
    CHECK_FALSE(scene.instance(id).casts_shadows);
    // Hiding and showing the renderer leaves the setting alone.
    scene.set_visible(id, false);
    scene.set_visible(id, true);
    CHECK_FALSE(scene.instance(id).casts_shadows);
    auto renderer = scene.object(id).renderer();
    renderer.set_casts_shadows(true);
    CHECK(scene.instance(id).casts_shadows);
    renderer.set_casts_shadows(false);
    renderer.set_mesh(mesh);
    CHECK(scene.instance(id).casts_shadows);
    const auto empty = scene.create("empty");
    CHECK_THROWS_WITH_AS(scene.set_casts_shadows(empty.id(), false), "GameObject has no MeshRenderer",
                         std::logic_error);
}

TEST_CASE("Shadow casting persists through scene documents and prefabs") {
    const auto mesh = anima::Mesh::compile(*asset());
    anima::Scene scene;
    auto caster = scene.create("caster", mesh);
    caster.renderer().set_casts_shadows(false);
    const anima::MeshName name = [](const std::shared_ptr<const anima::Mesh> &) { return std::string("mesh"); };
    const anima::MeshResolver resolve = [&](std::string_view) { return mesh; };
    const auto document = anima::serialize_scene(scene, name);
    CHECK(document.find(R"("casts_shadows": false)") != std::string::npos);
    const auto loaded = anima::load_scene(document, resolve);
    CHECK_FALSE(loaded->instance(loaded->roots().front().id()).casts_shadows);
    const auto prefab = anima::Prefab::capture(caster);
    CHECK_FALSE(prefab.nodes().front().casts_shadows);
    anima::Scene destination;
    const auto copy = prefab.instantiate(destination);
    CHECK_FALSE(destination.instance(copy.id()).casts_shadows);
    // Without a mesh there is no renderer to stop casting.
    anima::Prefab::Node empty;
    empty.casts_shadows = false;
    CHECK_THROWS_WITH_AS(anima::Prefab({empty}), "Empty scene object has renderer state", std::invalid_argument);
}

TEST_CASE("A skinned triangle whose corners blend to opposite determinant signs follows its first corner") {
    anima::Asset source;
    source.nodes.resize(3);
    source.skins.push_back({{0, 1}, {anima::identity(), anima::identity()}});
    anima::SourcePrimitive primitive;
    primitive.node = 2;
    primitive.skin = 0;
    const auto corner = [](anima::Vec3 position, std::uint32_t joint) {
        anima::SourceVertex vertex;
        vertex.position = position;
        vertex.normal = {0, 0, 1};
        vertex.joints = {joint, 0, 0, 0};
        vertex.weights = {1, 0, 0, 0};
        return vertex;
    };
    // Joint 0 mirrors and joint 1 does not. The first triangle starts on joint 0, the second on joint 1.
    primitive.vertices = {corner({0, 0, 0}, 0), corner({1, 0, 0}, 1), corner({0, 1, 0}, 1),
                          corner({0, 0, 0}, 1), corner({1, 0, 0}, 0), corner({0, 1, 0}, 0)};
    source.primitives.push_back(primitive);
    auto pose = anima::sample_pose(source);
    pose.world[0] = scaling(-1, 1, 1);
    const auto snapshot = anima::make_mesh_snapshot(source, pose);
    REQUIRE(snapshot.vertices.size() == 6);
    // The first triangle swaps its last two corners, which joint 1 leaves in place.
    CHECK(same_point(snapshot.vertices[1].position, {0, 1, 0}));
    CHECK(same_point(snapshot.vertices[2].position, {1, 0, 0}));
    // The second keeps its order; joint 0 mirrors its last two corners.
    CHECK(same_point(snapshot.vertices[4].position, {-1, 0, 0}));
    CHECK(same_point(snapshot.vertices[5].position, {0, 1, 0}));
}

TEST_CASE(exported_case) { snapshots(anima::load_asset(exported_model)); }

int main(int argc, char **argv) {
    doctest::Context context(argc, argv);
    // doctest ignores the model path, which precedes any doctest option.
    if (argc > 1 && argv[1][0] != '-')
        exported_model = argv[1];
    else
        context.addFilter("test-case-exclude", exported_case);
    return context.run();
}
