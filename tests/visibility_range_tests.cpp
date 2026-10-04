// Visibility ranges: their validation, the point they measure to, how scenes keep and reset them, and their persistence
// in scene, prefab and prefab variant documents. GPU checks cover how the renderer draws, dissolves and culls them.
#include <anima/mesh_placements.hpp>
#include <anima/prefab.hpp>
#include <anima/prefab_variant.hpp>
#include <anima/scene.hpp>
#include <cmath>
#include <doctest/doctest.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace anima;

namespace {
constexpr auto invalid_range = "A visibility range requires 0 <= begin < end and margins that fit between them";
constexpr auto endless_margin = "An endless visibility range has no end margin";
constexpr float infinity = std::numeric_limits<float>::infinity(),
                not_a_number = std::numeric_limits<float>::quiet_NaN();

std::shared_ptr<const Mesh> triangle() {
    Asset asset;
    asset.nodes.resize(1);
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
// The scene document of one object rendering the mesh named "triangle" with @p range, the JSON text of its field.
std::string document(const std::string &range) {
    return R"({"version":4,"kind":"anima.scene","next_key":"2","objects":[{"key":"1","name":"rock","parent":null,)"
           R"("local":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"mesh":"triangle","pose":null,"visible":true,"active":true,)"
           R"("material_factors":[],"custom_materials":[],"primitive_visible":[],"casts_shadows":true,)"
           R"("placements":null,"components":[],"visibility_range":)" +
           range + "}]}";
}
// Three small triangles near the ends of the unit axes, each its own draw, under a node that the rest pose places at
// @p rest.
std::shared_ptr<const Mesh> corner_triangles(const Transform &rest = {}) {
    Asset asset;
    asset.nodes.resize(1);
    asset.nodes[0].rest = rest;
    asset.materials.push_back({"surface", {1, 1, 1}, -1});
    for (const auto axis : {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}}) {
        SourcePrimitive primitive;
        primitive.material = 0;
        for (const auto offset : {Vec3{}, Vec3{.01F, 0, 0}, Vec3{0, .01F, 0}}) {
            SourceVertex vertex;
            vertex.position = axis + offset;
            vertex.normal = {0, 0, 1};
            primitive.vertices.push_back(vertex);
        }
        asset.primitives.push_back(std::move(primitive));
    }
    return Mesh::compile(asset);
}
} // namespace

TEST_CASE("Ranges measure to the center of the mesh's rest bounds, which placement clusters hold") {
    // The rest pose places the bounds: scaled by 2, then moved 5 along X.
    Transform rest;
    rest.translation = {5, 0, 0};
    rest.scale = {2, 2, 2};
    const auto moved = corner_triangles(rest);
    const auto &placed = moved->rest_bounds();
    REQUIRE(placed.valid);
    CHECK(placed.minimum.x == doctest::Approx(5));
    CHECK(placed.maximum.x == doctest::Approx(7.02));
    CHECK(placed.maximum.y == doctest::Approx(2.02));
    CHECK(placed.maximum.z == doctest::Approx(2));

    // Turning (1, 1, 1) onto +X leaves every triangle within 0.59 of the copy's plane x = 0, while the center of the
    // rest bounds, (0.505, 0.505, 0.5), lands at x = 0.87. The cluster must hold it anyway, or culling the cluster by
    // its bounds could hide a copy whose range still draws it.
    const auto mesh = corner_triangles();
    const float a = 1 / std::sqrt(3.F), b = 1 / std::sqrt(2.F), c = 1 / std::sqrt(6.F);
    const Mat4 turn{a, b, c, 0, a, -b, c, 0, a, 0, -2 * c, 0, 0, 0, 0, 1};
    const auto center = point(turn, (mesh->rest_bounds().minimum + mesh->rest_bounds().maximum) * .5F);
    const auto placements = MeshPlacements::create(mesh, std::vector<Mat4>{turn});
    for (const auto &bounds : placements->primitive_bounds())
        CHECK(bounds.maximum.x < center.x - .25F);
    const auto &cluster = placements->clusters()[0].bounds;
    CHECK(cluster.minimum.x <= center.x);
    CHECK(center.x <= cluster.maximum.x);
    CHECK(cluster.minimum.y <= center.y);
    CHECK(center.y <= cluster.maximum.y);
    CHECK(cluster.minimum.z <= center.z);
    CHECK(center.z <= cluster.maximum.z);
}

TEST_CASE("Visibility ranges outside the documented rules are rejected") {
    CHECK_NOTHROW(validate_visibility_range({}));
    CHECK_NOTHROW(validate_visibility_range({0, 10, 0, 10}));
    CHECK_NOTHROW(validate_visibility_range({5, infinity, 3, 0}));
    for (const VisibilityRange &range :
         {VisibilityRange{-1, 10, 0, 0}, VisibilityRange{not_a_number, 10, 0, 0},
          VisibilityRange{infinity, infinity, 0, 0}, VisibilityRange{10, 10, 0, 0}, VisibilityRange{10, 5, 0, 0},
          VisibilityRange{0, not_a_number, 0, 0}, VisibilityRange{0, 10, -1, 0},
          VisibilityRange{0, 10, 0, not_a_number}, VisibilityRange{0, 10, 6, 5}, VisibilityRange{0, 10, 0, infinity},
          VisibilityRange{0, infinity, 0, infinity}})
        CHECK_THROWS_WITH_AS(validate_visibility_range(range), invalid_range, std::invalid_argument);
    // No distance reaches an infinite end, so an end margin there could never dissolve anything.
    for (const VisibilityRange &range : {VisibilityRange{0, infinity, 0, 5}, VisibilityRange{5, infinity, 3, 1e-30F}})
        CHECK_THROWS_WITH_AS(validate_visibility_range(range), endless_margin, std::invalid_argument);
}

TEST_CASE("Renderers keep their visibility range until their mesh changes") {
    Scene scene;
    auto object = scene.create("rock", triangle());
    auto renderer = object.renderer();
    CHECK(renderer.visibility_range() == VisibilityRange{});
    CHECK(renderer.visibility_range().end == infinity);
    const VisibilityRange range{2, 80, 1, 10};
    renderer.set_visibility_range(range);
    CHECK(scene.instance(object.id()).visibility_range == range);
    CHECK_THROWS_WITH_AS(renderer.set_visibility_range({0, 10, 6, 5}), invalid_range, std::invalid_argument);
    CHECK_THROWS_WITH_AS(renderer.set_visibility_range({0, infinity, 0, 5}), endless_margin, std::invalid_argument);
    CHECK(renderer.visibility_range() == range);
    renderer.set_mesh(triangle());
    CHECK(renderer.visibility_range() == VisibilityRange{});
    object.remove_mesh();
    CHECK_THROWS_WITH_AS(renderer.set_visibility_range(range), "GameObject has no MeshRenderer", std::logic_error);
}

TEST_CASE("Scene documents and prefabs keep visibility ranges") {
    const auto mesh = triangle();
    const MeshName name = [](const std::shared_ptr<const Mesh> &) { return std::string("triangle"); };
    const MeshResolver resolve = [&](std::string_view) { return mesh; };
    Scene scene;
    auto near = scene.create("near", mesh), far = scene.create("far", mesh), always = scene.create("always", mesh);
    near.renderer().set_visibility_range({0, 60, 0, 8});
    far.renderer().set_visibility_range({40, infinity, 5, 0});
    const auto text = serialize_scene(scene, name);
    CHECK(text.find("\"visibility_range\": null") != std::string::npos);
    const auto loaded = load_scene(text, resolve);
    std::vector<VisibilityRange> ranges;
    for (const auto id : loaded->instances())
        ranges.push_back(loaded->instance(id).visibility_range);
    CHECK(ranges == std::vector<VisibilityRange>{{0, 60, 0, 8}, {40, infinity, 5, 0}, {}});

    const auto prefab = Prefab::capture(near);
    CHECK(prefab.nodes()[0].renderer.visibility_range == VisibilityRange{0, 60, 0, 8});
    Scene target;
    CHECK(prefab.instantiate(target).renderer().visibility_range() == VisibilityRange{0, 60, 0, 8});
}

TEST_CASE("Documents reject malformed visibility ranges") {
    const auto mesh = triangle();
    const MeshResolver resolve = [&](std::string_view) { return mesh; };
    const auto loaded = load_scene(document(R"({"begin":1,"end":null,"begin_margin":0.5,"end_margin":0})"), resolve);
    CHECK(loaded->instance(loaded->instances()[0]).visibility_range == VisibilityRange{1, infinity, .5F, 0});
    CHECK_THROWS_WITH_AS((void)load_scene(document(R"({"begin":10,"end":5,"begin_margin":0,"end_margin":0})"), resolve),
                         invalid_range, std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        (void)load_scene(document(R"({"begin":1,"end":null,"begin_margin":0.5,"end_margin":2})"), resolve),
        endless_margin, std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)load_scene(document("[0, 10]"), resolve), "Invalid scene visibility range",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        (void)load_scene(document(R"({"begin":"0","end":10,"begin_margin":0,"end_margin":0})"), resolve),
        "Invalid scene visibility range", std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)load_scene(document(R"({"begin":0,"end":10,"begin_margin":0})"), resolve),
                         "Missing JSON field: end_margin", std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        (void)load_scene(document(R"({"begin":0,"end":10,"begin_margin":0,"end_margin":0,"fade":1})"), resolve),
        "Unknown JSON field: fade", std::invalid_argument);

    Prefab::Node empty;
    empty.renderer.visibility_range = {0, 10, 0, 0};
    CHECK_THROWS_WITH_AS(Prefab(std::vector<Prefab::Node>{empty}), "Empty scene object has renderer state",
                         std::invalid_argument);
    Prefab::Node invalid;
    invalid.renderer.mesh = mesh;
    invalid.renderer.visibility_range = {0, 10, 6, 5};
    CHECK_THROWS_WITH_AS(Prefab(std::vector<Prefab::Node>{invalid}), invalid_range, std::invalid_argument);
    invalid.renderer.visibility_range = {0, infinity, 0, 5};
    CHECK_THROWS_WITH_AS(Prefab(std::vector<Prefab::Node>{invalid}), endless_margin, std::invalid_argument);
}

TEST_CASE("Prefab variants replace visibility ranges with the rest of a renderer") {
    const auto mesh = triangle();
    Prefab::Node root;
    root.key = ObjectKey{1};
    root.renderer.mesh = mesh;
    const auto base = std::make_shared<const Prefab>(std::vector<Prefab::Node>{root});
    RendererState renderer;
    renderer.mesh = mesh;
    renderer.visibility_range = {3, 120, 2, 20};
    const PrefabVariant variant("base", {{ObjectKey{1}, {}, {}, {}, renderer, {}, {}}});
    CHECK(variant.resolve([&](std::string_view) { return base; }, {}).nodes()[0].renderer.visibility_range ==
          VisibilityRange{3, 120, 2, 20});
    const MeshName name = [](const std::shared_ptr<const Mesh> &) { return std::string("triangle"); };
    const auto read = PrefabVariant::deserialize(variant.serialize(name), [&](std::string_view) { return mesh; });
    CHECK(read.resolve([&](std::string_view) { return base; }, {}).nodes()[0].renderer.visibility_range ==
          VisibilityRange{3, 120, 2, 20});

    RendererState empty;
    empty.visibility_range = {0, 10, 0, 0};
    CHECK_THROWS_WITH_AS(PrefabVariant("base", {{ObjectKey{1}, {}, {}, {}, empty, {}, {}}}),
                         "Empty prefab variant renderer has state", std::invalid_argument);
    renderer.visibility_range = {10, 5, 0, 0};
    CHECK_THROWS_WITH_AS(PrefabVariant("base", {{ObjectKey{1}, {}, {}, {}, renderer, {}, {}}}), invalid_range,
                         std::invalid_argument);
    renderer.visibility_range = {0, infinity, 0, 5};
    CHECK_THROWS_WITH_AS(PrefabVariant("base", {{ObjectKey{1}, {}, {}, {}, renderer, {}, {}}}), endless_margin,
                         std::invalid_argument);

    // A variant document's field follows the rules of scene documents, under the variant's own message.
    RendererState plain;
    plain.mesh = mesh;
    const auto text = PrefabVariant("base", {{ObjectKey{1}, {}, {}, {}, plain, {}, {}}}).serialize(name);
    const auto with_range = [&](const std::string &range) {
        auto result = text;
        const std::string field = "\"visibility_range\": null";
        const auto at = result.find(field);
        REQUIRE(at != std::string::npos);
        return result.replace(at, field.size(), "\"visibility_range\": " + range);
    };
    const MeshResolver resolve = [&](std::string_view) { return mesh; };
    CHECK(PrefabVariant::deserialize(with_range(R"({"begin":3,"end":120,"begin_margin":2,"end_margin":20})"), resolve)
              .overrides()[0]
              .renderer->visibility_range == VisibilityRange{3, 120, 2, 20});
    for (const std::string malformed : {"[0, 10]", R"({"begin":"0","end":10,"begin_margin":0,"end_margin":0})"})
        CHECK_THROWS_WITH_AS((void)PrefabVariant::deserialize(with_range(malformed), resolve),
                             "Invalid prefab variant visibility range", std::invalid_argument);
    CHECK_THROWS_WITH_AS((void)PrefabVariant::deserialize(
                             with_range(R"({"begin":0,"end":null,"begin_margin":0,"end_margin":5})"), resolve),
                         endless_margin, std::invalid_argument);
}
