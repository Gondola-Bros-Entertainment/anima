#include "near.hpp"
#include <anima/assets/asset.hpp>
#include <anima/assets/fitted.hpp>
#include <anima/assets/imports.hpp>
#include <anima/assets/mesh_snapshot.hpp>
#include <anima/assets/preview.hpp>
// This suite supplies its own main, which reads the optional exported GLB argument.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace anima;
namespace {
constexpr float tolerance = 1e-4F; // Absolute error allowed in imported and posed values.
constexpr auto exported_test = "An exported GLB loads its geometry in its bind pose";
// cgltf reports a GLB shorter than its header's length as cgltf_result_data_too_short.
constexpr auto truncated_glb = "Parse GLB failed (cgltf 1)";
constexpr auto invalid_factors = "Invalid material factors";

// The GLB that the exported_import test names on the command line.
std::optional<std::filesystem::path> exported_glb;

void integer(std::vector<char> &bytes, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes.push_back(static_cast<char>((value >> (i * 8)) & 255));
}
void scalar(std::vector<char> &bytes, float value) { integer(bytes, std::bit_cast<std::uint32_t>(value)); }
struct Temp {
    std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("anima-import-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temp() { std::filesystem::create_directory(directory); }
    Temp(const Temp &) = delete;
    Temp &operator=(const Temp &) = delete;
    ~Temp() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
};
std::filesystem::path motion_fixture(const Temp &temp) {
    std::vector<char> bin;
    for (float value : {0.F, 1.F, 0.F, 0.F, 0.F, 2.F, 0.F, 0.F})
        scalar(bin, value);
    std::string json = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
      "nodes":[{"name":"parent","translation":[10,0,0],"children":[1]},
               {"name":"animated","children":[2]}, {"name":"attachment","translation":[0,3,0]}],
      "buffers":[{"byteLength":32}],
      "bufferViews":[{"buffer":0,"byteLength":8},{"buffer":0,"byteOffset":8,"byteLength":24}],
      "accessors":[{"bufferView":0,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},
                   {"bufferView":1,"componentType":5126,"count":2,"type":"VEC3"}],
      "animations":[{"name":"move","samplers":[{"input":0,"output":1,"interpolation":"LINEAR"}],
                     "channels":[{"sampler":0,"target":{"node":1,"path":"translation"}}]}]})";
    while (json.size() % 4)
        json += ' ';
    std::vector<char> glb;
    integer(glb, 0x46546c67);
    integer(glb, 2);
    integer(glb, static_cast<std::uint32_t>(28 + json.size() + bin.size()));
    integer(glb, static_cast<std::uint32_t>(json.size()));
    integer(glb, 0x4e4f534a);
    glb.insert(glb.end(), json.begin(), json.end());
    integer(glb, static_cast<std::uint32_t>(bin.size()));
    integer(glb, 0x004e4942);
    glb.insert(glb.end(), bin.begin(), bin.end());
    const auto path = temp.directory / "motion.glb";
    std::ofstream output(path, std::ios::binary);
    output.write(glb.data(), static_cast<std::streamsize>(glb.size()));
    REQUIRE(bool(output));
    return path;
}
std::filesystem::path fixture(const Temp &temp, const std::string &kind) {
    std::vector<char> bin;
    for (auto p : {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}}) {
        for (float f : {p.x, p.y, p.z, 0.70710678F, 0.70710678F, 0.0F})
            scalar(bin, f);
    }
    bin.insert(bin.end(), {0, 0, 1, 0, 2, 0, 0, 0}); // ushort indices plus alignment to offset 80
    if (kind == "bad-index") {
        const auto invalid_byte = std::bit_cast<char>(std::uint8_t{255});
        bin[72] = invalid_byte;
        bin[73] = invalid_byte;
    }
    for (int i = 0; i < 3; ++i)
        for (float f : {1.F, 0.F, 0.F, 0.F})
            scalar(bin, f);
    bin.resize(140, 0); // byte joint indices
    auto inverse = identity();
    if (kind == "bind")
        inverse[13] = -5;
    for (auto value : inverse)
        scalar(bin, value);
    std::string nodes = R"([{"name":"parent","translation":[10,0,0],"children":[1]},
      {"name":"child","mesh":0,"translation":[1,2,3],"scale":[2,3,4]},
      {"name":"mirrored instance","mesh":0,"matrix":[-1,0,0,0,0,1,0,0,0,0,1,0,-3,0,0,1]},
      {"name":"unused scene","mesh":0,"translation":[999,0,0]}])";
    std::string scenes = R"([{"nodes":[0,2]},{"nodes":[3]}])";
    std::string skin;
    if (kind == "skin" || kind == "bind") {
        nodes = R"([{"name":"mesh with ignored skin transform","mesh":0,"skin":0,"translation":[99,0,0]},
                  {"name":"joint","translation":[0,5,0]}])";
        scenes = R"([{"nodes":[0,1]}])";
        skin = R"(,"skins":[{"joints":[1],"inverseBindMatrices":5}])";
    }
    std::string json =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":)" + scenes + R"(,"nodes":)" + nodes + skin + R"(,
      "buffers":[{"byteLength":204}],
      "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":72,"byteStride":24},
                     {"buffer":0,"byteOffset":72,"byteLength":6},
                     {"buffer":0,"byteOffset":80,"byteLength":48},
                     {"buffer":0,"byteOffset":128,"byteLength":12},
                     {"buffer":0,"byteOffset":140,"byteLength":64}],
      "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
                   {"bufferView":0,"byteOffset":12,"componentType":5126,"count":3,"type":"VEC3"},
                   {"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"},
                   {"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"},
                   {"bufferView":3,"componentType":5121,"count":3,"type":"VEC4"},
                   {"bufferView":4,"componentType":5126,"count":1,"type":"MAT4"}],
      "materials":[{"name":"red","pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1]}},
                   {"name":"green","pbrMetallicRoughness":{"baseColorFactor":[0,1,0,1]}}],
      "meshes":[{"primitives":[
        {"attributes":{"POSITION":0,"NORMAL":1,"WEIGHTS_0":3,"JOINTS_0":4},"indices":2,"material":0},
        {"attributes":{"POSITION":0,"NORMAL":1,"WEIGHTS_0":3,"JOINTS_0":4},"material":1}]}])";
    if (kind == "animation" || kind == "step" || kind == "cubic" || kind == "duplicate-channel" ||
        kind == "bad-times") {
        scalar(bin, 0);
        scalar(bin, kind == "bad-times" ? 0.F : 1.F);
        const auto samples = kind == "cubic" ? 6 : 2;
        for (int i = 0; i < samples; ++i)
            for (float f : {float(i * 2), 0.F, 0.F})
                scalar(bin, f);
        const auto views = json.find("],\n      \"accessors\"");
        json.insert(views, R"(,{"buffer":0,"byteOffset":204,"byteLength":8},
            {"buffer":0,"byteOffset":212,"byteLength":)" +
                               std::to_string(samples * 12) + "}");
        const auto accessors = json.find("],\n      \"materials\"");
        json.insert(accessors, R"(,{"bufferView":5,"componentType":5126,"count":2,"type":"SCALAR"},
            {"bufferView":6,"componentType":5126,"count":)" +
                                   std::to_string(samples) + R"(,"type":"VEC3"})");
        const auto bytes = json.find("\"byteLength\":204");
        json.replace(bytes, std::string("\"byteLength\":204").size(), "\"byteLength\":" + std::to_string(bin.size()));
        const std::string channel = R"({"sampler":0,"target":{"node":1,"path":"translation"}})";
        const std::string interpolation = kind == "cubic" ? "CUBICSPLINE" : kind == "step" ? "STEP" : "LINEAR";
        json += R"(,"animations":[{"name":"move","samplers":[{"input":6,"output":7,"interpolation":")" + interpolation +
                R"("}],"channels":[)" + channel + (kind == "duplicate-channel" ? "," + channel : "") + "]}]";
    }
    if (kind == "extension")
        json += R"(,"extensionsRequired":["KHR_draco_mesh_compression"])";
    json += '}';
    if (kind == "alpha") {
        json.insert(json.find("\"name\":\"red\""), "\"alphaMode\":\"BLEND\",");
        json.replace(json.find("[1,0,0,1]"), 9, "[1,0,0,0.4]");
    }
    if (kind == "double-sided")
        json.insert(json.find("\"name\":\"red\""), "\"doubleSided\":true,");
    if (kind == "mask") {
        json.insert(json.find("\"name\":\"red\""), "\"alphaMode\":\"MASK\",\"alphaCutoff\":0.35,");
        json.insert(json.find("\"POSITION\":0"), "\"COLOR_0\":3,");
        const auto factor = json.find("[1,0,0,1]");
        json.replace(factor, 9, "[1,0,0,0.7]");
    }
    if (kind == "pbr" || kind == "bad-metallic" || kind == "bad-roughness") {
        const std::string factors = kind == "bad-metallic"    ? "\"metallicFactor\":1.1,"
                                    : kind == "bad-roughness" ? "\"roughnessFactor\":-0.1,"
                                                              : "\"metallicFactor\":0.7,\"roughnessFactor\":0.23,";
        json.insert(json.find("\"baseColorFactor\""), factors);
    }
    if (kind == "implicit-material") {
        const auto material = json.find(",\"material\":0");
        json.erase(material, std::string(",\"material\":0").size());
    }
    if (kind == "bad-view") {
        const auto offset = json.find("\"byteOffset\":72");
        json.replace(offset, std::string("\"byteOffset\":72").size(), "\"byteOffset\":99999");
    }
    if (kind == "sparse")
        json.insert(
            json.find("\"bufferView\":0"),
            R"("sparse":{"count":1,"indices":{"bufferView":1,"componentType":5123},"values":{"bufferView":0}},)");
    while (json.size() % 4)
        json += ' ';
    std::vector<char> glb;
    integer(glb, 0x46546c67);
    integer(glb, 2);
    integer(glb, static_cast<std::uint32_t>(28 + json.size() + bin.size()));
    integer(glb, static_cast<std::uint32_t>(json.size()));
    integer(glb, 0x4e4f534a);
    glb.insert(glb.end(), json.begin(), json.end());
    integer(glb, static_cast<std::uint32_t>(bin.size()));
    integer(glb, 0x004e4942);
    glb.insert(glb.end(), bin.begin(), bin.end());
    if (kind == "truncated") {
        REQUIRE(glb.size() >= 16);
        glb.erase(glb.end() - 16, glb.end());
    }
    const auto path = temp.directory / (kind + ".glb");
    std::ofstream output(path, std::ios::binary);
    output.write(glb.data(), static_cast<std::streamsize>(glb.size()));
    REQUIRE(bool(output));
    return path;
}
} // namespace

TEST_CASE("A reimport publishes an independent version, and a failed reimport keeps the accepted one") {
    const Temp temp;
    const auto reimport_path = fixture(temp, "bind");
    AssetImports imports(temp.directory);
    auto imported = imports.add<Asset>(
        "body", [&](ImportSource &source) { return load_asset(source.read(reimport_path.filename())); });
    const auto original_import = imported.get();
    std::filesystem::copy_file(fixture(temp, "animation"), reimport_path,
                               std::filesystem::copy_options::overwrite_existing);
    CHECK(imports.refresh() == std::vector<std::string>{"body"});
    CHECK(imported.revision() == 2);
    CHECK(imported.get()->animations.size() == 1);
    CHECK(original_import->animations.empty());
    std::filesystem::copy_file(fixture(temp, "truncated"), reimport_path,
                               std::filesystem::copy_options::overwrite_existing);
    CHECK_THROWS_WITH_AS(imports.refresh(), truncated_glb, std::runtime_error);
    CHECK(imported.revision() == 2);
    CHECK(imported.get()->animations.size() == 1);
}

TEST_CASE("A motion resource animates its hierarchy without a rendering payload") {
    const Temp temp;
    const auto motion = load_motion_asset(motion_fixture(temp));
    CHECK(motion->primitives.empty());
    CHECK(motion->skins.empty());
    REQUIRE(motion->animations.size() == 1);
    const auto moving = sample_pose(*motion, &motion->animations[0], .5);
    CHECK(moving.world[1][12] == Near{11, tolerance}); // The parent's transform applies.
    CHECK(moving.world[2][12] == Near{11, tolerance}); // The unkeyed attachment follows the animation.
    // A bound local pose moves the descendants and keeps their unkeyed rest translation.
    auto local = moving.local;
    local[0].translation.x = 20;
    const auto rebound = pose_from_local(*motion, local);
    CHECK(rebound.world[2][12] == Near{21, tolerance});
    CHECK(rebound.world[2][13] == Near{3, tolerance});
    CHECK_THROWS_WITH_AS(pose_from_local(*motion, std::span<const Transform>(local).first(2)),
                         "Local pose must cover every asset node", std::invalid_argument);
}

TEST_CASE("The geometry and motion loaders reject each other's resources") {
    const Temp temp;
    CHECK_THROWS_WITH_AS(load_motion_asset(fixture(temp, "animation")),
                         "Motion resources require animation without geometry, skins or materials", std::runtime_error);
    CHECK_THROWS_WITH_AS(load_asset(motion_fixture(temp)), "Selected scene contains no renderable triangles",
                         std::runtime_error);
}

TEST_CASE("Materials keep their alpha mode, metallic-roughness factors, sidedness and glTF defaults") {
    const Temp temp;
    const auto masked = load_glb(fixture(temp, "mask"));
    CHECK(masked.material_data.at(0).alpha_mode == AlphaMode::mask);
    CHECK(masked.material_data[0].alpha_cutoff == Near{.35F, tolerance});
    CHECK(masked.material_data[0].alpha == Near{.7F, tolerance});
    CHECK(masked.vertices.at(0).alpha == Near{0, tolerance}); // The vertex colour's alpha.
    const auto blended = load_glb(fixture(temp, "alpha"));
    CHECK(blended.material_data.at(0).alpha_mode == AlphaMode::blend);
    CHECK(blended.material_data[0].alpha == Near{.4F, tolerance});
    CHECK(blended.material_data.at(1).alpha_mode == AlphaMode::opaque); // glTF's default mode.
    const auto pbr = load_glb(fixture(temp, "pbr"));
    CHECK(pbr.material_data.at(0).metallic == Near{.7F, tolerance});
    CHECK(pbr.material_data[0].roughness == Near{.23F, tolerance});
    // glTF's defaults for factors a material omits.
    CHECK(pbr.material_data.at(1).metallic == Near{1, tolerance});
    CHECK(pbr.material_data[1].roughness == Near{1, tolerance});
    // A primitive without a material uses the spec's default material.
    const auto implicit = load_glb(fixture(temp, "implicit-material"));
    const auto &fallback = implicit.material_data.at(implicit.primitives.at(0).material_index);
    CHECK(fallback.metallic == Near{1, tolerance});
    CHECK_FALSE(fallback.double_sided);
    // glTF materials are single-sided unless they say otherwise; programmatic ones render both sides.
    CHECK_FALSE(masked.material_data[0].double_sided);
    CHECK(load_glb(fixture(temp, "double-sided")).material_data.at(0).double_sided);
    CHECK(Material{}.double_sided);
}

TEST_CASE("The default scene keeps each primitive and instance with its transform, normals and material") {
    const Temp temp;
    const auto scene = load_glb(fixture(temp, "instances"));
    CHECK(scene.mesh_nodes == 2);
    REQUIRE(scene.primitives.size() == 4);
    REQUIRE(scene.vertices.size() == 12);
    CHECK(scene.primitives[0].material_index == 0);
    CHECK(scene.primitives[1].material_index == 1);
    CHECK(scene.minimum.x == Near{-4, tolerance}); // The mirrored matrix instance.
    CHECK(scene.maximum.x == Near{13, tolerance}); // The nested TRS.
    CHECK(scene.maximum.y == Near{5, tolerance});
    CHECK(scene.maximum.z == Near{3, tolerance}); // Only the default scene's hierarchy.
    // Normals transform by the inverse transpose, including under mirrored scale.
    CHECK(scene.vertices[0].normal.x == Near{0.8320503F, tolerance});
    CHECK(scene.vertices[0].normal.y == Near{0.5547002F, tolerance});
    CHECK(scene.vertices[6].normal.x == Near{-0.70710678F, tolerance});
    // Vertex colours carry each material's base color factor.
    CHECK(scene.vertices[0].color.x == Near{1, tolerance});
    CHECK(scene.vertices[3].color.y == Near{1, tolerance});
}

TEST_CASE("A skin applies its joint's transform, and inverse binds cancel it") {
    const Temp temp;
    const auto skinned = load_glb(fixture(temp, "skin"));
    CHECK(skinned.skinned_vertices == 6);
    CHECK_FALSE(skinned.default_is_bind_pose);
    CHECK(skinned.minimum.y == Near{5, tolerance});
    CHECK(skinned.minimum.x == Near{0, tolerance}); // The skinned mesh node's own transform is ignored.
    const auto bind = load_glb(fixture(temp, "bind"));
    CHECK(bind.default_is_bind_pose);
    CHECK(bind.minimum.y == Near{0, tolerance});
}

TEST_CASE("A fitted load shares the Mesh that a scene still draws after its fitted asset is gone") {
    const Temp temp;
    const auto body = load_asset(fixture(temp, "skin"));
    Manifest manifest;
    manifest.directory = temp.directory;
    manifest.skeleton_id = "rig";
    manifest.bind_signature = "signature";
    // The skinned model fits its own rig.
    const FittedLibrary library(
        body, manifest, "profile",
        R"({"version":2,"items":[{"id":"cover","fits":{"profile":{"model":"skin.glb","skeleton":"rig",)"
        R"("bind_signature":"signature"}}}]})");
    Scene scene;
    auto loaded = library.load("cover");
    (void)scene.add(loaded->render);
    const std::weak_ptr<const FittedAsset> dropped = loaded;
    const std::weak_ptr<const Mesh> drawn = loaded->render;
    loaded.reset();
    REQUIRE(dropped.expired());
    const auto reloaded = library.load("cover");
    CHECK(reloaded->render == drawn.lock());
    CHECK(library.resident_meshes() == std::vector{reloaded->render});
}

TEST_CASE("A model without clips previews statically, and rejected playback leaves it unchanged") {
    const Temp temp;
    (void)fixture(temp, "bind");
    const auto manifest_path = temp.directory / "bind.asset.json";
    {
        std::ofstream output(manifest_path);
        output << R"({"version":4,"units":"meters","asset_id":"test.bind","model":"bind.glb","motion_contract":null,
            "skeleton":{"id":"test.rig","joint_count":1,"bind_signature":")"
               << std::string(64, '0') << R"("},"clips":[]})";
        REQUIRE(bool(output));
    }
    AssetPreview preview(manifest_path);
    const auto static_status = preview.status();
    const auto static_pose = preview.pose().world;
    CHECK(preview.is_bind());
    CHECK_FALSE(preview.playback().animation());
    CHECK(static_status == "Bind pose | static");
    preview.toggle_play();
    preview.restart();
    CHECK(preview.advance(.25).empty());
    CHECK(preview.is_bind());
    CHECK(preview.pose().world == static_pose);
    CHECK(preview.status() == static_status);
    CHECK_THROWS_WITH_AS(preview.select("Walk"), "Clip has no manifest playback policy: Walk", std::out_of_range);
    CHECK_THROWS_WITH_AS(preview.seek(.5), "Invalid playback seek", std::invalid_argument);
    CHECK(preview.is_bind());
    CHECK(preview.pose().world == static_pose);
    CHECK(preview.status() == static_status);
}

TEST_CASE("Imported LINEAR and STEP samplers interpolate as declared") {
    const Temp temp;
    for (const auto *kind : {"animation", "step"}) {
        CAPTURE(kind);
        const auto animated = load_asset(fixture(temp, kind));
        REQUIRE(animated->animations.size() == 1);
        const auto pose = sample_pose(*animated, &animated->animations[0], .5);
        CHECK(pose.world[1][12] == Near{std::string(kind) == "step" ? 10.F : 11.F, tolerance});
    }
}

TEST_CASE("Unsupported animation samplers and channels are rejected with their reason") {
    const Temp temp;
    CHECK_THROWS_WITH_AS(
        load_asset(fixture(temp, "cubic")),
        "Unsupported animation interpolation: only LINEAR and STEP are implemented (CUBICSPLINE rejected)",
        std::runtime_error);
    CHECK_THROWS_WITH_AS(load_asset(fixture(temp, "duplicate-channel")), "Duplicate animation target channel",
                         std::runtime_error);
    CHECK_THROWS_WITH_AS(load_asset(fixture(temp, "bad-times")), "Animation times must increase strictly",
                         std::runtime_error);
}

TEST_CASE("Invalid and unsupported GLB content is rejected with its reason") {
    const Temp temp;
    // cgltf's validation reports an index beyond the vertices as cgltf_result_data_too_short.
    CHECK_THROWS_WITH_AS(load_glb(fixture(temp, "bad-index")),
                         "Validate GLB structure/accessor bounds failed (cgltf 1)", std::runtime_error);
    CHECK_THROWS_WITH_AS(load_glb(fixture(temp, "bad-view")), "Buffer view exceeds embedded buffer bounds",
                         std::runtime_error);
    CHECK_THROWS_WITH_AS(load_glb(fixture(temp, "sparse")), "Sparse accessors are unsupported", std::runtime_error);
    CHECK_THROWS_WITH_AS(load_glb(fixture(temp, "extension")), "Required glTF extension is unsupported",
                         std::runtime_error);
    CHECK_THROWS_WITH_AS(load_glb(fixture(temp, "truncated")), truncated_glb, std::runtime_error);
}

TEST_CASE("Material factors outside their ranges are rejected by validate_material") {
    // validate_material owns the factor ranges.
    const Temp temp;
    CHECK_THROWS_WITH_AS(load_glb(fixture(temp, "bad-metallic")), invalid_factors, std::invalid_argument);
    CHECK_THROWS_WITH_AS(load_glb(fixture(temp, "bad-roughness")), invalid_factors, std::invalid_argument);
}

TEST_CASE("A perspective projection maps the near and far planes to reversed Vulkan depths 1 and 0") {
    const auto projection = perspective(1.5F, 0.1F, 100.F);
    CHECK((-0.1F * projection[10] + projection[14]) / 0.1F == Near{1, tolerance});
    CHECK((-100.F * projection[10] + projection[14]) / 100.F == Near{0, tolerance});
}

TEST_CASE("Reversed depth keeps surfaces 1 cm apart at 450 m in order") {
    // A 3 cm near plane with forward depth spaces adjacent float depths about 40 cm apart at 450 m.
    const auto projection = perspective(16.F / 9.F, .03F, 500.F);
    const auto depth = [&](float distance) { return (-distance * projection[10] + projection[14]) / distance; };
    for (int step = 0; step < 100; ++step) {
        const float distance = 450.F + float(step) * .01F;
        CAPTURE(distance);
        CHECK(depth(distance) > depth(distance + .01F));
    }
}

TEST_CASE("An orbit camera's view origin and horizontal axes follow its orientation within its bounds") {
    const Temp temp;
    const auto scene = load_glb(fixture(temp, "instances"));
    OrbitCamera camera;
    camera.frame(scene.minimum, scene.maximum);
    const auto origin = view_origin(camera.matrix(1.5F));
    const auto eye = camera.target + Vec3{std::sin(camera.yaw) * std::cos(camera.pitch), std::sin(camera.pitch),
                                          std::cos(camera.yaw) * std::cos(camera.pitch)} *
                                         camera.distance;
    CHECK(origin[0] == Near{eye.x, tolerance});
    CHECK(origin[1] == Near{eye.y, tolerance});
    CHECK(origin[2] == Near{eye.z, tolerance});
    CHECK(origin[3] == Near{1, tolerance}); // A perspective origin is a point.
    CHECK(length(camera.position() - eye) == Near{0, tolerance});
    for (const float yaw : {0.F, 1.F, -2.F, 3.F}) {
        CAPTURE(yaw);
        camera.yaw = yaw;
        const auto forward = camera.horizontal_forward(), right = camera.horizontal_right();
        CHECK(forward.y == Near{0, tolerance});
        CHECK(right.y == Near{0, tolerance});
        CHECK(length(forward) == Near{1, tolerance});
        CHECK(length(right) == Near{1, tolerance});
        CHECK(dot(forward, right) == Near{0, tolerance});
        const auto toward = camera.target - camera.position();
        CHECK(length(forward - normalized(Vec3{toward.x, 0, toward.z})) == Near{0, tolerance});
        CHECK(length(right - cross(forward, {0, 1, 0})) == Near{0, tolerance}); // Strafing is not reversed.
    }
    camera.orbit(999, 999);
    camera.zoom(999);
    CHECK(camera.pitch <= 1.4F);
    CHECK(camera.distance >= camera.radius * 1.2F);
    for (const auto value : camera.matrix(1.5F))
        CHECK(std::isfinite(value));
}

TEST_CASE("An orthographic view origin is the direction toward the camera, and a singular view is rejected") {
    auto ortho = identity();
    ortho[5] = -1;
    ortho[10] = .01F; // Reversed depth grows toward the camera.
    const Vec3 oe{4, 2, 5}, ot{-1, 0, 1};
    const auto direction = view_origin(ortho * look_at(oe, ot));
    const auto expected = normalized(oe - ot);
    CHECK(direction[0] == Near{expected.x, tolerance});
    CHECK(direction[1] == Near{expected.y, tolerance});
    CHECK(direction[2] == Near{expected.z, tolerance});
    CHECK(direction[3] == Near{0, tolerance}); // An orthographic origin is a direction.
    CHECK_THROWS_WITH_AS(view_origin({}), math_error_message(MathErrorCode::singular_projection), MathError);
}

TEST_CASE(exported_test) {
    REQUIRE(exported_glb);
    const auto asset = load_glb(*exported_glb);
    CHECK(asset.mesh_nodes > 0);
    CHECK_FALSE(asset.vertices.empty());
    CHECK(asset.default_is_bind_pose);
    print_mesh_report(asset);
}

int main(int argc, char **argv) {
    doctest::Context context(argc, argv);
    // exported_import names an exported GLB: the first argument that is not a doctest option.
    for (int i = 1; i < argc; ++i)
        if (argv[i][0] != '-') {
            exported_glb = argv[i];
            break;
        }
    if (!exported_glb)
        context.addFilter("test-case-exclude", exported_test);
    return context.run();
}
