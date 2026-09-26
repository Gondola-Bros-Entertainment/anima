#include <anima/assets/asset.hpp>
#include <anima/assets/imports.hpp>
#include <anima/assets/mesh_snapshot.hpp>
#include <anima/assets/preview.hpp>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
void near(float actual, float expected, const char *message) { require(std::abs(actual - expected) < 1e-4F, message); }
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
    require(bool(output), "Cannot write motion fixture");
    return path;
}
std::filesystem::path fixture(const Temp &temp, const std::string &kind) {
    std::vector<char> bin;
    for (auto p : {anima::Vec3{0, 0, 0}, anima::Vec3{1, 0, 0}, anima::Vec3{0, 1, 0}}) {
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
    auto inverse = anima::identity();
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
    if (kind == "alpha")
        json.insert(json.find("\"name\":\"red\""), "\"alphaMode\":\"BLEND\",");
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
        require(glb.size() >= 16, "Synthetic GLB is too short to truncate");
        glb.erase(glb.end() - 16, glb.end());
    }
    const auto path = temp.directory / (kind + ".glb");
    std::ofstream output(path, std::ios::binary);
    output.write(glb.data(), static_cast<std::streamsize>(glb.size()));
    require(bool(output), "Cannot write synthetic GLB");
    return path;
}
} // namespace
int main(int argc, char **argv) {
    try {
        Temp temp;
        const auto reimport_path = fixture(temp, "bind");
        anima::AssetImports imports(temp.directory);
        auto imported = imports.add<anima::Asset>("body", [&](anima::ImportSource &source) {
            return anima::load_asset(source.read(reimport_path.filename()));
        });
        const auto original_import = imported.get();
        std::filesystem::copy_file(fixture(temp, "animation"), reimport_path,
                                   std::filesystem::copy_options::overwrite_existing);
        require(imports.refresh() == std::vector<std::string>{"body"} && imported.revision() == 2 &&
                    imported.get()->animations.size() == 1 && original_import->animations.empty(),
                "GLB reimport did not publish an independent resource version");
        std::filesystem::copy_file(fixture(temp, "truncated"), reimport_path,
                                   std::filesystem::copy_options::overwrite_existing);
        bool broken_import = false;
        try {
            imports.refresh();
        } catch (const std::runtime_error &) {
            broken_import = true;
        }
        require(broken_import && imported.revision() == 2 && imported.get()->animations.size() == 1,
                "Broken GLB reimport destroyed the accepted resource");
        (void)fixture(temp, "bind");
        const auto motion_path = motion_fixture(temp);
        const auto motion = anima::load_motion_asset(motion_path);
        require(motion->primitives.empty() && motion->skins.empty() && motion->animations.size() == 1,
                "Independent motion imported rendering payload");
        const auto moving = anima::sample_pose(*motion, &motion->animations[0], .5);
        near(moving.world[1][12], 11, "Independent motion lost parent transform");
        near(moving.world[2][12], 11, "Unkeyed attachment did not follow animation");
        auto local = moving.local;
        local[0].translation.x = 20;
        const auto rebound = anima::pose_from_local(*motion, local);
        near(rebound.world[2][12], 21, "Bound local pose did not propagate to descendants");
        near(rebound.world[2][13], 3, "Bound local pose lost unkeyed rest translation");
        bool invalid_local = false, geometry_as_motion = false, motion_as_geometry = false;
        try {
            (void)anima::pose_from_local(*motion, std::span<const anima::Transform>(local).first(2));
        } catch (const std::invalid_argument &) {
            invalid_local = true;
        }
        try {
            (void)anima::load_motion_asset(fixture(temp, "animation"));
        } catch (const std::runtime_error &) {
            geometry_as_motion = true;
        }
        try {
            (void)anima::load_asset(motion_path);
        } catch (const std::runtime_error &) {
            motion_as_geometry = true;
        }
        require(invalid_local && geometry_as_motion && motion_as_geometry, "Resource contract was not enforced");
        const auto masked = anima::load_glb(fixture(temp, "mask"));
        require(masked.material_data[0].alpha_mode == anima::AlphaMode::mask, "Lost MASK alpha mode");
        near(masked.material_data[0].alpha_cutoff, .35F, "Lost alpha cutoff");
        near(masked.material_data[0].alpha, .7F, "Lost material alpha");
        near(masked.vertices[0].alpha, 0, "Lost vertex colour alpha");
        const auto pbr = anima::load_glb(fixture(temp, "pbr"));
        near(pbr.material_data[0].metallic, .7F, "Lost metallic factor");
        near(pbr.material_data[0].roughness, .23F, "Lost roughness factor");
        near(pbr.material_data[1].metallic, 1, "Wrong glTF metallic default");
        near(pbr.material_data[1].roughness, 1, "Wrong glTF roughness default");
        const auto implicit = anima::load_glb(fixture(temp, "implicit-material"));
        near(implicit.material_data.at(implicit.primitives[0].material_index).metallic, 1,
             "Missing glTF material must use spec default");
        const auto scene = anima::load_glb(fixture(temp, "instances"));
        require(scene.mesh_nodes == 2 && scene.primitives.size() == 4 && scene.vertices.size() == 12,
                "Lost primitive or mesh instance");
        require(scene.primitives[0].material_index == 0 && scene.primitives[1].material_index == 1,
                "Lost material assignment");
        near(scene.minimum.x, -4, "Wrong matrix/mirror transform");
        near(scene.maximum.x, 13, "Wrong nested TRS transform");
        near(scene.maximum.y, 5, "Wrong scale/translation");
        near(scene.maximum.z, 3, "Wrong hierarchy/default scene");
        near(scene.vertices[0].normal.x, 0.8320503F, "Wrong inverse-transpose normal x");
        near(scene.vertices[0].normal.y, 0.5547002F, "Wrong inverse-transpose normal y");
        near(scene.vertices[0].color.x, 1, "Wrong red factor");
        near(scene.vertices[3].color.y, 1, "Wrong green factor");
        near(scene.vertices[6].normal.x, -0.70710678F, "Wrong normal under mirrored scale");
        const auto skinned = anima::load_glb(fixture(temp, "skin"));
        require(skinned.skinned_vertices == 6 && !skinned.default_is_bind_pose, "Skin was ignored or mislabeled bind");
        near(skinned.minimum.y, 5, "Joint transform was not applied");
        near(skinned.minimum.x, 0, "Skinned mesh transform applied twice");
        const auto bind = anima::load_glb(fixture(temp, "bind"));
        require(bind.default_is_bind_pose, "Inverse binds were ignored");
        near(bind.minimum.y, 0, "Inverse bind did not cancel joint translation");
        const auto manifest_path = temp.directory / "bind.asset.json";
        {
            std::ofstream output(manifest_path);
            output << R"({"schema_version":1,"units":"meters","asset_id":"test.bind","model":"bind.glb",
                "skeleton":{"id":"test.rig","joint_count":1,"bind_signature":")"
                   << std::string(64, '0') << R"("},"clips":[],"equipment":[]})";
            require(bool(output), "Cannot write static manifest");
        }
        anima::AssetPreview preview(manifest_path);
        const auto static_status = preview.status();
        const auto static_pose = preview.pose().world;
        require(preview.is_bind() && !preview.playback().animation() && static_status == "Bind pose | static",
                "Clip-free model is not a static preview");
        preview.toggle_play();
        preview.restart();
        require(preview.advance(.25).empty() && preview.is_bind() && preview.pose().world == static_pose &&
                    preview.status() == static_status,
                "Static playback controls changed the bind pose");
        bool missing_clip = false, missing_timeline = false;
        try {
            preview.select("Walk");
        } catch (const std::out_of_range &) {
            missing_clip = true;
        }
        try {
            preview.seek(.5);
        } catch (const std::invalid_argument &) {
            missing_timeline = true;
        }
        require(missing_clip && missing_timeline && preview.is_bind() && preview.pose().world == static_pose &&
                    preview.status() == static_status,
                "Rejected static playback operation corrupted the preview");
        for (const auto *kind : {"animation", "step"}) {
            const auto animated = anima::load_asset(fixture(temp, kind));
            require(animated->animations.size() == 1, "Animation channels not imported");
            const auto pose = anima::sample_pose(*animated, &animated->animations[0], .5);
            near(pose.world[1][12], std::string(kind) == "step" ? 10.F : 11.F,
                 "Imported sampler interpolation incorrect");
        }
        for (const auto &[kind, diagnostic] :
             {std::pair{"cubic", "CUBICSPLINE"}, std::pair{"duplicate-channel", "Duplicate animation target"},
              std::pair{"bad-times", "increase strictly"}}) {
            bool rejected = false;
            try {
                (void)anima::load_asset(fixture(temp, kind));
            } catch (const std::runtime_error &error) {
                rejected = std::string(error.what()).find(diagnostic) != std::string::npos;
            }
            require(rejected, "Unsupported animation needs an explicit diagnostic");
        }
        for (const auto *kind :
             {"bad-index", "bad-view", "sparse", "extension", "alpha", "truncated", "bad-metallic", "bad-roughness"}) {
            bool rejected = false;
            try {
                (void)anima::load_glb(fixture(temp, kind));
            } catch (const std::runtime_error &) {
                rejected = true;
            }
            require(rejected, "Invalid or unsupported GLB accepted");
        }
        const auto projection = anima::perspective(1.5F, 0.1F, 100.F);
        near((-0.1F * projection[10] + projection[14]) / 0.1F, 0, "Vulkan near plane incorrect");
        near((-100.F * projection[10] + projection[14]) / 100.F, 1, "Vulkan far plane incorrect");
        anima::OrbitCamera camera;
        camera.frame(scene.minimum, scene.maximum);
        const auto origin = anima::view_origin(camera.matrix(1.5F));
        const auto eye =
            camera.target + anima::Vec3{std::sin(camera.yaw) * std::cos(camera.pitch), std::sin(camera.pitch),
                                        std::cos(camera.yaw) * std::cos(camera.pitch)} *
                                camera.distance;
        near(origin[0], eye.x, "View origin x incorrect");
        near(origin[1], eye.y, "View origin y incorrect");
        near(origin[2], eye.z, "View origin z incorrect");
        near(origin[3], 1, "Perspective origin must be a point");
        near(anima::length(camera.position() - eye), 0, "Orbit position disagrees with the view matrix");
        for (const float yaw : {0.F, 1.F, -2.F, 3.F}) {
            camera.yaw = yaw;
            const auto forward = camera.horizontal_forward(), right = camera.horizontal_right();
            near(forward.y, 0, "Orbit movement must stay horizontal");
            near(right.y, 0, "Orbit strafe must stay horizontal");
            near(anima::length(forward), 1, "Orbit forward is not normalized");
            near(anima::length(right), 1, "Orbit right is not normalized");
            near(anima::dot(forward, right), 0, "Orbit horizontal axes are not perpendicular");
            const auto toward = camera.target - camera.position();
            near(anima::length(forward - anima::normalized(anima::Vec3{toward.x, 0, toward.z})), 0,
                 "Orbit forward disagrees with its viewing direction");
            near(anima::length(right - anima::cross(forward, {0, 1, 0})), 0, "Orbit strafe is reversed");
        }
        auto ortho = anima::identity();
        ortho[5] = -1;
        ortho[10] = -.01F;
        const anima::Vec3 oe{4, 2, 5}, ot{-1, 0, 1};
        const auto direction = anima::view_origin(anima::operator*(ortho, anima::look_at(oe, ot)));
        const auto expected = anima::normalized(oe - ot);
        near(direction[0], expected.x, "Orthographic direction x incorrect");
        near(direction[1], expected.y, "Orthographic direction y incorrect");
        near(direction[2], expected.z, "Orthographic direction z incorrect");
        near(direction[3], 0, "Orthographic origin must be a direction");
        bool singular_rejected = false;
        try {
            (void)anima::view_origin({});
        } catch (const std::invalid_argument &) {
            singular_rejected = true;
        }
        require(singular_rejected, "Singular view matrix accepted");
        camera.orbit(999, 999);
        camera.zoom(999);
        require(camera.pitch <= 1.4F && camera.distance >= camera.radius * 1.2F, "Camera exceeded bounds");
        for (const auto value : camera.matrix(1.5F))
            require(std::isfinite(value), "Camera produced non-finite matrix");
        if (argc > 1) {
            const auto asset = anima::load_glb(argv[1]);
            require(asset.mesh_nodes > 0 && !asset.vertices.empty(), "External asset has no geometry");
            require(asset.default_is_bind_pose, "External asset did not load in its bind pose");
            anima::print_mesh_report(asset);
        }
        std::cout
            << "PASS: all primitives, scene selection/instances, indexed/strided accessors, mirrored TRS, normals, "
               "materials, CPU skin/inverse binds, malformed data, unsupported features, orbit projection\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
