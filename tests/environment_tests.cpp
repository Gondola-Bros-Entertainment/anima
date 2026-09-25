#include <anima/assets/scene_validation.hpp>
#include <anima/environment.hpp>
#include <anima/scene.hpp>
#include <iostream>
#include <limits>

namespace {
void require(bool v, const char *message) {
    if (!v)
        throw std::runtime_error(message);
}
void near(float a, float b) { require(std::abs(a - b) < 1e-4F, "Numeric contract mismatch"); }
template <class F> void invalid(F action) {
    try {
        action();
    } catch (const std::invalid_argument &) {
        return;
    }
    throw std::runtime_error("Invalid input accepted");
}

} // namespace
int main() {
    using namespace anima;
    try {
        const auto vp = perspective(1.3F, .03F, 80) * look_at({4, 5, 7}, {1, 0, 2});
        const auto product = vp * inverse(vp);
        for (std::size_t i = 0; i < 16; ++i)
            near(product[i], identity()[i]);
        invalid([] { (void)inverse({}); });
        Environment env;
        env.sun.direction = {0, 1, 0};
        env.shadow.extent = 8;
        env.shadow.depth = 20;
        const auto light = directional_shadow_matrix(env);
        near(point(light, {0, 0, 0}).z, .5F);
        near(point(light, {0, 10, 0}).z, 0);
        near(point(light, {0, -10, 0}).z, 1);
        env.shadow.center.x = .001F;
        require(light == directional_shadow_matrix(env), "Shadow origin did not snap to texels");
        env.detail_shadow = env.shadow;
        env.detail_shadow.enabled = true;
        env.detail_shadow.extent = 1.5F;
        env.detail_shadow.center = {1, 2, 3};
        const auto detail = directional_shadow_matrix(env, true);
        env.detail_shadow.center.x += .0001F;
        require(detail == directional_shadow_matrix(env, true), "Detail origin did not snap to its texels");
        require(light == directional_shadow_matrix(env), "Moving detail shadows changed the world projection");
        env.detail_shadow.center.x += .2F;
        require(detail != directional_shadow_matrix(env, true), "Detail region did not follow its center");
        auto bad = env;
        bad.sun.direction = {};
        invalid([&] { validate_environment(bad); });
        bad = env;
        bad.exposure = 0;
        invalid([&] { validate_environment(bad); });
        bad = env;
        bad.fog_density = -1;
        invalid([&] { validate_environment(bad); });
        bad = env;
        bad.shadow.extent = std::numeric_limits<float>::max();
        invalid([&] { (void)directional_shadow_matrix(bad); });
        bad = env;
        bad.shadow.extent = std::numeric_limits<float>::denorm_min();
        invalid([&] { (void)directional_shadow_matrix(bad); });
        bad = env;
        bad.detail_shadow.extent = std::numeric_limits<float>::denorm_min();
        invalid([&] { (void)directional_shadow_matrix(bad, true); });
        bad = env;
        bad.detail_shadow.center.y = std::numeric_limits<float>::quiet_NaN();
        invalid([&] { validate_environment(bad); });
        Texture image{2, 1, {0, 0, 0, 0, 255, 255, 255, 255}, {}};
        auto mips = texture_mips(image);
        require(mips.back().rgba[0] == 188 && mips.back().rgba[3] == 128, "Colour or alpha mip encoding wrong");
        image.encoding = TextureEncoding::linear;
        mips = texture_mips(image);
        require(mips.back().rgba[0] == 128, "Data texture incorrectly treated as sRGB");
        invalid([&] { (void)base_color_mips(image); });
        auto source = std::make_shared<Asset>();
        source->nodes.resize(1);
        source->textures.push_back(image);
        image.encoding = TextureEncoding::srgb;
        source->textures.push_back(image);
        Material material;
        material.normal_texture = 0;
        material.metallic_roughness_texture = 0;
        material.occlusion_texture = 0;
        material.emissive_texture = 1;
        material.texture = 1;
        material.emissive = {.2F, .3F, .4F};
        material.alpha_mode = AlphaMode::mask;
        source->materials.push_back(material);
        validate_material(material, source->textures);
        material.normal_texture = 1;
        invalid([&] { validate_material(material, source->textures); });
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
        const auto compiled = Mesh::compile(*source);
        require(compiled->vertices().size() == 6, "Index welding erased tangent/alpha seam");
        auto mirror = identity();
        mirror[0] = -2;
        mirror[5] = 3;
        const auto t = tangent(mirror, {1, 0, 0, 1});
        near(t[0], -2);
        near(t[3], -1);
        require(tangent(mirror, {}) == std::array<float, 4>{}, "Missing tangent gained a frame");
        Scene resources;
        const auto id = resources.add(compiled);
        resources.set_pose(id, sample_pose(*source), mirror);
        const auto snapshot = resources.snapshot();
        near(snapshot.vertices[3].alpha, .25F);
        near(snapshot.vertices[3].tangent[3], 1);
        (void)resources.add(compiled);
        const auto composed = resources.snapshot();
        require(composed.material_data[1].normal_texture == 2 && composed.material_data[1].emissive_texture == 3,
                "Snapshot did not offset every material texture");
        validate_scene(composed);
        source->materials[0].normal_scale = .5F;
        require(compiled->materials()->material_data[0].normal_scale == 1,
                "Compiled materials depend on mutable source storage");
        source->primitives[0].vertices[0].tangent[3] = .5F;
        invalid([&] { (void)Mesh::compile(*source); });
        std::cout << "PASS: environment projection, shadow stability, data mips, material contracts, tangent and alpha "
                     "preservation\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
