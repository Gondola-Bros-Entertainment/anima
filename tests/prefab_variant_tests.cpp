#include "consumer/prefab_variant.hpp"
#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace anima;
namespace {
constexpr auto marker_component = R"({"type":"test.marker.v1","state":"ok","enabled":true})";
constexpr auto empty_renderer =
    R"({"mesh":null,"pose":null,"visible":true,"material_factors":[],"custom_materials":[],)"
    R"("primitive_visible":[],"casts_shadows":true,"placements":null,"visibility_range":null})";
constexpr auto missing_version = "Missing JSON field: version";
constexpr auto unsupported_version = "Unsupported prefab variant document version";
constexpr auto duplicate_field = "Duplicate JSON document field";
constexpr auto invalid_key = "Invalid prefab variant resource or component key";
constexpr auto duplicate_object = "Null or duplicate prefab variant object key";
constexpr auto invalid_object_key = "Invalid object key";
constexpr auto component_count = "Invalid prefab variant component count";
constexpr auto duplicate_component = "Duplicate prefab variant component override";
constexpr auto conflicting_removal = "Duplicate or conflicting prefab variant component removal";
constexpr auto renderer_state = "Empty prefab variant renderer has state";
constexpr auto pose_mismatch = "Prefab variant pose does not match the mesh";
constexpr auto nonfinite_transform = "Non-finite instance transform";
constexpr auto not_string = "[json.exception.type_error.302] type must be string, but is number";
constexpr auto not_boolean = "[json.exception.type_error.302] type must be boolean, but is number";

std::string substitute(std::string value, std::string_view from, std::string_view to) {
    const auto at = value.find(from);
    REQUIRE(at != std::string::npos);
    value.replace(at, from.size(), to);
    return value;
}
std::string change() {
    return "{\"key\":\"41\",\"name\":\"override\",\"local\":null,\"active\":null,\"renderer\":null,"
           "\"set_components\":[],\"remove_components\":[]}";
}
std::string envelope(const std::string &overrides) {
    return "{\"version\":2,\"kind\":\"anima.prefab-variant\",\"base\":\"base\",\"overrides\":[" + overrides + "]}";
}
PrefabVariant decode(const std::string &document) { return PrefabVariant::deserialize(document, {}); }
} // namespace

TEST_CASE("Variants resolve typed overrides over their base, keep references per instance and roll back failures") {
    prefab_variant_test::run();
}

TEST_CASE("A canonical variant document decodes its override, a component, an empty renderer and a wide key") {
    const auto valid = envelope(change());
    const auto decoded = decode(valid);
    CHECK(decoded.base_key() == "base");
    REQUIRE(decoded.overrides().size() == 1);
    CHECK(decoded.overrides()[0].key.value == 41);
    CHECK(decoded.overrides()[0].name == "override");
    // Codecs are required only when resolving the base, and meshes only when a renderer names one.
    CHECK_NOTHROW(decode(
        substitute(valid, "\"set_components\":[]", std::string("\"set_components\":[") + marker_component + "]")));
    CHECK_NOTHROW(decode(substitute(valid, "\"renderer\":null", std::string("\"renderer\":") + empty_renderer)));
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto widest = decode(substitute(valid, "\"key\":\"41\"", "\"key\":\"" + std::to_string(maximum) + "\""));
    REQUIRE(widest.overrides().size() == 1);
    CHECK(widest.overrides()[0].key.value == maximum);
}

TEST_CASE("Malformed variant documents are rejected with their reason") {
    const auto valid = envelope(change());
    CHECK_THROWS_WITH_AS(decode("{}"), missing_version, std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"version\":2", "\"version\":1")), unsupported_version,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"version\":2", "\"version\":2.0")), unsupported_version,
                         std::invalid_argument);
    // A removed field does not hide another version, nor does an override that omits a field.
    CHECK_THROWS_WITH_AS(
        decode(substitute(substitute(valid, "\"version\":2", "\"version\":3"), "{", "{\"removed\":0,")),
        unsupported_version, std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        decode(substitute(substitute(valid, "\"version\":2", "\"version\":1"), "\"renderer\":null,", "")),
        unsupported_version, std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"version\":2,", "")), missing_version, std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "anima.prefab-variant", "anima.prefab")),
                         "Invalid prefab variant document kind", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "{", "{\"unexpected\":null,")), "Unknown JSON field: unexpected",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "{", "{\"version\":2,")), duplicate_field, std::invalid_argument);
    // A key that decodes to one already present is a duplicate.
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "{", "{\"ver\\u0073ion\":2,")), duplicate_field,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"base\":\"base\"", "\"base\":\"\"")), invalid_key,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"base\":\"base\"", "\"base\":null")),
                         "[json.exception.type_error.302] type must be string, but is null", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"base\":\"base\",", "")), "Missing JSON field: base",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"base\":\"base\"", "\"base\":\"" + std::string(4097, 'x') + "\"")),
                         invalid_key, std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(envelope(change() + "," + change())), duplicate_object, std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"key\":\"41\"", "\"key\":\"0\"")), duplicate_object,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"key\":\"41\"", "\"key\":\"041\"")), invalid_object_key,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"key\":\"41\"", "\"key\":\"-1\"")), invalid_object_key,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"key\":\"41\"", "\"key\":41")), not_string, std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"key\":\"41\"", "\"key\":\"18446744073709551616\"")),
                         invalid_object_key, std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"key\":\"41\",", "")), "Missing JSON field: key",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"key\":\"41\"", "\"key\":\"41\",\"k\\u0065y\":\"42\"")),
                         duplicate_field, std::invalid_argument);
    // Without its name, the override changes nothing.
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"name\":\"override\"", "\"name\":null")),
                         "Empty prefab variant override", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"name\":\"override\"", "\"name\":42")), not_string,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"local\":null", "\"local\":[]")),
                         "Prefab variant matrix requires 16 scalars", std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        decode(substitute(valid, "\"local\":null", "\"local\":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1e1000]")),
        "[json.exception.out_of_range.406] number overflow parsing '1e1000'", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"active\":null", "\"active\":1")), not_boolean,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"renderer\":null", "\"renderer\":{}")), "Missing JSON field: mesh",
                         std::invalid_argument);
    // Every field of an override is required: null, not omission, inherits from the base.
    for (const auto &[field, missing] :
         {std::pair{"\"name\":\"override\",", "Missing JSON field: name"},
          std::pair{"\"local\":null,", "Missing JSON field: local"},
          std::pair{"\"active\":null,", "Missing JSON field: active"},
          std::pair{"\"renderer\":null,", "Missing JSON field: renderer"},
          std::pair{"\"set_components\":[],", "Missing JSON field: set_components"},
          std::pair{",\"remove_components\":[]", "Missing JSON field: remove_components"}}) {
        CAPTURE(missing);
        CHECK_THROWS_WITH_AS(decode(substitute(valid, field, "")), missing, std::invalid_argument);
    }
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"renderer\":null", "\"renderer\":null,\"parent\":null")),
                         "Unknown JSON field: parent", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"set_components\":[]", "\"set_components\":null")), component_count,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"set_components\":[]", "\"set_components\":[{}]")),
                         "Missing JSON field: type", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"remove_components\":[]", "\"remove_components\":[\"\"]")),
                         invalid_key, std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        decode(substitute(valid, "\"remove_components\":[]", "\"remove_components\":[\"type\",\"type\"]")),
        conflicting_removal, std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"remove_components\":[]", "\"remove_components\":[true]")),
                         "[json.exception.type_error.302] type must be string, but is boolean", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(valid, "\"name\":\"override\"",
                                           "\"name\":" + std::string(32, '[') + "0" + std::string(32, ']'))),
                         "JSON document exceeds nesting limit", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(std::string(16 * 1024 * 1024, ' ') + valid), "JSON document exceeds byte limit",
                         std::invalid_argument);
    std::string too_many;
    too_many.reserve(3 * 65537);
    for (std::size_t i = 0; i < 65537; ++i) {
        if (i)
            too_many += ',';
        too_many += "{}";
    }
    CHECK_THROWS_WITH_AS(decode(envelope(too_many)), "Invalid prefab variant override count", std::invalid_argument);
}

TEST_CASE("Malformed component overrides are rejected with their reason") {
    const auto valid = envelope(change());
    const auto with_component =
        substitute(valid, "\"set_components\":[]", std::string("\"set_components\":[") + marker_component + "]");
    CHECK_THROWS_WITH_AS(decode(substitute(with_component, "\"state\":\"ok\",", "")), "Missing JSON field: state",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(with_component, "\"state\":\"ok\"", "\"state\":42")), not_string,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(with_component, "\"enabled\":true", "\"enabled\":1")), not_boolean,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(with_component, "\"enabled\":true", "\"enabled\":true,\"extra\":null")),
                         "Unknown JSON field: extra", std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        decode(substitute(valid, "\"set_components\":[]",
                          std::string("\"set_components\":[") + marker_component + "," + marker_component + "]")),
        duplicate_component, std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        decode(substitute(with_component, "\"remove_components\":[]", "\"remove_components\":[\"test.marker.v1\"]")),
        conflicting_removal, std::invalid_argument);
}

TEST_CASE("Malformed renderer overrides are rejected with their reason") {
    const auto with_renderer =
        substitute(envelope(change()), "\"renderer\":null", std::string("\"renderer\":") + empty_renderer);
    CHECK_THROWS_WITH_AS(decode(substitute(with_renderer, "\"visible\":true", "\"visible\":false")), renderer_state,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(with_renderer, "\"pose\":null", "\"pose\":[]")), pose_mismatch,
                         std::invalid_argument);
    // Without a mesh there is no renderer to stop casting.
    CHECK_THROWS_WITH_AS(decode(substitute(with_renderer, "\"casts_shadows\":true", "\"casts_shadows\":false")),
                         renderer_state, std::invalid_argument);
    // Every field of a renderer is required, as for a scene object.
    for (const auto &[field, missing] :
         {std::pair{"\"mesh\":null,", "Missing JSON field: mesh"},
          std::pair{"\"pose\":null,", "Missing JSON field: pose"},
          std::pair{"\"visible\":true,", "Missing JSON field: visible"},
          std::pair{"\"material_factors\":[],", "Missing JSON field: material_factors"},
          std::pair{"\"custom_materials\":[],", "Missing JSON field: custom_materials"},
          std::pair{"\"primitive_visible\":[],", "Missing JSON field: primitive_visible"},
          std::pair{"\"casts_shadows\":true,", "Missing JSON field: casts_shadows"},
          std::pair{"\"placements\":null,", "Missing JSON field: placements"},
          std::pair{",\"visibility_range\":null", "Missing JSON field: visibility_range"}}) {
        CAPTURE(missing);
        CHECK_THROWS_WITH_AS(decode(substitute(with_renderer, field, "")), missing, std::invalid_argument);
    }
    CHECK_THROWS_WITH_AS(decode(substitute(with_renderer, "\"primitive_visible\":[]", "\"primitive_visible\":[false]")),
                         renderer_state, std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(with_renderer, "\"material_factors\":[]", "\"material_factors\":[[1,1,1]]")),
                         renderer_state, std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(substitute(with_renderer, "\"mesh\":null", "\"mesh\":null,\"unknown\":0")),
                         "Unknown JSON field: unknown", std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        decode(substitute(with_renderer, "\"placements\":null", "\"placements\":[[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]]")),
        "Prefab variant placements must copy the renderer's mesh, which has no pose", std::invalid_argument);
}

TEST_CASE("A renderer override holds the RendererState of a prefab node, which both readers decode alike") {
    const auto mesh = prefab_variant_test::mesh();
    Prefab::Node root;
    root.key = {41};
    auto &state = root.renderer;
    state.mesh = mesh;
    state.pose.emplace().world = mesh->rest_pose().world;
    state.visible = false;
    state.material_factors = {{0.25F, 0.5F, 1}};
    state.custom_materials = {nullptr};
    state.primitive_visible = {false};
    state.casts_shadows = false;
    state.visibility_range = {3, 120, 2, 20};
    const auto base = std::make_shared<const Prefab>(std::vector{root});
    const auto &authored = base->nodes()[0].renderer;
    REQUIRE(authored == state);
    PrefabVariant::Override change;
    change.key = root.key;
    change.renderer = authored;
    const PrefabVariant variant("base", {change});
    CHECK(variant.resolve([&](std::string_view) { return base; }, {}).nodes()[0].renderer == authored);

    // The prefab and variant documents write the same renderer fields, and reading them back with the same mesh
    // gives equal states.
    const MeshName name = [](const std::shared_ptr<const Mesh> &) { return std::string("triangle"); };
    const MeshResolver resolve = [&](std::string_view) { return mesh; };
    CHECK(Prefab::deserialize(base->serialize(name), resolve).nodes()[0].renderer == authored);
    const auto read = PrefabVariant::deserialize(variant.serialize(name), resolve);
    REQUIRE(read.overrides()[0].renderer);
    CHECK(*read.overrides()[0].renderer == authored);

    // A changed setting or pose matrix makes the states differ, and so does another mesh, which compares by address.
    auto other = authored;
    other.casts_shadows = true;
    CHECK_FALSE(other == authored);
    other = authored;
    other.pose->world[0][12] = 1;
    CHECK_FALSE(other == authored);
    other = authored;
    other.mesh = prefab_variant_test::mesh();
    CHECK_FALSE(other == authored);
    CHECK(RendererState{} == RendererState{});
}

TEST_CASE("Invalid programmatic variants are rejected with their reason") {
    PrefabVariant::Override value;
    value.key = {41};
    value.name = "valid";
    CHECK_THROWS_WITH_AS(PrefabVariant("", {value}), invalid_key, std::invalid_argument);
    CHECK_THROWS_WITH_AS(PrefabVariant(std::string(4097, 'b'), {value}), invalid_key, std::invalid_argument);
    CHECK_THROWS_WITH_AS(PrefabVariant("base", {value, value}), duplicate_object, std::invalid_argument);
    auto bad = value;
    bad.key = {};
    CHECK_THROWS_WITH_AS(PrefabVariant("base", {bad}), duplicate_object, std::invalid_argument);
    bad = value;
    bad.name.reset();
    CHECK_THROWS_WITH_AS(PrefabVariant("base", {bad}), "Empty prefab variant override", std::invalid_argument);
    bad = value;
    bad.local = identity();
    (*bad.local)[12] = std::numeric_limits<float>::quiet_NaN();
    CHECK_THROWS_WITH_AS(PrefabVariant("base", {bad}), nonfinite_transform, std::invalid_argument);
    bad = value;
    bad.set_components = {{"type", "a", true}, {"type", "b", false}};
    CHECK_THROWS_WITH_AS(PrefabVariant("base", {bad}), duplicate_component, std::invalid_argument);
    bad.set_components.resize(1);
    bad.remove_components = {"type"};
    CHECK_THROWS_WITH_AS(PrefabVariant("base", {bad}), conflicting_removal, std::invalid_argument);
    bad = value;
    bad.set_components = {{"", "state", true}};
    CHECK_THROWS_WITH_AS(PrefabVariant("base", {bad}), invalid_key, std::invalid_argument);
    bad.set_components[0].type.assign(4097, 't');
    CHECK_THROWS_WITH_AS(PrefabVariant("base", {bad}), invalid_key, std::invalid_argument);
    bad = value;
    bad.set_components.resize(1025);
    for (std::size_t i = 0; i < bad.set_components.size(); ++i)
        bad.set_components[i].type = "type-" + std::to_string(i);
    CHECK_THROWS_WITH_AS(PrefabVariant("base", {bad}), component_count, std::invalid_argument);
    bad = value;
    bad.remove_components.resize(1025);
    for (std::size_t i = 0; i < bad.remove_components.size(); ++i)
        bad.remove_components[i] = "type-" + std::to_string(i);
    CHECK_THROWS_WITH_AS(PrefabVariant("base", {bad}), component_count, std::invalid_argument);
    bad = value;
    bad.renderer.emplace();
    bad.renderer->visible = false;
    CHECK_THROWS_WITH_AS(PrefabVariant("base", {bad}), renderer_state, std::invalid_argument);
    bad.renderer->mesh = prefab_variant_test::mesh();
    bad.renderer->pose.emplace();
    CHECK_THROWS_WITH_AS(PrefabVariant("base", {bad}), pose_mismatch, std::invalid_argument);
    bad.renderer->pose = bad.renderer->mesh->rest_pose();
    bad.renderer->pose->world[0][12] = std::numeric_limits<float>::quiet_NaN();
    CHECK_THROWS_WITH_AS(PrefabVariant("base", {bad}), nonfinite_transform, std::invalid_argument);
    bad.renderer->pose->world[0] = identity();
    bad.renderer->pose->world[0][15] = 2;
    CHECK_THROWS_WITH_AS(PrefabVariant("base", {bad}), "Instance transform must be affine", std::invalid_argument);
    bad.renderer->pose.reset();
    bad.renderer->material_factors = {{1, 1, 1}, {1, 1, 1}};
    CHECK_THROWS_WITH_AS(PrefabVariant("base", {bad}), "Prefab variant material factors do not match the mesh",
                         std::invalid_argument);
    bad = value;
    bad.set_components = {{"type", std::string(16 * 1024 * 1024, 's'), true}};
    CHECK_THROWS_WITH_AS(PrefabVariant("base", {bad}).serialize({}), "Prefab variant document exceeds the byte limit",
                         std::invalid_argument);
}
