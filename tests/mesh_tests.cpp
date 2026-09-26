#include <anima/mesh.hpp>
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

using namespace anima;
namespace {
constexpr std::size_t triangle_corners = 3;
constexpr std::uint32_t authored_edge = 4; // Texture edge in the fixtures.
constexpr unsigned reduced_edge = 2;       // A texture limit that halves the fixtures' textures.
constexpr std::uint8_t opaque = 255;

// One node with one triangle per entry of @p materials, in order, each using that material. Material 0
// samples a texture larger than reduced_edge, so a texture limit has work to do.
Asset static_source(std::initializer_list<int> materials) {
    Asset source;
    source.nodes.resize(1);
    source.materials.resize(2);
    source.materials[0].name = "textured";
    source.materials[0].texture = 0;
    source.materials[1].name = "plain";
    source.textures.push_back(
        {authored_edge, authored_edge, std::vector<std::uint8_t>(authored_edge * authored_edge * 4, opaque), {}});
    for (const auto material : materials) {
        SourcePrimitive primitive;
        primitive.material = material;
        for (const auto corner : {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}}) {
            SourceVertex vertex;
            vertex.position = corner;
            vertex.normal = {0, 0, 1};
            primitive.vertices.push_back(vertex);
        }
        source.primitives.push_back(primitive);
    }
    return source;
}
} // namespace

TEST_CASE("A texture limit alone leaves static geometry unsplit") {
    const auto source = static_source({0, 1, 0});
    const auto meshes = Mesh::compile_static(source, {.max_texture_edge = reduced_edge});
    REQUIRE(meshes.size() == 1);
    const auto &mesh = *meshes.front();
    REQUIRE(mesh.draws().size() == source.primitives.size());
    CHECK(mesh.indices().size() == source.primitives.size() * triangle_corners);
    const auto &snapshot = *mesh.materials();
    for (std::size_t i = 0; i < source.primitives.size(); ++i) {
        const auto &material = snapshot.material_data.at(mesh.draws()[i].material);
        CHECK(material.name == source.materials.at(source.primitives[i].material).name);
        if (material.texture >= 0)
            CHECK(snapshot.textures.at(material.texture).width == reduced_edge);
    }
}

TEST_CASE("A vertex limit starts a new static mesh at each material change") {
    const auto source = static_source({0, 1, 0});
    // Room for every triangle, so only the material changes split the source.
    const auto meshes = Mesh::compile_static(source, {.max_vertices = source.primitives.size() * triangle_corners});
    REQUIRE(meshes.size() == source.primitives.size());
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        REQUIRE(meshes[i]->draws().size() == 1);
        CHECK(meshes[i]->materials()->material_data.at(0).name ==
              source.materials.at(source.primitives[i].material).name);
    }
}

TEST_CASE("A static source without primitives compiles into one empty mesh under any limits") {
    Asset source;
    source.nodes.resize(2);
    for (const auto options : {MeshCompileOptions{}, MeshCompileOptions{.max_vertices = triangle_corners},
                               MeshCompileOptions{.max_texture_edge = reduced_edge}}) {
        CAPTURE(options.max_vertices);
        CAPTURE(options.max_texture_edge);
        const auto meshes = Mesh::compile_static(source, options);
        REQUIRE(meshes.size() == 1);
        CHECK(meshes.front()->draws().empty());
        CHECK(meshes.front()->palette_size() == source.nodes.size());
    }
}
