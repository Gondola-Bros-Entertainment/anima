#include <anima/scene.hpp>
#include <iostream>
#include <limits>

namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void rejects(F action) {
    try {
        action();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error("Invalid resource operation accepted");
}
bool near(anima::Vec3 a, anima::Vec3 b) { return anima::length(a - b) < 1e-4F; }
anima::Asset fixture() {
    anima::Asset asset;
    asset.nodes.resize(2);
    asset.skins.push_back({{0, 1}, {anima::identity(), anima::identity()}});
    asset.materials.push_back({"surface", {.3F, .6F, .9F}, -1});
    anima::SourcePrimitive p;
    p.skin = 0;
    p.material = 0;
    for (const auto position : {anima::Vec3{0, 0, 0}, anima::Vec3{1, 0, 0}, anima::Vec3{0, 1, 0}}) {
        anima::SourceVertex v;
        v.position = position;
        v.normal = anima::normalized({.2F, .4F, 1});
        v.color = {.4F, .5F, .6F};
        v.joints = {0, 1, 0, 0};
        v.weights = {.25F, .75F, 0, 0};
        v.uv = {position.x, position.y};
        p.vertices.push_back(v);
    }
    const auto copy = p.vertices;
    p.vertices.insert(p.vertices.end(), copy.begin(), copy.end());
    asset.primitives.push_back(p);
    p.skin = -1;
    p.node = 1;
    asset.primitives.push_back(p);
    return asset;
}
void parity(const anima::Asset &source, const anima::Scene::Instance &instance, const anima::Pose &pose,
            const anima::Mat4 &world) {
    auto reference = anima::make_mesh_snapshot(source, pose);
    anima::pose_mesh_snapshot(source, pose, reference, 0, world);
    std::size_t corner = 0, primitive = 0;
    for (const auto &draw : instance.asset->draws()) {
        for (std::size_t i = draw.first_index; i < draw.first_index + draw.index_count; ++i) {
            const auto &vertex = instance.asset->vertices()[instance.asset->indices()[i]];
            auto matrix = instance.palette[draw.palette_offset];
            if (draw.skinned) {
                matrix = {};
                for (unsigned j = 0; j < 4; ++j)
                    if (vertex.weights[j] != 0)
                        for (unsigned k = 0; k < 16; ++k)
                            matrix[k] +=
                                instance.palette[draw.palette_offset + vertex.joints[j]][k] * vertex.weights[j];
            }
            const auto position = anima::point(matrix, vertex.position);
            const auto &expected = reference.vertices[corner++];
            const auto factor = instance.factors[draw.material];
            require(near(position, expected.position) && near(anima::normal(matrix, vertex.normal), expected.normal) &&
                        near({vertex.color.x * factor.x, vertex.color.y * factor.y, vertex.color.z * factor.z},
                             expected.color) &&
                        vertex.uv == expected.uv,
                    "Indexed palette/reference mismatch");
            const auto &b = instance.primitive_bounds[primitive];
            require(position.x >= b.minimum.x - 1e-4F && position.y >= b.minimum.y - 1e-4F &&
                        position.z >= b.minimum.z - 1e-4F && position.x <= b.maximum.x + 1e-4F &&
                        position.y <= b.maximum.y + 1e-4F && position.z <= b.maximum.z + 1e-4F,
                    "Animated influence bounds clipped a reference vertex");
            const auto &whole = instance.bounds;
            require(whole.valid && whole.minimum.x <= b.minimum.x && whole.minimum.y <= b.minimum.y &&
                        whole.minimum.z <= b.minimum.z && whole.maximum.x >= b.maximum.x &&
                        whole.maximum.y >= b.maximum.y && whole.maximum.z >= b.maximum.z,
                    "Instance broad-phase bounds clipped a primitive bound");
        }
        ++primitive;
    }
}
} // namespace
int main() {
    try {
        auto source = fixture();
        const auto asset = anima::Mesh::compile(source);
        require(asset->indices().size() == 12 && asset->vertices().size() == 6, "Exact duplicate indexing failed");
        anima::Scene scene, other;
        const auto a = scene.add(asset), b = scene.add(asset);
        require(scene.instance(a).asset == scene.instance(b).asset, "Instances duplicated their compiled asset");
        auto pose = anima::sample_pose(source);
        anima::Transform transform;
        transform.scale = {.7F, 1.2F, 1.7F};
        transform.translation = {2, 1, -3};
        auto world = anima::matrix(transform);
        for (float time : {0.F, .2F, .7F, 1.F}) {
            pose.world[0] = anima::identity();
            pose.world[0][12] = time;
            transform.rotation = {0, 0, std::sin(time * .5F), std::cos(time * .5F)};
            pose.world[1] = anima::matrix(transform);
            scene.set_pose(a, pose, world);
            parity(source, scene.instance(a), pose, world);
            parity(source, scene.instance(b), asset->rest_pose(), anima::identity());
        }
        const auto accepted = scene.instance(a).palette;
        auto invalid = pose;
        invalid.world.pop_back();
        rejects([&] { scene.set_pose(a, invalid); });
        invalid = pose;
        invalid.world[0][0] = std::numeric_limits<float>::quiet_NaN();
        rejects([&] { scene.set_pose(a, invalid); });
        require(scene.instance(a).palette == accepted, "Rejected pose changed accepted palette");
        const auto snapshot = scene.snapshot();
        require(snapshot.vertices.size() == 24 && snapshot.primitives.size() == 4,
                "Reference export topology is wrong");
        rejects([&] { (void)scene.snapshot({snapshot.vertices.size() * sizeof(anima::MeshVertex) - 1}); });
        require(scene.instance(a).palette == accepted, "Rejected tooling export changed the render instance");
        scene.set_material_factor(a, 0, {1, 0, 0});
        require(scene.instance(b).factors[0].y == .6F && asset->materials()->material_data[0].factor.y == .6F,
                "Material edit leaked into another instance or asset");
        rejects([&] { scene.set_material_factor(a, 0, {2, 0, 0}); });
        scene.clear_material_factor(a, 0);
        scene.set_primitive_visible(a, 0, false);
        require(scene.instance(b).primitive_visible[0], "Visibility leaked into another instance");
        rejects([&] { other.set_visible(a, false); });
        scene.remove(a);
        rejects([&] { scene.set_visible(a, false); });
        const auto c = scene.add(asset);
        require(c.slot == a.slot && c.generation != a.generation, "Reused slot accepted stale handle generation");
        rejects([&] { scene.remove(a); });
        source.primitives[0].vertices[0].position = {99, 99, 99};
        source.materials[0].factor = {0, 0, 0};
        require(asset->vertices()[0].position.x == 0 && asset->materials()->material_data[0].factor.y == .6F,
                "Compiled resource depends on mutable source storage");
        auto seam = fixture();
        seam.primitives[0].vertices[3].uv[0] = .5F;
        require(anima::Mesh::compile(seam)->vertices().size() == 7, "Indexing welded a UV seam");
        seam = fixture();
        seam.primitives[0].vertices[3].weights = {.5F, .5F, 0, 0};
        require(anima::Mesh::compile(seam)->vertices().size() == 7, "Indexing welded different skin influences");
        auto rounded = fixture();
        for (auto &v : rounded.primitives[0].vertices)
            v.weights = {.25008F, .75F, 0, 0};
        const auto rounded_asset = anima::Mesh::compile(rounded);
        const auto rounded_id = other.add(rounded_asset);
        auto distant = anima::identity();
        distant[12] = 100000;
        other.set_pose(rounded_id, rounded_asset->rest_pose(), distant);
        parity(rounded, other.instance(rounded_id), rounded_asset->rest_pose(), distant);
        for (int kind = 0; kind < 7; ++kind) {
            auto bad = fixture();
            if (kind == 0)
                bad.primitives[0].vertices[0].weights[0] = -1;
            if (kind == 1)
                bad.primitives[0].vertices[0].joints[0] = 2;
            if (kind == 2)
                bad.primitives[0].vertices.pop_back();
            if (kind == 3)
                bad.primitives[0].skin = 3;
            if (kind == 4)
                bad.primitives[0].node = 3;
            if (kind == 5)
                bad.primitives[0].material = 3;
            if (kind == 6)
                bad.skins[0].inverse_bind.pop_back();
            rejects([&] { (void)anima::Mesh::compile(bad); });
        }
        anima::Scene empty;
        require(!empty.bounds().valid && empty.instances().empty(), "Empty resource scene invalid");
        std::cout << "PASS immutable indexed resources, independent poses/materials, influence bounds and generational "
                     "handles\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
