#include <anima/assets/scene_validation.hpp>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr auto draw_range = "MeshSnapshot draw exceeds vertex range";
constexpr auto partial_triangles = "MeshSnapshot draw must contain complete triangles";
constexpr auto invalid_vertex = "Invalid scene vertex";
constexpr auto nonfinite_bounds = "Non-finite scene bounds";
constexpr auto material_factors = "Invalid material factors";
constexpr auto texture_dimensions = "Invalid scene texture dimensions";
constexpr auto invalid_sampler = "Invalid scene sampler";

anima::MeshSnapshot fixture() {
    anima::MeshSnapshot scene;
    scene.vertices.resize(6);
    scene.primitives.resize(2);
    scene.primitives[0].vertex_count = scene.primitives[1].vertex_count = 3;
    scene.primitives[1].first_vertex = 3;
    scene.primitives[0].material_index = 0;
    scene.primitives[1].material_index = 1;
    scene.material_data = {{"a", {1, 1, 1}, 0}, {"b", {1, 1, 1}, 1}};
    scene.textures = {{2, 1, std::vector<std::uint8_t>(8, 255), {}}, {1, 1, std::vector<std::uint8_t>(4, 255), {}}};
    return scene;
}
// The fixture after @p edit.
template <class Edit> anima::MeshSnapshot edited(Edit edit) {
    auto scene = fixture();
    edit(scene);
    return scene;
}
} // namespace

TEST_CASE("Geometry over its byte budget is rejected with the requested and allowed sizes") {
    const auto bytes = fixture().vertices.size() * sizeof(anima::MeshVertex);
    CHECK_NOTHROW(anima::validate_scene(fixture(), {bytes}));
    const auto message = "MeshSnapshot geometry needs " + std::to_string(bytes) + " bytes; budget is " +
                         std::to_string(bytes - 1) + " bytes";
    REQUIRE_THROWS_WITH_AS(anima::validate_scene(fixture(), {bytes - 1}), message.c_str(), anima::SceneCapacityError);
    try {
        anima::validate_scene(fixture(), {bytes - 1});
    } catch (const anima::SceneCapacityError &error) {
        CHECK(error.requested_bytes == bytes);
        CHECK(error.budget_bytes == bytes - 1);
    }
    CHECK_NOTHROW(anima::validate_scene({}, {0}));
    CHECK_NOTHROW(anima::validate_scene({}));
}

TEST_CASE("Geometry that overflows draw addressing is rejected without allocating") {
    constexpr auto most = std::numeric_limits<std::size_t>::max();
    CHECK_THROWS_WITH_AS(anima::validate_scene_geometry(most, {most}), "MeshSnapshot draw address overflow",
                         std::invalid_argument);
}

TEST_CASE("Draws must cover whole triangles inside the vertex array") {
    CHECK_THROWS_WITH_AS(anima::validate_scene(edited([](auto &s) { s.primitives[0].first_vertex = UINT32_MAX; })),
                         draw_range, std::invalid_argument);
    CHECK_THROWS_WITH_AS(anima::validate_scene(edited([](auto &s) { s.primitives[0].vertex_count = UINT32_MAX; })),
                         draw_range, std::invalid_argument);
    CHECK_THROWS_WITH_AS(anima::validate_scene(edited([](auto &s) { s.primitives[0].vertex_count = 4; })),
                         partial_triangles, std::invalid_argument);
    CHECK_THROWS_WITH_AS(anima::validate_scene(edited([](auto &s) { s.primitives[0].vertex_count = 0; })),
                         partial_triangles, std::invalid_argument);
    CHECK_THROWS_WITH_AS(anima::validate_scene(edited([](auto &s) { s.primitives[0].first_vertex = 6; })), draw_range,
                         std::invalid_argument);
}

TEST_CASE("Material and texture references must name existing entries") {
    for (int index : {-2, 2, std::numeric_limits<int>::max()}) {
        CAPTURE(index);
        CHECK_THROWS_WITH_AS(anima::validate_scene(edited([=](auto &s) { s.primitives[0].material_index = index; })),
                             "MeshSnapshot draw references an invalid material", std::invalid_argument);
        CHECK_THROWS_WITH_AS(anima::validate_scene(edited([=](auto &s) { s.material_data[0].texture = index; })),
                             "Invalid material texture reference", std::invalid_argument);
    }
}

TEST_CASE("Nonfinite vertices, node transforms and bounds are rejected") {
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    CHECK_THROWS_WITH_AS(anima::validate_scene(edited([=](auto &s) { s.vertices[0].position.x = nan; })),
                         invalid_vertex, std::invalid_argument);
    CHECK_THROWS_WITH_AS(anima::validate_scene(edited([=](auto &s) { s.vertices[0].normal.y = nan; })), invalid_vertex,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(anima::validate_scene(edited([=](auto &s) { s.vertices[0].color.z = nan; })), invalid_vertex,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(anima::validate_scene(edited([=](auto &s) { s.vertices[0].uv[1] = nan; })), invalid_vertex,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(anima::validate_scene(edited([=](auto &s) { s.primitives[0].node_world[0] = nan; })),
                         "Non-finite scene node transform", std::invalid_argument);
    CHECK_THROWS_WITH_AS(anima::validate_scene(edited([=](auto &s) { s.minimum.x = nan; })), nonfinite_bounds,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(anima::validate_scene(edited([=](auto &s) { s.maximum.y = nan; })), nonfinite_bounds,
                         std::invalid_argument);
}

TEST_CASE("Material factors outside [0, 1] are rejected") {
    for (float value : {std::numeric_limits<float>::quiet_NaN(), -0.1F, 1.1F, std::numeric_limits<float>::infinity()}) {
        CAPTURE(value);
        CHECK_THROWS_WITH_AS(anima::validate_scene(edited([=](auto &s) { s.material_data[0].factor.x = value; })),
                             material_factors, std::invalid_argument);
        CHECK_THROWS_WITH_AS(anima::validate_scene(edited([=](auto &s) { s.material_data[0].metallic = value; })),
                             material_factors, std::invalid_argument);
        CHECK_THROWS_WITH_AS(anima::validate_scene(edited([=](auto &s) { s.material_data[0].roughness = value; })),
                             material_factors, std::invalid_argument);
    }
}

TEST_CASE("Textures must match their dimensions and use known sampler values") {
    CHECK_THROWS_WITH_AS(anima::validate_scene(edited([](auto &s) { s.textures[0].width = 0; })), texture_dimensions,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(anima::validate_scene(edited([](auto &s) { s.textures[0].height = 0; })), texture_dimensions,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        anima::validate_scene(edited([](auto &s) { s.textures[0].width = s.textures[0].height = UINT32_MAX; })),
        texture_dimensions, std::invalid_argument);
    CHECK_THROWS_WITH_AS(anima::validate_scene(edited([](auto &s) { s.textures[0].rgba.pop_back(); })),
                         "MeshSnapshot texture byte count does not match dimensions", std::invalid_argument);
    const auto unknown_filter = static_cast<anima::Filter>(99);
    const auto unknown_wrap = static_cast<anima::Wrap>(99);
    CHECK_THROWS_WITH_AS(anima::validate_scene(edited([=](auto &s) { s.textures[0].sampler.mag = unknown_filter; })),
                         invalid_sampler, std::invalid_argument);
    CHECK_THROWS_WITH_AS(anima::validate_scene(edited([=](auto &s) { s.textures[0].sampler.min = unknown_filter; })),
                         invalid_sampler, std::invalid_argument);
    CHECK_THROWS_WITH_AS(anima::validate_scene(edited([=](auto &s) { s.textures[0].sampler.mip = unknown_filter; })),
                         invalid_sampler, std::invalid_argument);
    CHECK_THROWS_WITH_AS(anima::validate_scene(edited([=](auto &s) { s.textures[0].sampler.u = unknown_wrap; })),
                         invalid_sampler, std::invalid_argument);
    CHECK_THROWS_WITH_AS(anima::validate_scene(edited([=](auto &s) { s.textures[0].sampler.v = unknown_wrap; })),
                         invalid_sampler, std::invalid_argument);
}

TEST_CASE("Posed, colored and hidden geometry and the default material and texture are accepted") {
    auto scene = fixture();
    scene.vertices[0].position.x += 1;
    scene.vertices[0].normal = {0, 1, 0};
    scene.vertices[0].color = {.2F, .3F, .4F};
    scene.vertices[0].uv = {.5F, .7F};
    scene.primitives[0].visible = false;
    scene.primitives[0].node_world = anima::identity();
    scene.material_data[0].factor = {.2F, .3F, .4F};
    scene.material_data[0].metallic = .7F;
    scene.material_data[0].roughness = .15F;
    CHECK_NOTHROW(anima::validate_scene(scene));
    scene.primitives[0].material_index = -1;
    scene.material_data[0].texture = -1;
    CHECK_NOTHROW(anima::validate_scene(scene));
}
