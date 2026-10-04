#include <anima/assets/material_textures.hpp>
#include <anima/assets/mesh_preparation.hpp>
#include <anima/assets/scene_validation.hpp>
#include <anima/mesh.hpp>
#include <anima/scene.hpp>
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
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
constexpr float beyond_unit_factor = 2; // A base-color factor channel outside [0, 1].
// Mesh::compile() rejections that static splitting must reproduce.
constexpr auto invalid_material = "Invalid render primitive material";
constexpr auto invalid_texture_reference = "Invalid material texture reference";
constexpr auto invalid_factors = "Invalid material factors";
constexpr auto texture_byte_mismatch = "MeshSnapshot texture byte count does not match dimensions";
constexpr auto no_texels = "Texture image has no texels";
constexpr auto released_texels = "Mesh texture texels were released after upload";
constexpr auto unknown_retention = "Unknown texel retention";
constexpr auto invalid_parent = "Invalid node parent";
constexpr auto invalid_vertex = "Invalid render vertex";

// A texture over new pixels, with the default sampler and sRGB encoding.
Texture texture_of(Image image) { return {std::make_shared<Image>(std::move(image)), {}}; }

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
    source.textures.push_back(
        texture_of({authored_edge, authored_edge,
                    std::vector<std::uint8_t>(authored_edge * authored_edge * rgba_channels, opaque)}));
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
    Image image{cutout_edge, cutout_edge, std::vector<std::uint8_t>(cutout_edge * cutout_edge * rgba_channels)};
    for (std::uint32_t block = 0; block < opaque_per_block.size(); ++block)
        for (std::uint32_t texel = 0; texel < block_edge * block_edge; ++texel) {
            const auto x = block % blocks_per_row * block_edge + texel % block_edge;
            const auto y = block / blocks_per_row * block_edge + texel / block_edge;
            const auto offset = (y * cutout_edge + x) * rgba_channels;
            const auto visible = texel < opaque_per_block[block] ? opaque : transparent;
            image.rgba[offset] = image.rgba[offset + 2] = opaque;
            image.rgba[offset + 1] = image.rgba[offset + 3] = visible;
        }
    return texture_of(std::move(image));
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
// A snapshot of one object that draws @p mesh, as tools inspect it.
MeshSnapshot snapshot_of(std::shared_ptr<const Mesh> mesh) {
    Scene scene;
    (void)scene.add(std::move(mesh));
    return scene.snapshot();
}
// Both split paths of Mesh::compile_static must reject @p source exactly as Mesh::compile() does.
template <class Error> void rejects_like_compile(const Asset &source, const char *message) {
    CHECK_THROWS_WITH_AS(Mesh::compile(source), message, Error);
    for (const auto options :
         {MeshCompileOptions{.max_vertices = triangle_corners}, MeshCompileOptions{.max_texture_edge = reduced_edge}}) {
        CAPTURE(options.max_vertices);
        CAPTURE(options.max_texture_edge);
        CHECK_THROWS_WITH_AS(Mesh::compile_static(source, options), message, Error);
    }
}
} // namespace

TEST_CASE("A texture limit alone leaves static geometry unsplit") {
    const auto source = static_source({0, 1, 0});
    const auto meshes = Mesh::compile_static(source, {.max_texture_edge = reduced_edge});
    REQUIRE(meshes.size() == 1);
    const auto &mesh = *meshes.front();
    REQUIRE(mesh.draws().size() == source.primitives.size());
    CHECK(mesh.indices().size() == source.primitives.size() * triangle_corners);
    const auto &description = *mesh.description();
    for (std::size_t i = 0; i < source.primitives.size(); ++i) {
        const auto &material = description.materials.at(mesh.draws()[i].material);
        CHECK(material.name == source.materials.at(source.primitives[i].material).name);
        if (material.texture >= 0)
            CHECK(description.textures.at(material.texture).image->width == reduced_edge);
    }
}

TEST_CASE("A vertex limit starts a new static mesh at each material change") {
    const auto source = static_source({0, 1, 0});
    // Room for every triangle, so only the material changes split the source.
    const auto meshes = Mesh::compile_static(source, {.max_vertices = source.primitives.size() * triangle_corners});
    REQUIRE(meshes.size() == source.primitives.size());
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        REQUIRE(meshes[i]->draws().size() == 1);
        CHECK(meshes[i]->description()->materials.at(0).name ==
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
            const auto &description = *mesh->description();
            for (const auto &material : description.materials) {
                CAPTURE(material.name);
                const auto found =
                    std::find_if(source.materials.begin(), source.materials.end(),
                                 [&](const Material &authored) { return authored.name == material.name; });
                REQUIRE(found != source.materials.end());
                const auto index = static_cast<std::size_t>(found - source.materials.begin());
                const auto check = [&](int texture, std::size_t binding) {
                    CAPTURE(binding);
                    const auto &shrunk = *description.textures.at(texture).image;
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
            CHECK(meshes.front()->description()->textures.size() == coverage_variants); // One copy per variant.
        } else
            CHECK(meshes.size() == source.primitives.size()); // The limit fits one triangle per piece.
    }
}

TEST_CASE("A shrunk blended base color weights its color by alpha, as its upload's mips do") {
    Asset source;
    source.nodes.resize(1);
    source.textures.push_back(cutout());
    Material blended, plain;
    blended.name = "blended";
    plain.name = "plain";
    blended.texture = plain.texture = 0;
    blended.alpha_mode = AlphaMode::blend;
    source.materials = {blended, plain};
    source.primitives = {triangle(0), triangle(1)};
    const auto plan = material_texture_plan(source.materials, source.textures);
    const auto planned = [&](std::size_t index) {
        const auto &image = plan.images.at(plan.bindings.at(index + 1)[base_color_binding]);
        return texture_mips(source.textures.at(image.source), image.mips).at(cutout_level).rgba;
    };
    REQUIRE(planned(0) != planned(1)); // Otherwise the check below could not tell the weighting apart.
    CHECK(planned(0) == texture_mips(source.textures[0], {.alpha_weighted_color = true}).at(cutout_level).rgba);
    const auto meshes = Mesh::compile_static(source, {.max_texture_edge = cutout_limit});
    REQUIRE(meshes.size() == 1);
    const auto &description = *meshes.front()->description();
    CHECK(description.textures.size() == 2); // One copy per set of mip options.
    for (const auto &material : description.materials) {
        CAPTURE(material.name);
        const auto &shrunk = *description.textures.at(material.texture).image;
        CHECK(shrunk.width == cutout_limit);
        CHECK(shrunk.rgba == planned(material.name == "blended" ? 0 : 1));
    }
}

TEST_CASE("A texture within the limit keeps its texels once for every cutoff") {
    const auto source = coverage_source();
    const auto meshes = Mesh::compile_static(source, {.max_texture_edge = cutout_edge});
    REQUIRE(meshes.size() == 1);
    const auto &textures = meshes.front()->description()->textures;
    REQUIRE(textures.size() == 1);
    CHECK(textures.front().image == source.textures.front().image);
}

TEST_CASE("Compiled meshes and static pieces share their source's images") {
    auto source = static_source({0, 1, 0});
    // Texture 1 samples texture 0's image with another filter, for material 1.
    source.textures.push_back(source.textures[0]);
    source.textures[1].sampler.mag = Filter::nearest;
    source.materials[1].texture = 1;
    const auto &authored = source.textures[0].image;
    const auto whole = Mesh::compile(source);
    for (const auto &texture : whole->description()->textures)
        CHECK(texture.image == authored);
    // One triangle per piece, so the three pieces alternate between the two textures.
    const auto pieces = Mesh::compile_static(source, {.max_vertices = triangle_corners});
    REQUIRE(pieces.size() == source.primitives.size());
    for (const auto &piece : pieces) {
        REQUIRE(piece->description()->textures.size() == 1);
        CHECK(piece->description()->textures[0].image == authored);
    }
    // The image shrinks once, for every piece and for both textures, which share it and its encoding.
    const auto shrunk =
        Mesh::compile_static(source, {.max_vertices = triangle_corners, .max_texture_edge = reduced_edge});
    REQUIRE(shrunk.size() == source.primitives.size());
    REQUIRE(shrunk[0]->description()->textures.size() == 1);
    const auto &reduced = shrunk[0]->description()->textures[0].image;
    CHECK(reduced != authored);
    CHECK(reduced->width == reduced_edge);
    for (const auto &piece : shrunk) {
        REQUIRE(piece->description()->textures.size() == 1);
        CHECK(piece->description()->textures[0].image == reduced);
    }
    CHECK(shrunk[1]->description()->textures[0].sampler.mag == Filter::nearest);
}

TEST_CASE("Static splitting rejects an out-of-range material index as compile() does") {
    auto source = static_source({0, 1});
    source.primitives.back().material = static_cast<int>(source.materials.size());
    rejects_like_compile<std::invalid_argument>(source, invalid_material);
}

TEST_CASE("Static splitting rejects an invalid material as compile() does, used or not") {
    auto referencing = static_source({0, 1});
    referencing.materials[1].normal_texture = static_cast<int>(referencing.textures.size());
    rejects_like_compile<std::invalid_argument>(referencing, invalid_texture_reference);
    auto unused = static_source({1}); // Material 0 is referenced by no primitive.
    unused.materials[0].factor.x = beyond_unit_factor;
    rejects_like_compile<std::invalid_argument>(unused, invalid_factors);
}

TEST_CASE("Static splitting rejects a malformed texture as compile() does, used or not") {
    auto truncated = static_source({0, 1});
    auto pixels = *truncated.textures[0].image; // Material 0's texture is larger than the texture limit.
    pixels.rgba.pop_back();
    truncated.textures[0].image = std::make_shared<Image>(std::move(pixels));
    rejects_like_compile<std::invalid_argument>(truncated, texture_byte_mismatch);
    auto unused = static_source({0, 1});
    unused.textures.push_back(texture_of({1, 1, {}})); // No material samples it, and it has no texels.
    rejects_like_compile<std::invalid_argument>(unused, no_texels);
}

TEST_CASE("A mesh describes its source's materials, textures and metadata, which snapshots add up") {
    // A root, a joint above it whose inverse bind matrix is off by a quarter unit, and a mesh node with a skinned
    // triangle of the textured material and a rigid one of the plain material.
    auto source = static_source({});
    source.nodes.resize(3);
    source.nodes[1].parent = 0;
    source.nodes[1].rest.translation = {0, 1, 0};
    auto inverse_bind = identity();
    inverse_bind[13] = -.75F;
    source.skins.push_back({{0, 1}, {identity(), inverse_bind}});
    auto skinned = triangle(0);
    skinned.node = 2;
    skinned.skin = 0;
    for (auto &vertex : skinned.vertices) {
        vertex.joints = {1, 0, 0, 0};
        vertex.weights = {1, 0, 0, 0};
    }
    auto rigid = triangle(1);
    rigid.node = 2;
    source.primitives = {skinned, rigid};
    source.mesh_nodes = 1;
    source.notices = {"Imported for description"};
    Animation clip;
    clip.name = "wave";
    source.animations.push_back(clip);
    const auto mesh = Mesh::compile(source);
    static_assert(noexcept(mesh->description()));
    REQUIRE(mesh->description());
    const auto &description = *mesh->description();
    REQUIRE(description.materials.size() == source.materials.size());
    for (std::size_t i = 0; i < source.materials.size(); ++i) {
        CAPTURE(i);
        CHECK(description.materials[i].name == source.materials[i].name);
        CHECK(description.materials[i].texture == source.materials[i].texture);
    }
    REQUIRE(description.textures.size() == source.textures.size());
    CHECK(description.textures[0].image == source.textures[0].image);
    CHECK(description.mesh_nodes == 1);
    CHECK(description.skins == 1);
    CHECK(description.joints == 2);
    CHECK(description.skinned_vertices == triangle_corners);
    CHECK(description.clips == std::vector<std::string>{"wave"});
    CHECK(description.notices == source.notices);
    CHECK(description.bind_deviation == .25F);
    CHECK_FALSE(description.default_is_bind_pose);
    // Snapshots list each object's materials and textures and add up the statistics.
    const auto plain = Mesh::compile(static_source({1}));
    CHECK(plain->description()->bind_deviation == 0);
    CHECK(plain->description()->default_is_bind_pose);
    Scene scene;
    for (const auto &object : {mesh, mesh, plain})
        (void)scene.add(object);
    const auto snapshot = scene.snapshot();
    CHECK(snapshot.materials.size() == description.materials.size() * 2 + plain->description()->materials.size());
    CHECK(snapshot.textures.size() == description.textures.size() * 2 + plain->description()->textures.size());
    CHECK(snapshot.mesh_nodes == 2);
    CHECK(snapshot.skins == 2);
    CHECK(snapshot.joints == 4);
    CHECK(snapshot.skinned_vertices == triangle_corners * 2);
    CHECK(snapshot.clips == std::vector<std::string>{"wave", "wave"});
    CHECK(snapshot.notices == std::vector<std::string>{source.notices[0], source.notices[0]});
    CHECK(snapshot.bind_deviation == .25F);
    CHECK_FALSE(snapshot.default_is_bind_pose);
}

TEST_CASE("Static pieces carry the source's mesh node count and import notices, as compile() does") {
    auto source = static_source({0, 1, 0});
    source.mesh_nodes = source.nodes.size();
    source.notices = {"Imported for static splitting"};
    const auto whole = Mesh::compile(source);
    REQUIRE(whole->description()->mesh_nodes == source.mesh_nodes);
    REQUIRE(whole->description()->notices == source.notices);
    auto empty = source;
    empty.primitives.clear();
    for (const auto *input : {&source, &empty})
        for (const auto options : {MeshCompileOptions{.max_vertices = triangle_corners},
                                   MeshCompileOptions{.max_texture_edge = reduced_edge}}) {
            CAPTURE(input->primitives.size());
            CAPTURE(options.max_vertices);
            const auto pieces = Mesh::compile_static(*input, options);
            REQUIRE_FALSE(pieces.empty());
            for (const auto &piece : pieces) {
                CHECK(piece->description()->mesh_nodes == source.mesh_nodes);
                CHECK(piece->description()->notices == source.notices);
            }
        }
}

TEST_CASE("Static splitting reports the first of several defects that compile() reports") {
    // The node hierarchy is checked before any material.
    auto hierarchy = static_source({0, 1});
    hierarchy.nodes[0].parent = static_cast<int>(hierarchy.nodes.size()); // Past the last node.
    hierarchy.materials[0].factor.x = beyond_unit_factor;
    rejects_like_compile<std::runtime_error>(hierarchy, invalid_parent);
    // Each primitive is checked whole, in order, so an earlier primitive's vertex comes before a later
    // primitive's material index.
    auto ordered = static_source({0, 1});
    ordered.primitives[0].vertices[0].position.x = std::numeric_limits<float>::quiet_NaN();
    ordered.primitives[1].material = static_cast<int>(ordered.materials.size());
    rejects_like_compile<std::invalid_argument>(ordered, invalid_vertex);
}

namespace {
// static_source({0, 1}) with a second texture that samples texture 0's image with another filter, for material 1.
Asset shared_image_source() {
    auto source = static_source({0, 1});
    source.textures.push_back(source.textures[0]);
    source.textures[1].sampler.mag = Filter::nearest;
    source.materials[1].texture = 1;
    return source;
}
} // namespace

TEST_CASE("A mesh that keeps its texels reads them through its textures") {
    const auto source = shared_image_source();
    const auto mesh = Mesh::compile(source);
    CHECK(mesh->texel_retention() == TexelRetention::keep);
    const auto images = mesh->texel_images();
    REQUIRE(images.size() == source.textures.size());
    for (std::size_t i = 0; i < images.size(); ++i)
        CHECK(images[i] == mesh->description()->textures[i].image);
    mesh->release_texels(); // Does nothing for TexelRetention::keep.
    CHECK(mesh->texel_images() == images);
}

TEST_CASE("A mesh compiled until upload describes its textures without texels and holds its source's images") {
    std::weak_ptr<const Image> authored;
    std::shared_ptr<const Mesh> mesh;
    {
        const auto source = shared_image_source();
        authored = source.textures[0].image;
        mesh = Mesh::compile(source, {.texel_retention = TexelRetention::until_upload});
    }
    CHECK(mesh->texel_retention() == TexelRetention::until_upload);
    // The source is gone, but the mesh holds its image for the upload.
    REQUIRE_FALSE(authored.expired());
    const auto &textures = mesh->description()->textures;
    REQUIRE(textures.size() == 2);
    const auto &description = textures[0].image;
    CHECK(description != authored.lock());
    CHECK(description->width == authored_edge);
    CHECK(description->height == authored_edge);
    CHECK(description->rgba.empty());
    // Textures that share an image share its description, and keep their own samplers.
    CHECK(textures[1].image == description);
    CHECK(textures[1].sampler.mag == Filter::nearest);
    CHECK_NOTHROW(validate_scene(snapshot_of(mesh)));
    const auto images = mesh->texel_images();
    REQUIRE(images.size() == 2);
    CHECK(images[0] == authored.lock());
    CHECK(images[1] == authored.lock());
}

TEST_CASE("Released texels are freed with their last holder and stay readable until then") {
    auto source = std::make_shared<Asset>(shared_image_source());
    const std::weak_ptr<const Image> authored = source->textures[0].image;
    const auto mesh = Mesh::compile(*source, {.texel_retention = TexelRetention::until_upload});
    mesh->release_texels();
    // The application still holds the source, so the images stay readable through the mesh.
    CHECK(mesh->texel_images().front() == authored.lock());
    source.reset();
    CHECK(authored.expired());
    CHECK_THROWS_WITH_AS((void)mesh->texel_images(), released_texels, std::logic_error);
    CHECK_THROWS_WITH_AS(MeshPreparation(mesh), released_texels, std::logic_error);
    mesh->release_texels(); // Idempotent.
    // The mesh itself stays usable for drawing and inspection.
    CHECK(mesh->description()->textures.size() == 2);
    CHECK_NOTHROW(validate_scene(snapshot_of(mesh)));
}

TEST_CASE("Meshes that share images hold them until each of them is released") {
    std::weak_ptr<const Image> authored, reduced;
    std::vector<std::shared_ptr<const Mesh>> pieces;
    {
        const auto source = static_source({0, 1, 0});
        authored = source.textures[0].image;
        pieces = Mesh::compile_static(source, {.max_vertices = triangle_corners,
                                               .max_texture_edge = reduced_edge,
                                               .mesh = {.texel_retention = TexelRetention::until_upload}});
    }
    REQUIRE(pieces.size() == 3);
    // Only the shrunk image is needed, and every textured piece holds it.
    CHECK(authored.expired());
    reduced = pieces[0]->texel_images().at(0);
    REQUIRE_FALSE(reduced.expired());
    CHECK(reduced.lock()->width == reduced_edge);
    CHECK(pieces[0]->description()->textures[0].image->rgba.empty());
    pieces[0]->release_texels();
    CHECK(pieces[0]->texel_images().at(0) == reduced.lock()); // Piece 2 still holds it.
    pieces[2]->release_texels();
    CHECK(reduced.expired());
    CHECK_THROWS_WITH_AS((void)pieces[0]->texel_images(), released_texels, std::logic_error);
    CHECK(pieces[1]->texel_images().empty()); // The untextured piece has nothing to release.
}

TEST_CASE("A copy of a mesh holds its images on its own") {
    std::weak_ptr<const Image> authored;
    std::shared_ptr<const Mesh> mesh;
    {
        const auto source = static_source({0});
        authored = source.textures[0].image;
        mesh = Mesh::compile(source, {.texel_retention = TexelRetention::until_upload});
    }
    const auto copy = std::make_shared<const Mesh>(*mesh);
    mesh->release_texels();
    CHECK(mesh->texel_images() == copy->texel_images());
    copy->release_texels();
    CHECK(authored.expired());
}

TEST_CASE("A copy of a released mesh finds its images only while something else holds them") {
    auto source = std::make_shared<Asset>(static_source({0}));
    const std::weak_ptr<const Image> authored = source->textures[0].image;
    const auto mesh = Mesh::compile(*source, {.texel_retention = TexelRetention::until_upload});
    mesh->release_texels();
    const auto copy = std::make_shared<const Mesh>(*mesh);
    CHECK(copy->texel_images().at(0) == authored.lock());
    source.reset();
    CHECK(authored.expired());
    CHECK_THROWS_WITH_AS((void)copy->texel_images(), released_texels, std::logic_error);
}

// Only compiling or copying creates a Mesh, so every Mesh has the materials that scenes and texel_images() read.
static_assert(!std::is_default_constructible_v<Mesh>);
static_assert(std::is_copy_constructible_v<Mesh> && std::is_copy_assignable_v<Mesh>);

TEST_CASE("Moving a mesh copies it, so the mesh moved from keeps its content") {
    const auto compiled = Mesh::compile(static_source({0, 1}));
    Mesh constructed_from = *compiled, assigned_from = *compiled;
    const Mesh constructed = std::move(constructed_from);
    Mesh assigned = *Mesh::compile(static_source({1}));
    assigned = std::move(assigned_from);
    Scene scene;
    // The meshes moved from are read on purpose: the moves copied them.
    for (const Mesh *mesh :
         std::initializer_list<const Mesh *>{&constructed_from, &assigned_from, &constructed, &assigned}) {
        REQUIRE(mesh->description() == compiled->description());
        CHECK(mesh->vertices().size() == compiled->vertices().size());
        CHECK(mesh->draws().size() == compiled->draws().size());
        CHECK_NOTHROW((void)scene.create({}, std::make_shared<const Mesh>(*mesh)));
    }
}

TEST_CASE("Compilation rejects an unknown texel retention and images without texels") {
    const auto source = static_source({0, 1});
    const auto unknown = static_cast<TexelRetention>(2);
    CHECK_THROWS_WITH_AS(Mesh::compile(source, {.texel_retention = unknown}), unknown_retention, std::invalid_argument);
    CHECK_THROWS_WITH_AS(Mesh::compile_static(source, {.mesh = {.texel_retention = unknown}}), unknown_retention,
                         std::invalid_argument);
    // compile() checks the levels of detail first, and compile_static() the retention before anything else.
    const MeshOptions both{.texel_retention = unknown, .lods = {.levels = 9}};
    CHECK_THROWS_WITH_AS(Mesh::compile(source, both), "Mesh LOD levels must be from 0 to 8", std::invalid_argument);
    CHECK_THROWS_WITH_AS(Mesh::compile_static(source, {.max_vertices = 1, .mesh = both}), unknown_retention,
                         std::invalid_argument);
    // A mesh's descriptions cannot be compiled again: they have no texels to upload.
    auto described = source;
    described.textures =
        Mesh::compile(source, {.texel_retention = TexelRetention::until_upload})->description()->textures;
    rejects_like_compile<std::invalid_argument>(described, no_texels);
}
