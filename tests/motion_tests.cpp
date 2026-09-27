#include "near.hpp"
#include <anima/assets/motion_runtime.hpp>
#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace anima;
namespace {
constexpr float pose_tolerance = 1e-5F; // Absolute error allowed in sampled matrix elements.
constexpr std::size_t rig_joints = 5;
// A bind signature is opaque to MotionRuntime; the contract only has to repeat the manifest's.
const std::string rig_signature(64, 'a');

// A directory in the temporary directory, removed on destruction.
struct TempDirectory {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("anima-motion-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDirectory() { std::filesystem::create_directory(path); }
    TempDirectory(const TempDirectory &) = delete;
    TempDirectory &operator=(const TempDirectory &) = delete;
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void append_u32(std::vector<char> &bytes, std::uint32_t number) {
    for (unsigned i = 0; i < 4; ++i)
        bytes.push_back(static_cast<char>((number >> (i * 8)) & 255));
}
void append_f32(std::vector<char> &bytes, float number) { append_u32(bytes, std::bit_cast<std::uint32_t>(number)); }
// Writes a binary glTF file of @p json and one @p binary buffer.
void write_glb(const std::filesystem::path &file, std::string json, const std::vector<char> &binary) {
    while (json.size() % 4)
        json += ' ';
    std::vector<char> bytes;
    append_u32(bytes, 0x46546c67); // "glTF"
    append_u32(bytes, 2);
    append_u32(bytes, static_cast<std::uint32_t>(28 + json.size() + binary.size()));
    append_u32(bytes, static_cast<std::uint32_t>(json.size()));
    append_u32(bytes, 0x4e4f534a); // "JSON"
    bytes.insert(bytes.end(), json.begin(), json.end());
    append_u32(bytes, static_cast<std::uint32_t>(binary.size()));
    append_u32(bytes, 0x004e4942); // "BIN"
    bytes.insert(bytes.end(), binary.begin(), binary.end());
    std::ofstream output(file, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(bool(output));
}

// A root with a three-joint limb and a separate side joint. The skin uses every node, and each inverse bind
// cancels the rest pose.
Asset limb_model() {
    Asset result;
    const std::array<const char *, rig_joints> names{"root", "upper", "middle", "end", "side"};
    const std::array<int, rig_joints> parents{-1, 0, 1, 2, 0};
    const std::array<Vec3, rig_joints> offsets{Vec3{0, 0, 0}, Vec3{0, 1, 0}, Vec3{0, -.4F, .1F}, Vec3{0, -.4F, -.1F},
                                               Vec3{.5F, 1, 0}};
    AssetSkin skin;
    for (std::size_t i = 0; i < rig_joints; ++i) {
        AssetNode node;
        node.name = names[i];
        node.parent = parents[i];
        node.rest.translation = offsets[i];
        result.nodes.push_back(node);
        skin.joints.push_back(i);
    }
    const auto rest = sample_pose(result);
    for (const auto &world : rest.world)
        skin.inverse_bind.push_back(inverse(world));
    result.skins.push_back(skin);
    return result;
}

// The limb model's nodes, with a base clip that moves the root 1 unit along +Z over a second, and a layer
// clip for each mask that turns its root joint a quarter turn about +Z.
void write_motion(const std::filesystem::path &file) {
    std::vector<char> binary;
    for (const float number :
         {0.F, 1.F, 0.F, 0.F, 0.F, 0.F, 0.F, 1.F, 0.F, 0.F, 0.F, 1.F, 0.F, 0.F, .70710678F, .70710678F})
        append_f32(binary, number);
    write_glb(file, R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
        "nodes":[{"name":"root","children":[1,4]},{"name":"upper","translation":[0,1,0],"children":[2]},
                 {"name":"middle","translation":[0,-0.4,0.1],"children":[3]},{"name":"end","translation":[0,-0.4,-0.1]},
                 {"name":"side","translation":[0.5,1,0]}],
        "buffers":[{"byteLength":64}],
        "bufferViews":[{"buffer":0,"byteLength":8},{"buffer":0,"byteOffset":8,"byteLength":24},
                       {"buffer":0,"byteOffset":32,"byteLength":32}],
        "accessors":[{"bufferView":0,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},
                     {"bufferView":1,"componentType":5126,"count":2,"type":"VEC3"},
                     {"bufferView":2,"componentType":5126,"count":2,"type":"VEC4"}],
        "animations":[
          {"name":"base","samplers":[{"input":0,"output":1}],"channels":[{"sampler":0,"target":{"node":0,"path":"translation"}}]},
          {"name":"layer.limb","samplers":[{"input":0,"output":2}],"channels":[{"sampler":0,"target":{"node":1,"path":"rotation"}}]},
          {"name":"layer.side","samplers":[{"input":0,"output":2}],"channels":[{"sampler":0,"target":{"node":4,"path":"rotation"}}]}]})",
              binary);
}

// A limb model bound to its motion: the motion GLB on disk, the manifest and the contract that names it.
struct MotionFixture {
    TempDirectory directory;
    std::shared_ptr<const Asset> body = std::make_shared<const Asset>(limb_model());
    Manifest manifest;
    std::string contract = R"({"version":3,"skeleton":{"id":"test.rig","bind_signature":")" + rig_signature +
                           R"(","joint_count":5},"resource":"motion.glb",
        "evaluation":{"version":1,"id":"test.evaluation",
          "parents":{"root":null,"upper":"root","middle":"upper","end":"middle","side":"root"},
          "masks":{"limb":["upper"],"side":["side"]},
          "chains":{"limb":{"joints":["upper","middle","end"],"minimum_angle":0,"maximum_angle":3.1}}},
        "clips":[{"name":"base","loop":true,"events":[]}],
        "layers":{"layer.limb":{"mask":"limb","owned_joints":["end","middle","upper"],"context_joints":["root"]},
                  "layer.side":{"mask":"side","owned_joints":["side"],"context_joints":["root"]}}})";
    MotionFixture() {
        write_motion(directory.path / "motion.glb");
        manifest.directory = directory.path;
        manifest.asset_id = "test.body";
        manifest.model = "body.glb";
        manifest.skeleton_id = "test.rig";
        manifest.bind_signature = rig_signature;
        manifest.joint_count = rig_joints;
        manifest.motion_contract = "motion.json";
    }
    [[nodiscard]] MotionRuntime runtime() const { return {body, manifest, contract}; }
};
} // namespace

TEST_CASE("A motion contract binds to a model that carries clips of its own, and ignores them") {
    const MotionFixture fixture;
    auto animated = limb_model();
    Animation own;
    own.name = "model.clip";
    own.duration = 1;
    own.channels.push_back({0, ChannelPath::translation, Interpolation::linear, {0, 1}, {{0, 0, 0, 0}, {5, 0, 0, 0}}});
    animated.animations.push_back(own);
    auto listed = fixture.manifest;
    listed.clips.push_back({"model.clip", false, {}});
    const MotionRuntime runtime(std::make_shared<const Asset>(animated), listed, fixture.contract);
    CHECK(runtime.clips().size() == 1);
    CHECK(runtime.clips().contains("base"));
    CHECK_THROWS_WITH_AS(runtime.clip("model.clip"), "Missing animation: model.clip", std::out_of_range);
    // The base clip of the motion resource moves the root; the model's own clip does not.
    const auto sampled = runtime.sample("base", .5);
    CHECK(sampled.world[0][12] == Near{0, pose_tolerance});
    CHECK(sampled.world[0][14] == Near{.5, pose_tolerance});
}
