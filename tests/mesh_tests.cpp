#include <anima/assets/material_textures.hpp>
#include <anima/mesh.hpp>
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using namespace anima;
namespace {
constexpr std::size_t triangle_corners = 3;
constexpr std::uint32_t rgba_channels = 4;
constexpr std::uint32_t authored_edge = 4; // Texture edge in the fixtures.
constexpr unsigned reduced_edge = 2;       // A texture limit that halves the fixtures' textures.
constexpr std::uint8_t opaque = 255;
// material_texture_plan binding order: base color, normal, metallic-roughness, emissive, occlusion.
constexpr std::size_t base_color_binding = 0, emissive_binding = 3;
// Alpha-test cutoffs and alpha factors of the coverage fixture's materials.
constexpr float strict_cutoff = .75F, loose_cutoff = .25F, half_alpha = .5F;

SourcePrimitive triangle(int material) {
    SourcePrimitive primitive;
    primitive.material = material;
    for (const auto corner : {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}}) {
        SourceVertex vertex;
        vertex.position = corner;
        vertex.normal = {0, 0, 1};
        primitive.vertices.push_back(vertex);
    }
    return primitive;
}

// One node with one triangle per entry of @p materials, in order, each using that material. Material 0
// samples a texture larger than reduced_edge, so a texture limit has work to do.
Asset static_source(std::initializer_list<int> materials) {
    Asset source;
    source.nodes.resize(1);
    source.materials.resize(2);
    source.materials[0].name = "textured";
    source.materials[0].texture = 0;
    source.materials[1].name = "plain";
    source.textures.push_back({authored_edge,
                               authored_edge,
                               std::vector<std::uint8_t>(authored_edge * authored_edge * rgba_channels, opaque),
                               {}});
    for (const auto material : materials)
        source.primitives.push_back(triangle(material));
    return source;
}

constexpr std::uint32_t cutout_edge = 8;
constexpr unsigned cutout_limit = cutout_edge / 2; // Shrinks the cutout to its first mip level.
constexpr std::size_t cutout_level = 1;
// An 8x8 texture whose 2x2 blocks hold 0 to 4 opaque texels, with magenta transparent texels: box filtering
// loses coverage at every cutoff, and alpha-weighted color differs from plain color.
Texture cutout() {
    constexpr std::uint32_t block_edge = 2, blocks_per_row = cutout_edge / block_edge;
    constexpr std::array<unsigned, blocks_per_row * blocks_per_row> opaque_per_block{0, 0, 0, 0, 1, 1, 1, 1,
                                                                                     1, 1, 1, 2, 2, 3, 4, 4};
    constexpr std::uint8_t transparent = 0;
    Texture texture{cutout_edge, cutout_edge, std::vector<std::uint8_t>(cutout_edge * cutout_edge * rgba_channels), {}};
    for (std::uint32_t block = 0; block < opaque_per_block.size(); ++block)
        for (std::uint32_t texel = 0; texel < block_edge * block_edge; ++texel) {
            const auto x = block % blocks_per_row * block_edge + texel % block_edge;
            const auto y = block / blocks_per_row * block_edge + texel / block_edge;
            const auto offset = (y * cutout_edge + x) * rgba_channels;
            const auto visible = texel < opaque_per_block[block] ? opaque : transparent;
            texture.rgba[offset] = texture.rgba[offset + 2] = opaque;
            texture.rgba[offset + 1] = texture.rgba[offset + 3] = visible;
        }
    return texture;
}
Material masked(const char *name, float cutoff, float alpha = 1) {
    Material material;
    material.name = name;
    material.texture = 0;
    material.alpha_mode = AlphaMode::mask;
    material.alpha_cutoff = cutoff;
    material.alpha = alpha;
    return material;
}
// Materials sharing the cutout as base color: texture-space cutoffs of 0.75, 0.25 and 0.5, a zero cutoff that
// keeps every fragment, one above the alpha factor that discards every fragment, and an opaque material that
// also uses the cutout for emission. One triangle each.
Asset coverage_source() {
    Asset source;
    source.nodes.resize(1);
    source.textures.push_back(cutout());
    Material opaque_emissive;
    opaque_emissive.name = "opaque";
    opaque_emissive.texture = opaque_emissive.emissive_texture = 0;
    source.materials = {masked("strict", strict_cutoff),
                        masked("loose", loose_cutoff),
                        masked("scaled", loose_cutoff, half_alpha),
                        masked("zero cutoff", 0),
                        masked("unreachable", strict_cutoff, half_alpha),
                        opaque_emissive};
    for (std::size_t i = 0; i < source.materials.size(); ++i)
        source.primitives.push_back(triangle(static_cast<int>(i)));
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

TEST_CASE("Shrunk textures keep the alpha coverage that each material's upload preserves") {
    // Cutoffs 0.75, 0.25 and 0.5, and plain filtering for the uses whose visibility ignores texture alpha.
    constexpr std::size_t coverage_variants = 4;
    const auto source = coverage_source();
    const auto plan = material_texture_plan(source.materials, source.textures);
    // The level that the upload of source material @p index's @p binding would build from the authored
    // texture; shrinking to cutout_limit must keep exactly that level.
    const auto planned = [&](std::size_t index, std::size_t binding) {
        const auto &image = plan.images.at(plan.bindings.at(index + 1)[binding]);
        return texture_mips(source.textures.at(image.source), image.mips).at(cutout_level);
    };
    // Compared for equality only: ordering byte vectors, as a std::set would, trips a false
    // -Wstringop-overread in GCC 12 to 14 at -O3.
    std::vector<std::vector<std::uint8_t>> variants;
    for (std::size_t i = 0; i < source.materials.size(); ++i) {
        auto level = planned(i, base_color_binding).rgba;
        if (std::ranges::find(variants, level) == variants.end())
            variants.push_back(std::move(level));
    }
    REQUIRE(variants.size() == coverage_variants); // Otherwise the checks below could not tell cutoffs apart.
    // Each pass checks every material's base color and each emissive map once.
    const auto emissive_uses = static_cast<std::size_t>(std::count_if(
        source.materials.begin(), source.materials.end(), [](const Material &m) { return m.emissive_texture >= 0; }));
    for (const auto options :
         {MeshCompileOptions{.max_texture_edge = cutout_limit},
          MeshCompileOptions{.max_vertices = triangle_corners, .max_texture_edge = cutout_limit}}) {
        CAPTURE(options.max_vertices);
        const auto meshes = Mesh::compile_static(source, options);
        std::size_t checked = 0;
        for (const auto &mesh : meshes) {
            const auto &snapshot = *mesh->materials();
            for (const auto &material : snapshot.material_data) {
                CAPTURE(material.name);
                const auto found =
                    std::find_if(source.materials.begin(), source.materials.end(),
                                 [&](const Material &authored) { return authored.name == material.name; });
                REQUIRE(found != source.materials.end());
                const auto index = static_cast<std::size_t>(found - source.materials.begin());
                const auto check = [&](int texture, std::size_t binding) {
                    CAPTURE(binding);
                    const auto &shrunk = snapshot.textures.at(texture);
                    const auto expected = planned(index, binding);
                    CHECK(shrunk.width == expected.width);
                    CHECK(shrunk.height == expected.height);
                    CHECK(shrunk.rgba == expected.rgba);
                    ++checked;
                };
                check(material.texture, base_color_binding);
                if (material.emissive_texture >= 0)
                    check(material.emissive_texture, emissive_binding);
            }
        }
        CHECK(checked == source.materials.size() + emissive_uses);
        if (!options.max_vertices) {
            REQUIRE(meshes.size() == 1);
            CHECK(meshes.front()->materials()->textures.size() == coverage_variants); // One copy per variant.
        } else
            CHECK(meshes.size() == source.primitives.size()); // The limit fits one triangle per piece.
    }
}

TEST_CASE("A texture within the limit keeps its texels once for every cutoff") {
    const auto source = coverage_source();
    const auto meshes = Mesh::compile_static(source, {.max_texture_edge = cutout_edge});
    REQUIRE(meshes.size() == 1);
    const auto &textures = meshes.front()->materials()->textures;
    REQUIRE(textures.size() == 1);
    CHECK(textures.front().rgba == source.textures.front().rgba);
}
