#include <anima/scene.hpp>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void near(float a, float b, const char *message) { require(std::abs(a - b) < 1e-4F, message); }
template <class F> void rejects(F action, const std::string &message) {
    try {
        action();
    } catch (const std::exception &error) {
        if (std::string(error.what()).find(message) != std::string::npos)
            return;
        throw;
    }
    throw std::runtime_error("Expected rejection: " + message);
}
anima::Mat4 translation(float x) {
    auto m = anima::identity();
    m[12] = x;
    return m;
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
    source->textures = {{1, 1, {255, 0, 0, 255}, {}}, {1, 1, {0, 255, 0, 255}, {}}};
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
void snapshots(const std::shared_ptr<const anima::Asset> &source) {
    const auto compiled = anima::Mesh::compile(*source);
    anima::Scene instances;
    const auto a = instances.add(compiled), b = instances.add(compiled);
    const auto reference = anima::make_mesh_snapshot(*source, anima::sample_pose(*source));
    const auto count = reference.vertices.size();
    const auto initial = instances.snapshot();
    require(initial.vertices.size() == count * 2 && initial.primitives.size() == source->primitives.size() * 2,
            "Snapshot lost instance geometry");
    require(initial.material_data.size() == source->materials.size() * 2 &&
                initial.textures.size() == source->textures.size() * 2,
            "Snapshot lost instance materials/textures");
    for (std::size_t i = 0; i < source->materials.size(); ++i) {
        const auto texture = source->materials[i].texture;
        require(initial.material_data[source->materials.size() + i].texture ==
                    (texture < 0 ? -1 : texture + int(source->textures.size())),
                "Snapshot failed to remap texture references");
    }
    auto world = translation(10);
    auto pose = anima::sample_pose(*source);
    if (!source->animations.empty())
        pose = anima::sample_pose(*source, &source->animations.front(), .5);
    instances.set_pose(a, pose, world);
    auto expected = reference;
    anima::pose_mesh_snapshot(*source, pose, expected, 0, world);
    const auto posed = instances.snapshot();
    for (std::size_t i = 0; i < count; ++i) {
        require(anima::length(posed.vertices[i].position - expected.vertices[i].position) < 1e-4F &&
                    anima::length(posed.vertices[i].normal - expected.vertices[i].normal) < 1e-4F &&
                    posed.vertices[i].uv == expected.vertices[i].uv,
                "Snapshot differs from independent source deformation");
        require(anima::length(posed.vertices[count + i].position - reference.vertices[i].position) < 1e-4F,
                "Posing one instance changed another");
    }
    const auto bytes = posed.vertices.size() * sizeof(anima::MeshVertex);
    require(instances.snapshot({bytes}).vertices.size() == count * 2, "Exact snapshot budget rejected");
    const auto accepted = instances.instance(a).palette;
    rejects([&] { (void)instances.snapshot({bytes - 1}); }, "budget");
    require(instances.instance(a).palette == accepted, "Rejected snapshot changed the pose");
    instances.set_visible(a, false);
    instances.set_primitive_visible(b, 0, false);
    const auto hidden = instances.snapshot();
    require(!hidden.primitives.front().visible && !hidden.primitives[source->primitives.size()].visible,
            "Snapshot lost explicit instance/primitive visibility");
    instances.set_visible(a, true);
    if (!source->materials.empty()) {
        instances.set_material_factor(a, 0, {.2F, .4F, .8F});
        const auto colored = instances.snapshot();
        near(colored.material_data[0].factor.x, .2F, "Snapshot lost material override");
        near(colored.material_data[source->materials.size()].factor.x, source->materials[0].factor.x,
             "Snapshot appearance leaked into another instance");
        for (std::size_t p = 0; p < source->primitives.size(); ++p)
            if (source->primitives[p].material == 0) {
                const auto offset = colored.primitives[p].first_vertex;
                near(colored.vertices[offset].color.x, source->primitives[p].vertices[0].color.x * .2F,
                     "Snapshot did not multiply vertex colour by the override");
            }
        instances.clear_material_factor(a, 0);
        near(instances.snapshot().material_data[0].factor.x, source->materials[0].factor.x,
             "Clearing override failed to restore source material");
    }
    instances.remove(a);
    instances.remove(b);
    require(instances.snapshot({0}).vertices.empty(), "Empty scene cannot be inspected with a zero budget");
    require(initial.vertices.size() == count * 2, "Snapshot borrowed mutable instance storage");
}
void seams() {
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
        const auto pose = anima::sample_pose(*source, &source->animations[0], t);
        anima::Transform transform;
        transform.translation = {2, float(t), -1};
        transform.rotation = {0, std::sin(float(t)), 0, std::cos(float(t))};
        transform.scale = {1, 1.1F, .9F};
        const auto world = anima::matrix(transform);
        anima::pose_mesh_snapshot(*source, pose, reference, 0, world);
        instances.set_pose(id, pose, world);
        const auto actual = instances.snapshot();
        for (std::size_t i = 0; i < reference.vertices.size(); ++i) {
            const auto &a = actual.vertices[i], &b = reference.vertices[i];
            require(anima::length(a.position - b.position) < 1e-4F && anima::length(a.normal - b.normal) < 1e-4F &&
                        anima::length(a.color - b.color) < 1e-4F && a.uv == b.uv,
                    "Snapshot changed a seam, skin influence, material or rigid transform");
        }
    }
}
} // namespace
int main(int argc, char **argv) {
    try {
        snapshots(asset());
        seams();
        if (argc > 1)
            snapshots(anima::load_asset(argv[1]));
        std::cout << "PASS independent CPU snapshots, poses, materials, texture remapping, visibility and budgets\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
