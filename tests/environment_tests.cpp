#include "near.hpp"
#include <anima/assets/scene_validation.hpp>
#include <anima/environment.hpp>
#include <anima/scene.hpp>
#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>

using namespace anima;
namespace {
constexpr float tolerance = 1e-4F; // Matrix products, shadow depths, tangents and vertex alpha.
constexpr auto texel_range = "Shadow texel size exceeds finite range";
// A sun straight overhead, a world shadow region of extent 8 and depth 20, and an enabled detail
// region of extent 1.5 centered at (1, 2, 3).
Environment overhead_sun() {
    Environment env;
    env.sun.direction = {0, 1, 0};
    env.shadow.extent = 8;
    env.shadow.depth = 20;
    env.detail_shadow = env.shadow;
    env.detail_shadow.enabled = true;
    env.detail_shadow.extent = 1.5F;
    env.detail_shadow.center = {1, 2, 3};
    return env;
}
// A 2x1 image from transparent black to opaque white.
Texture gradient(TextureEncoding encoding) {
    Texture image{2, 1, {0, 0, 0, 0, 255, 255, 255, 255}, {}};
    image.encoding = encoding;
    return image;
}
// Data maps on texture 0, which must be linear, and color maps on texture 1, which must be sRGB.
Material textured() {
    Material material;
    material.normal_texture = 0;
    material.metallic_roughness_texture = 0;
    material.occlusion_texture = 0;
    material.emissive_texture = 1;
    material.texture = 1;
    material.emissive = {.2F, .3F, .4F};
    material.alpha_mode = AlphaMode::mask;
    return material;
}
// Two triangles with textured() that differ only in tangent handedness and alpha.
std::shared_ptr<Asset> seams() {
    auto source = std::make_shared<Asset>();
    source->nodes.resize(1);
    source->textures = {gradient(TextureEncoding::linear), gradient(TextureEncoding::srgb)};
    source->materials.push_back(textured());
    SourcePrimitive primitive;
    primitive.material = 0;
    for (int i = 0; i < 6; ++i) {
        SourceVertex v;
        v.position = {float(i % 3), float(i % 3 == 1), 0};
        v.normal = {0, 0, 1};
        v.tangent = {1, 0, 0, i < 3 ? 1.F : -1.F};
        v.alpha = i < 3 ? 1.F : .25F;
        primitive.vertices.push_back(v);
    }
    source->primitives.push_back(primitive);
    return source;
}
// Negates X and scales Y, so it flips tangent handedness.
Mat4 mirror() {
    auto result = identity();
    result[0] = -2;
    result[5] = 3;
    return result;
}
} // namespace

TEST_CASE("A view projection times its inverse is the identity, and a singular matrix is rejected") {
    const auto vp = perspective(1.3F, .03F, 80) * look_at({4, 5, 7}, {1, 0, 2});
    const auto product = vp * inverse(vp);
    for (std::size_t i = 0; i < product.size(); ++i) {
        CAPTURE(i);
        CHECK(product[i] == Near{identity()[i], tolerance});
    }
    CHECK_THROWS_WITH_AS(inverse({}), math_error_message(MathErrorCode::singular_matrix), MathError);
}

TEST_CASE("Shadow regions map their depth to [0, 1] and snap their origins to texels") {
    auto env = overhead_sun();
    const auto light = directional_shadow_matrix(env);
    CHECK(point(light, {0, 0, 0}).z == Near{.5F, tolerance});
    CHECK(point(light, {0, 10, 0}).z == Near{0, tolerance});
    CHECK(point(light, {0, -10, 0}).z == Near{1, tolerance});
    env.shadow.center.x = .001F;
    CHECK(light == directional_shadow_matrix(env));
    const auto detail = directional_shadow_matrix(env, true);
    env.detail_shadow.center.x += .0001F;
    CHECK(detail == directional_shadow_matrix(env, true));
    CHECK(light == directional_shadow_matrix(env)); // Moving the detail region leaves the world region.
    env.detail_shadow.center.x += .2F;
    CHECK(detail != directional_shadow_matrix(env, true));
}

TEST_CASE("Invalid lights, exposure, fog and shadow regions are rejected") {
    const auto env = overhead_sun();
    auto bad = env;
    bad.sun.direction = {};
    CHECK_THROWS_WITH_AS(validate_environment(bad), "Invalid directional light", std::invalid_argument);
    bad = env;
    bad.exposure = 0;
    CHECK_THROWS_WITH_AS(validate_environment(bad), "Invalid environment exposure or fog density",
                         std::invalid_argument);
    bad = env;
    bad.fog_density = -1;
    CHECK_THROWS_WITH_AS(validate_environment(bad), "Invalid environment exposure or fog density",
                         std::invalid_argument);
    bad = env;
    bad.shadow.extent = std::numeric_limits<float>::max();
    CHECK_THROWS_WITH_AS(directional_shadow_matrix(bad), texel_range, std::invalid_argument);
    bad = env;
    bad.shadow.extent = std::numeric_limits<float>::denorm_min();
    CHECK_THROWS_WITH_AS(directional_shadow_matrix(bad), texel_range, std::invalid_argument);
    bad = env;
    bad.detail_shadow.extent = std::numeric_limits<float>::denorm_min();
    CHECK_THROWS_WITH_AS(directional_shadow_matrix(bad, true), texel_range, std::invalid_argument);
    bad = env;
    bad.detail_shadow.center.y = std::numeric_limits<float>::quiet_NaN();
    CHECK_THROWS_WITH_AS(validate_environment(bad), "Invalid directional shadow region", std::invalid_argument);
}

TEST_CASE("Color mips average in sRGB, and data mips and alpha average linearly") {
    const auto color = texture_mips(gradient(TextureEncoding::srgb));
    CHECK(color.back().rgba[0] == 188);
    CHECK(color.back().rgba[3] == 128);
    const auto data = gradient(TextureEncoding::linear);
    CHECK(texture_mips(data).back().rgba[0] == 128);
    CHECK_THROWS_WITH_AS(base_color_mips(data), "Base-color mipmaps require sRGB encoding", std::invalid_argument);
}

TEST_CASE("A material texture must have the encoding its use requires") {
    const auto source = seams();
    auto material = textured();
    CHECK_NOTHROW(validate_material(material, source->textures));
    material.normal_texture = 1;
    CHECK_THROWS_WITH_AS(validate_material(material, source->textures),
                         "Material texture encoding does not match its usage", std::invalid_argument);
}

TEST_CASE("Compiling keeps tangent and alpha seams, and mirroring flips tangent handedness") {
    const auto source = seams();
    const auto compiled = Mesh::compile(*source);
    CHECK(compiled->vertices().size() == 6);
    const auto t = tangent(mirror(), {1, 0, 0, 1});
    CHECK(t[0] == Near{-2, tolerance});
    CHECK(t[3] == Near{-1, tolerance});
    CHECK(tangent(mirror(), {}) == std::array<float, 4>{}); // A missing tangent gains no frame.
    Scene resources;
    const auto id = resources.add(compiled);
    resources.set_pose(id, sample_pose(*source), mirror());
    const auto snapshot = resources.snapshot();
    CHECK(snapshot.vertices[3].alpha == Near{.25F, tolerance});
    CHECK(snapshot.vertices[3].tangent[3] == Near{1, tolerance});
}

TEST_CASE("Snapshots offset each instance's texture references, and compiled materials keep their own copy") {
    const auto source = seams();
    const auto compiled = Mesh::compile(*source);
    Scene resources;
    const auto id = resources.add(compiled);
    resources.set_pose(id, sample_pose(*source), mirror());
    (void)resources.add(compiled);
    const auto composed = resources.snapshot();
    CHECK(composed.material_data[1].normal_texture == 2);
    CHECK(composed.material_data[1].emissive_texture == 3);
    CHECK_NOTHROW(validate_scene(composed));
    source->materials[0].normal_scale = .5F;
    CHECK(compiled->materials()->material_data[0].normal_scale == 1);
}

TEST_CASE("A tangent handedness other than 0, 1 or -1 is rejected") {
    auto source = seams();
    source->primitives[0].vertices[0].tangent[3] = .5F;
    CHECK_THROWS_WITH_AS(Mesh::compile(*source), "Invalid render vertex", std::invalid_argument);
}
