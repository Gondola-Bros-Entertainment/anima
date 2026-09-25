#include <anima/assets/scene_validation.hpp>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
unsigned checks{};
void require(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
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
template <class F> void invalid(F edit) {
    auto scene = fixture();
    edit(scene);
    bool rejected = false;
    try {
        (void)anima::validate_scene(scene);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "Invalid scene accepted");
}
} // namespace
int main() {
    try {
        const auto bytes = 6 * sizeof(anima::MeshVertex);
        anima::validate_scene(fixture(), {bytes});
        try {
            (void)anima::validate_scene(fixture(), {bytes - 1});
            require(false, "Geometry exceeding budget by one byte accepted");
        } catch (const anima::SceneCapacityError &error) {
            require(error.requested_bytes == bytes && error.budget_bytes == bytes - 1,
                    "Capacity error lost requested and allowed bytes");
        }
        anima::validate_scene({}, {0});
        // Exercise size/index guards without allocating enormous buffers.
        try {
            (void)anima::validate_scene_geometry(std::numeric_limits<std::size_t>::max(),
                                                 {std::numeric_limits<std::size_t>::max()});
            require(false, "Geometry address/byte overflow accepted");
        } catch (const std::invalid_argument &) {
        }
        anima::validate_scene({});
        invalid([](auto &s) { s.primitives[0].first_vertex = UINT32_MAX; });
        invalid([](auto &s) { s.primitives[0].vertex_count = UINT32_MAX; });
        invalid([](auto &s) { s.primitives[0].vertex_count = 4; });
        invalid([](auto &s) { s.primitives[0].vertex_count = 0; });
        invalid([](auto &s) { s.primitives[0].first_vertex = 6; });
        for (int index : {-2, 2, std::numeric_limits<int>::max()}) {
            invalid([=](auto &s) { s.primitives[0].material_index = index; });
            invalid([=](auto &s) { s.material_data[0].texture = index; });
        }
        const auto nan = std::numeric_limits<float>::quiet_NaN();
        invalid([=](auto &s) { s.vertices[0].position.x = nan; });
        invalid([=](auto &s) { s.vertices[0].normal.y = nan; });
        invalid([=](auto &s) { s.vertices[0].color.z = nan; });
        invalid([=](auto &s) { s.vertices[0].uv[1] = nan; });
        invalid([=](auto &s) { s.primitives[0].node_world[0] = nan; });
        invalid([=](auto &s) { s.minimum.x = nan; });
        invalid([=](auto &s) { s.maximum.y = nan; });
        for (float value : {nan, -0.1F, 1.1F, std::numeric_limits<float>::infinity()}) {
            invalid([=](auto &s) { s.material_data[0].factor.x = value; });
            invalid([=](auto &s) { s.material_data[0].metallic = value; });
            invalid([=](auto &s) { s.material_data[0].roughness = value; });
        }
        invalid([](auto &s) { s.textures[0].width = 0; });
        invalid([](auto &s) { s.textures[0].height = 0; });
        invalid([](auto &s) { s.textures[0].width = s.textures[0].height = UINT32_MAX; });
        invalid([](auto &s) { s.textures[0].rgba.pop_back(); });
        invalid([](auto &s) { s.textures[0].sampler.mag = static_cast<anima::Filter>(99); });
        invalid([](auto &s) { s.textures[0].sampler.min = static_cast<anima::Filter>(99); });
        invalid([](auto &s) { s.textures[0].sampler.mip = static_cast<anima::Filter>(99); });
        invalid([](auto &s) { s.textures[0].sampler.u = static_cast<anima::Wrap>(99); });
        invalid([](auto &s) { s.textures[0].sampler.v = static_cast<anima::Wrap>(99); });
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
        anima::validate_scene(scene);
        require(true, "Pose/color/visibility rejected");
        scene.primitives[0].material_index = -1;
        scene.material_data[0].texture = -1;
        (void)anima::validate_scene(scene);
        require(true, "Default material/texture sentinel rejected");
        std::cout << "PASS scene validation: " << checks << " checks\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
