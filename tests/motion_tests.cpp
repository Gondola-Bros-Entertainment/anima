#include "near.hpp"
#include <anima/assets/action_runtime.hpp>
#include <anima/assets/actor_presentation.hpp>
#include <anima/assets/attachments.hpp>
#include <anima/assets/interaction_runtime.hpp>
#include <anima/assets/motion_runtime.hpp>
#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace anima;
namespace {
constexpr float pose_tolerance = 1e-5F; // Absolute error allowed in sampled matrix elements.
constexpr std::size_t rig_joints = 9;
// A bind signature is opaque to MotionRuntime; the contract only has to repeat the manifest's.
const std::string rig_signature(64, 'a');
constexpr auto identity_frame = "[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]";

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

// A root with a three-joint limb, a separate side joint that carries a tip, and another three-joint limb. The skin
// uses every node, and each inverse bind cancels the rest pose.
Asset limb_model() {
    Asset result;
    const std::array<const char *, rig_joints> names{"root", "upper",       "middle",       "end",      "side",
                                                     "tip",  "other.upper", "other.middle", "other.end"};
    const std::array<int, rig_joints> parents{-1, 0, 1, 2, 0, 4, 0, 6, 7};
    const std::array<Vec3, rig_joints> offsets{Vec3{0, 0, 0},       Vec3{0, 1, 0},      Vec3{0, -.4F, .1F},
                                               Vec3{0, -.4F, -.1F}, Vec3{.5F, 1, 0},    Vec3{.2F, 0, 0},
                                               Vec3{-.5F, 1, 0},    Vec3{0, -.4F, .1F}, Vec3{0, -.4F, -.1F}};
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

// Appends a node named @p name to @p model, at @p offset from node @p parent, or from the model's origin for -1.
void add_node(Asset &model, const char *name, int parent, Vec3 offset) {
    AssetNode node;
    node.name = name;
    node.parent = parent;
    node.rest.translation = offset;
    model.nodes.push_back(node);
}
// Nodes that helper_model adds to the limb model, outside the skin and the contract: a socket node below the limb's end
// joint, a node below that, and a marker at the root of the model.
constexpr std::size_t helper_socket = rig_joints, helper_socket_tip = rig_joints + 1, helper_marker = rig_joints + 2;
// Offset of each socket node from its parent.
const Vec3 helper_offset{0, -.1F, 0};
Asset helper_model() {
    constexpr int limb_end = 3;
    auto result = limb_model();
    add_node(result, "end.socket", limb_end, helper_offset);
    add_node(result, "end.socket.tip", static_cast<int>(helper_socket), helper_offset);
    add_node(result, "marker", -1, {1, 0, 0});
    return result;
}

// The limb model's nodes, with a base clip that moves the root 1 unit along +Z over a second, and a layer
// clip for each mask that turns its root joint a quarter turn about +Z.
constexpr auto motion_document = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
        "nodes":[{"name":"root","children":[1,4,6]},{"name":"upper","translation":[0,1,0],"children":[2]},
                 {"name":"middle","translation":[0,-0.4,0.1],"children":[3]},{"name":"end","translation":[0,-0.4,-0.1]},
                 {"name":"side","translation":[0.5,1,0],"children":[5]},{"name":"tip","translation":[0.2,0,0]},
                 {"name":"other.upper","translation":[-0.5,1,0],"children":[7]},
                 {"name":"other.middle","translation":[0,-0.4,0.1],"children":[8]},
                 {"name":"other.end","translation":[0,-0.4,-0.1]}],
        "buffers":[{"byteLength":64}],
        "bufferViews":[{"buffer":0,"byteLength":8},{"buffer":0,"byteOffset":8,"byteLength":24},
                       {"buffer":0,"byteOffset":32,"byteLength":32}],
        "accessors":[{"bufferView":0,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},
                     {"bufferView":1,"componentType":5126,"count":2,"type":"VEC3"},
                     {"bufferView":2,"componentType":5126,"count":2,"type":"VEC4"}],
        "animations":[
          {"name":"base","samplers":[{"input":0,"output":1}],"channels":[{"sampler":0,"target":{"node":0,"path":"translation"}}]},
          {"name":"layer.limb","samplers":[{"input":0,"output":2}],"channels":[{"sampler":0,"target":{"node":1,"path":"rotation"}}]},
          {"name":"layer.side","samplers":[{"input":0,"output":2}],"channels":[{"sampler":0,"target":{"node":4,"path":"rotation"}}]}]})";
// Writes a motion GLB of @p document, whose buffer holds the keys that motion_document reads: the times 0 and 1, two
// translations and two rotations.
void write_motion(const std::filesystem::path &file, const std::string &document = motion_document) {
    std::vector<char> binary;
    for (const float number :
         {0.F, 1.F, 0.F, 0.F, 0.F, 0.F, 0.F, 1.F, 0.F, 0.F, 0.F, 1.F, 0.F, 0.F, .70710678F, .70710678F})
        append_f32(binary, number);
    write_glb(file, document, binary);
}

// A one-triangle model for attachment visuals, on one node named @p node; an @p animated one has a clip that moves
// it.
void write_prop(const std::filesystem::path &file, bool animated, std::string_view node = "prop") {
    std::vector<char> binary;
    for (const float number : {0.F, 0.F, 0.F, 1.F, 0.F, 0.F, 0.F, 1.F, 0.F, 0.F, 1.F, 0.F, 0.F, 0.F, 0.F, 0.F, 1.F})
        append_f32(binary, number);
    write_glb(file,
              std::string(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
        "nodes":[{"name":")") +
                  std::string(node) +
                  R"(","mesh":0}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],
        "buffers":[{"byteLength":68}],
        "bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":8},
                       {"buffer":0,"byteOffset":44,"byteLength":24}],
        "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
                     {"bufferView":1,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},
                     {"bufferView":2,"componentType":5126,"count":2,"type":"VEC3"}])" +
                  (animated ? R"(,"animations":[{"name":"turn","samplers":[{"input":1,"output":2}],
                         "channels":[{"sampler":0,"target":{"node":0,"path":"translation"}}]}]})"
                            : "}"),
              binary);
}

// An attachment catalog of two items held at the grip socket. Both layer the limb, except that the prop's
// grip profile layers the side joint over the base clip instead. The spinner visual's model is animated but
// declares no tracks, so it cannot load.
std::string attachment_catalog() {
    constexpr auto unbound = R"("primary_node":null,"marker_nodes":{},"animation_tracks":{})";
    return std::string(R"({"version":4,"units":"meters","empty_handling":"free",
        "handling":[{"id":"free","socket":"","layer":"","layer_overrides":{},"support_contacts":[]},
                    {"id":"grip","socket":"grip","layer":"layer.limb","layer_overrides":{"base":"layer.side"},
                     "support_contacts":[]},
                    {"id":"brace","socket":"grip","layer":"layer.limb","layer_overrides":{},"support_contacts":[]}],
        "visuals":[{"id":"prop","model":"prop.glb","primary_grip":)") +
           identity_frame + R"(,"markers":{},)" + unbound +
           R"(},{"id":"spinner","model":"spinner.glb","primary_grip":)" + identity_frame + R"(,"markers":{},)" +
           unbound + R"(}],
        "items":[{"id":"prop","visual":"prop","handling":"grip"},{"id":"brace","visual":"prop","handling":"brace"}]})";
}
// @p text with its first @p from replaced by @p to; the text must contain @p from.
std::string replaced(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to);
}
// @p text with its first field @p name renamed, so that the object holding it lacks it.
std::string without(const std::string &text, std::string_view name) {
    const auto key = "\"" + std::string(name) + "\":";
    return replaced(text, key, "\"" + std::string(name) + "_renamed\":");
}
// attachment_catalog, with the brace held at a hold socket while a support contact puts the limb's end on the grip
// socket, at the prop's support marker.
std::string braced_catalog() {
    return replaced(
        replaced(attachment_catalog(),
                 R"({"id":"brace","socket":"grip","layer":"layer.limb","layer_overrides":{},"support_contacts":[]})",
                 R"({"id":"brace","socket":"hold","layer":"","layer_overrides":{},"support_contacts":[
                     {"chain":"limb","socket":"grip","marker":"support","pole":[0,0,2],"clips":["base"],
                      "actions":[]}]})"),
        R"("markers":{})", std::string(R"("markers":{"support":)") + identity_frame + "}");
}
// An interaction in which the child role is placed by its anchor socket on the parent's while a contact moves its
// limb toward the parent's target socket.
constexpr auto meeting = R"({"version":2,"id":"meet","phases":[{"id":"hold","duration":1,"held":false,"cues":[]}],
    "roles":{"child":{"hold":{"layers":[{"clip":"base","mask":null,"interval":[0,1],"mode":"override",
                                         "weight":[[0,1],[1,1]],"reference":null}]}},
             "parent":{"hold":{"layers":[{"clip":"base","mask":null,"interval":[0,1],"mode":"override",
                                          "weight":[[0,1],[1,1]],"reference":null}]}}},
    "attachments":[{"child":"child","parent":"parent","child_socket":"anchor","parent_socket":"anchor",
                    "weights":{"hold":[[0,1],[1,1]]}}],
    "contacts":[{"child":"child","parent":"parent","chain":"limb","target_socket":"target","pole":[0,0,2],
                 "weights":{"hold":[[0,1],[1,1]]},"orientation":false}]})";

// Two actions on the limb. reach needs a tool role held with the grip profile and may be performed with
// either profile; twirl needs the tool's visual to have a spin track, which the prop lacks.
constexpr auto action_catalog = R"({"version":2,"actions":[
    {"id":"reach","handling":["free","grip"],"roles":{"tool":["grip"]},"phases":[
      {"id":"extend","duration":0.5,"held":false,
       "layers":[{"clip":"layer.limb","mask":"limb","interval":[0,1],"mode":"override","weight":[[0,1],[1,1]],
                  "reference":null}],
       "cues":[],"props":[{"role":"tool","track":"spin","interval":[0,1],"required":false}],"contacts":{}}]},
    {"id":"twirl","handling":["grip"],"roles":{},"phases":[
      {"id":"spin","duration":0.5,"held":false,
       "layers":[{"clip":"layer.limb","mask":"limb","interval":[0,1],"mode":"override","weight":[[0,1],[1,1]],
                  "reference":null}],
       "cues":[],"props":[{"role":"tool","track":"spin","interval":[0,1],"required":true}],"contacts":{}}]}]})";

// A limb model bound to its motion: the motion GLB on disk, the manifest and the contract that names it, and
// a prop model with a grip socket at the end of the limb.
struct MotionFixture {
    TempDirectory directory;
    std::shared_ptr<const Asset> body = std::make_shared<const Asset>(limb_model());
    Manifest manifest;
    std::string contract = R"({"version":4,"skeleton":{"id":"test.rig","bind_signature":")" + rig_signature +
                           R"(","joint_count":)" + std::to_string(rig_joints) + R"(},"resource":"motion.glb",
        "evaluation":{"version":1,"id":"test.evaluation",
          "parents":{"root":null,"upper":"root","middle":"upper","end":"middle","side":"root","tip":"side",
                     "other.upper":"root","other.middle":"other.upper","other.end":"other.middle"},
          "masks":{"limb":["upper"],"side":["side"]},
          "chains":{"limb":{"joints":["upper","middle","end"],"minimum_angle":0,"maximum_angle":3.1},
                    "other":{"joints":["other.upper","other.middle","other.end"],"minimum_angle":0,"maximum_angle":3.1}}},
        "clips":[{"name":"base","loop":true,"reference_speed":null,"events":[]}],
        "layers":{"layer.limb":{"mask":"limb","owned_joints":["end","middle","upper"],"context_joints":["root"]},
                  "layer.side":{"mask":"side","owned_joints":["side","tip"],"context_joints":["root"]}}})";
    AttachmentSockets sockets{{"grip", {3, identity()}}};
    MotionFixture() {
        write_motion(directory.path / "motion.glb");
        write_prop(directory.path / "prop.glb", false);
        write_prop(directory.path / "spinner.glb", true);
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

// The limb model and its motion with the side joint hidden by a scale of 0 at rest, which collapses the tip below it.
struct HiddenSide {
    std::shared_ptr<const Asset> body;
    std::shared_ptr<const MotionRuntime> motion;
};
// Binds @p fixture's contract to a limb model whose side joint is hidden, through a motion GLB that hides it too.
HiddenSide hidden_side(const MotionFixture &fixture) {
    constexpr std::size_t side = 4;
    auto model = limb_model(); // Nothing here reads the inverse binds, which the visible side joint gave.
    model.nodes[side].rest.scale = {0, 0, 0};
    write_motion(fixture.directory.path / "hidden.glb",
                 replaced(motion_document, R"({"name":"side","translation":[0.5,1,0],)",
                          R"({"name":"side","translation":[0.5,1,0],"scale":[0,0,0],)"));
    HiddenSide result{std::make_shared<const Asset>(std::move(model)), nullptr};
    result.motion = std::make_shared<const MotionRuntime>(
        result.body, fixture.manifest,
        replaced(fixture.contract, R"("resource":"motion.glb")", R"("resource":"hidden.glb")"));
    return result;
}
// Checks that the side joint and the tip below it stay collapsed at the side joint's offset from the root in @p pose.
void check_hidden_side(const Pose &pose) {
    constexpr std::size_t root = 0, side = 4, tip = 5;
    const auto at = point(pose.world[root], {.5F, 1, 0});
    for (const auto node : {side, tip}) {
        CAPTURE(node);
        CHECK(length(point(pose.world[node], {}) - at) < pose_tolerance);
        CHECK(length(axis_x(pose.world[node])) < pose_tolerance);
        CHECK(length(axis_y(pose.world[node])) < pose_tolerance);
        CHECK(length(axis_z(pose.world[node])) < pose_tolerance);
    }
}
// Whether attachment_placement accepts a @p Frame. A socket is where the grip goes, not the prop's origin, so a
// socket must go through bind_attachment first.
template <typename Frame>
concept PlacesProp = requires(const Pose &pose, const Frame &frame) { attachment_placement(pose, frame); };
static_assert(PlacesProp<AttachmentBinding> && !PlacesProp<AttachmentSocket>);
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

TEST_CASE("Evaluation applies as many layers, offsets and contacts as the caller supplies") {
    const MotionFixture fixture;
    const auto runtime = fixture.runtime();
    const auto base = runtime.sample("base", 0);
    MotionLayer layer;
    layer.clip = "layer.limb";
    layer.mask = "limb";
    layer.time = .5;
    layer.weight = .5F;
    Transform step;
    step.translation = {.01F, 0, 0};
    MotionContact contact;
    contact.chain = "limb";
    contact.target = point(base.world[3], {});
    contact.pole = {0, 0, 2};
    MotionControls controls;
    controls.layers.assign(9, layer);
    controls.offsets.assign(33, {.joint = "side", .delta = matrix(step)});
    controls.contacts.assign(9, contact);
    const auto evaluated = runtime.evaluate(base, controls);
    CHECK(evaluated.contacts.size() == 9);
    // The layers leave the side joint alone, so the offsets move it 33 steps from its rest position.
    CHECK(evaluated.pose.world[4][12] == Near{.83, pose_tolerance});
}

TEST_CASE("An attachment set prepares as many roles as the caller names") {
    const MotionFixture fixture;
    const AttachmentLibrary library(decode_attachment_catalog(attachment_catalog(), fixture.directory.path));
    std::map<std::string, std::string, std::less<>> desired;
    for (unsigned i = 0; i < 9; ++i)
        desired.emplace("role." + std::to_string(i), "prop");
    const auto prepared = AttachmentSet::prepare(library, fixture.sockets, desired);
    CHECK(prepared.roles.size() == 9);
    CHECK(prepared.matches(desired));
    CHECK(library.resident_meshes().size() == 1); // Every role shares the one loaded model.
}

TEST_CASE("An attachment library compiles its models with the texel retention it was given") {
    const MotionFixture fixture;
    auto catalog = decode_attachment_catalog(attachment_catalog(), fixture.directory.path);
    CHECK_THROWS_WITH_AS(AttachmentLibrary(catalog, static_cast<TexelRetention>(2)), "Unknown texel retention",
                         std::invalid_argument);
    const AttachmentLibrary kept(catalog);
    CHECK(kept.load("prop")->render->texel_retention() == TexelRetention::keep);
    const AttachmentLibrary released(std::move(catalog), TexelRetention::until_upload);
    const auto loaded = released.load("prop");
    CHECK(loaded->render->texel_retention() == TexelRetention::until_upload);
    CHECK(loaded->source->textures.size() == loaded->render->materials()->textures.size());
    CHECK(released.load("prop")->source == loaded->source); // Loads still share the model while it lives.
}

TEST_CASE("A load shares the Mesh that a scene still draws after the attachment set that loaded it is gone") {
    const MotionFixture fixture;
    const AttachmentLibrary library(decode_attachment_catalog(attachment_catalog(), fixture.directory.path));
    Scene scene;
    std::weak_ptr<const AttachmentAsset> dropped;
    std::weak_ptr<const Mesh> drawn;
    {
        auto held = AttachmentSet::prepare(library, fixture.sockets, {{"tool", "prop"}});
        held.add(scene);
        dropped = held.roles.at("tool").asset;
        drawn = held.roles.at("tool").asset->render;
    }
    // Only the scene keeps the Mesh. A load imports the file again and keeps the Mesh.
    REQUIRE(dropped.expired());
    REQUIRE_FALSE(drawn.expired());
    auto reloaded = library.load("prop");
    CHECK(reloaded->render == drawn.lock());
    CHECK(library.resident_meshes() == std::vector{drawn.lock()});
    // A file whose nodes changed while its Mesh lived compiles a new Mesh, and both stay resident.
    reloaded.reset();
    write_prop(fixture.directory.path / "prop.glb", false, "renamed");
    const auto changed = library.load("prop");
    CHECK(changed->render != drawn.lock());
    CHECK(changed->source->nodes.front().name == "renamed");
    CHECK(library.resident_meshes().size() == 2);
}

TEST_CASE("Loads on several threads share one import per file, and a failed load is tried again") {
    const MotionFixture fixture;
    auto catalog = decode_attachment_catalog(attachment_catalog(), fixture.directory.path);
    // A visual with a file of its own, and one whose file does not exist until the test writes it.
    for (const std::string id : {"other", "late"}) {
        AttachmentVisual visual;
        visual.id = id;
        visual.model = id + ".glb";
        catalog.visuals.emplace(id, std::move(visual));
    }
    write_prop(fixture.directory.path / "other.glb", false);
    const AttachmentLibrary library(std::move(catalog));
    const AttachmentLibrary copy = library; // Copies share the loaded models.
    constexpr std::array<std::string_view, 6> visuals{"prop", "prop", "other", "other", "late", "late"};
    std::array<std::shared_ptr<const AttachmentAsset>, visuals.size()> loaded;
    std::array<std::string, visuals.size()> failures;
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < visuals.size(); ++i)
        threads.emplace_back([&, i] {
            try {
                loaded.at(i) = (i % 2 ? copy : library).load(visuals.at(i));
            } catch (const std::runtime_error &error) {
                failures.at(i) = error.what();
            } catch (const std::exception &error) {
                failures.at(i) = std::string("Not a std::runtime_error: ") + error.what();
            }
        });
    for (auto &thread : threads)
        thread.join();
    REQUIRE(loaded[0]);
    REQUIRE(loaded[2]);
    CHECK(loaded[1] == loaded[0]);
    CHECK(loaded[3] == loaded[2]);
    CHECK(loaded[2]->render != loaded[0]->render);
    // Each load of the missing file fails, whether it shared another load's import or ran its own.
    CHECK(failures[4] == "Cannot open GLB file");
    CHECK(failures[5] == "Cannot open GLB file");
    CHECK(library.resident_meshes().size() == 2);
    CHECK_THROWS_WITH_AS(library.load("late"), "Cannot open GLB file", std::runtime_error);
    write_prop(fixture.directory.path / "late.glb", false);
    const auto late = copy.load("late");
    CHECK(late->render != loaded[0]->render);
    CHECK(library.resident_meshes().size() == 3);
}

TEST_CASE("An animated grip stays in prop model space, where its node carries it from the node's rest placement") {
    // A prop whose primary node rests away from the origin, turned and moved below a turned and moved root.
    Asset prop;
    add_node(prop, "base", -1, {.3F, -.2F, .1F});
    prop.nodes[0].rest.rotation = {0, .38268343F, 0, .92387953F}; // An eighth turn about +Y.
    add_node(prop, "handle", 0, {0, .5F, .2F});
    prop.nodes[1].rest.rotation = {.25881905F, 0, 0, .96592583F}; // A twelfth turn about +X.
    AttachmentVisual visual;
    visual.primary_node = "handle";
    Transform grip;
    grip.translation = {.05F, .1F, 0};
    grip.rotation = {0, 0, .70710678F, .70710678F}; // A quarter turn about +Z.
    visual.primary_grip = matrix(grip);
    Transform frame;
    frame.translation = {0, -.1F, .05F};
    const AttachmentSocket socket{3, matrix(frame)};
    const auto bound = bind_attachment(socket, visual);
    // At the prop's rest pose, the grip is where bind_attachment put it.
    const auto rest = sample_pose(prop);
    const auto at_rest = animated_attachment_binding(bound, visual, prop, rest);
    CHECK(at_rest.node == bound.node);
    for (std::size_t i = 0; i < at_rest.local.size(); ++i)
        CHECK(at_rest.local[i] == Near{bound.local[i], 1e-6});
    // Moving the node from its rest placement moves the grip with it, and the moved grip meets the socket.
    auto local = rest.local;
    local[1].translation = {.1F, .4F, .3F};
    local[1].rotation = {0, 0, .38268343F, .92387953F};
    const auto moved = pose_from_local(prop, local);
    const auto body = sample_pose(limb_model());
    const auto placement = attachment_placement(body, animated_attachment_binding(bound, visual, prop, moved));
    const auto held = placement * moved.world[1] * inverse(rest.world[1]) * visual.primary_grip;
    const auto expected = body.world[socket.node] * socket.local;
    for (std::size_t i = 0; i < held.size(); ++i)
        CHECK(held[i] == Near{expected[i], 1e-6});
}

TEST_CASE("Replacing an attachment counts a removed instance as gone, and checks the scene before adding") {
    const MotionFixture fixture;
    const AttachmentLibrary library(decode_attachment_catalog(attachment_catalog(), fixture.directory.path));
    Scene scene;
    AttachmentInstance held;
    REQUIRE(held.replace(scene, library, fixture.sockets, "prop"));
    scene.remove(*held.instance);
    CHECK(held.replace(scene, library, fixture.sockets, "brace"));
    CHECK(held.item_id == "brace");
    CHECK(scene.size() == 1);
    CHECK(scene.contains(*held.instance));
    // An instance of another scene is rejected before the new item is loaded or added.
    Scene other;
    CHECK_THROWS_WITH_AS(held.replace(other, library, fixture.sockets, "prop"),
                         "Attachment instance belongs to another scene", std::invalid_argument);
    CHECK(other.size() == 0);
    CHECK(held.item_id == "brace");
    CHECK(scene.contains(*held.instance));
    // An instance removed with its parent is gone too, so emptying the role changes nothing else.
    auto parent = scene.create("Parent");
    scene.object(*held.instance).set_parent(parent, ReparentMode::keep_local);
    parent.destroy();
    CHECK(held.replace(scene, library, fixture.sockets, ""));
    CHECK_FALSE(held.instance);
    CHECK(scene.size() == 0);
}

TEST_CASE("An attachment follower keeps its items on their sockets as the owner's pose and visibility change") {
    const MotionFixture fixture;
    const AttachmentLibrary library(decode_attachment_catalog(attachment_catalog(), fixture.directory.path));
    const auto set = AttachmentSet::prepare(library, fixture.sockets, {{"tool", "prop"}});
    const auto binding = set.roles.at("tool").binding;
    Scene scene;
    auto owner = scene.create("Owner", Mesh::compile(*fixture.body));
    owner.set_position({2, 0, 0});
    const auto follower = owner.add_component<AttachmentFollower>(set);
    const auto item = follower->object("tool");
    REQUIRE(item.parent());
    CHECK(item.parent() == owner);
    const auto rest = sample_pose(*fixture.body);
    CHECK(item.local_matrix() == attachment_placement(rest, binding));
    // Turning the limb moves its end, which carries the grip socket, and the late update moves the item with it.
    auto local = rest.local;
    local[1].rotation = {0, 0, .38268343F, .92387953F}; // An eighth turn about +Z.
    const auto turned = pose_from_local(*fixture.body, local);
    REQUIRE(attachment_placement(turned, binding) != attachment_placement(rest, binding));
    owner.renderer().set_pose(turned);
    scene.update(.1);
    CHECK(item.local_matrix() == attachment_placement(turned, binding));
    CHECK(item.position().x == Near{2 + attachment_placement(turned, binding)[12], pose_tolerance});
    // A hidden owner hides the item and leaves it in place until the owner shows again.
    owner.renderer().set_visible(false);
    owner.renderer().set_pose(rest);
    scene.update(.1);
    CHECK_FALSE(scene.instance(item.id()).visible);
    CHECK(item.local_matrix() == attachment_placement(turned, binding));
    owner.renderer().set_visible(true);
    scene.update(.1);
    CHECK(scene.instance(item.id()).visible);
    CHECK(item.local_matrix() == attachment_placement(rest, binding));
    // A binding chosen for the role, such as an animated prop's, replaces the set's, which stays available.
    Transform frame;
    frame.translation = {0, .2F, .1F};
    frame.rotation = {0, .38268343F, 0, .92387953F}; // An eighth turn about +Y.
    const AttachmentBinding chosen{5, matrix(frame)};
    follower->set_binding("tool", chosen);
    follower->sync();
    CHECK(item.local_matrix() == attachment_placement(rest, chosen));
    CHECK(follower->attachments().roles.at("tool").binding.node == binding.node);
    CHECK(follower->attachments().roles.at("tool").binding.local == binding.local);
    follower->set_binding("tool", follower->attachments().roles.at("tool").binding);
    follower->sync();
    CHECK(item.local_matrix() == attachment_placement(rest, binding));
}

TEST_CASE("An attachment follower checks its roles, bindings and owner, and destroys the items it holds") {
    const MotionFixture fixture;
    const AttachmentLibrary library(decode_attachment_catalog(attachment_catalog(), fixture.directory.path));
    const auto set = AttachmentSet::prepare(library, fixture.sockets, {{"tool", "prop"}, {"spare", "brace"}});
    Scene scene;
    const auto mesh = Mesh::compile(*fixture.body);
    // An owner without a mesh adds nothing.
    auto bare = scene.create("Bare");
    CHECK_THROWS_WITH_AS(bare.add_component<AttachmentFollower>(set), "Attachment owner requires a mesh",
                         std::invalid_argument);
    CHECK(scene.size() == 1);
    auto owner = scene.create("Owner", mesh);
    const auto follower = owner.add_component<AttachmentFollower>(set);
    CHECK(scene.size() == 4);
    auto item = follower->object("tool");
    const auto placed = item.local_matrix();
    CHECK_THROWS_WITH_AS(follower->object("absent"), "Unknown attachment role: absent", std::out_of_range);
    const AttachmentBinding beyond{rig_joints, identity()};
    CHECK_THROWS_WITH_AS(follower->set_binding("absent", beyond), "Unknown attachment role: absent", std::out_of_range);
    CHECK_THROWS_WITH_AS(follower->set_binding("tool", beyond), "Attachment binding node is outside the owner mesh",
                         std::out_of_range);
    auto projective = identity();
    projective[3] = .5F;
    CHECK_THROWS_WITH_AS(follower->set_binding("tool", {0, projective}),
                         "Attachment binding frame must be finite and affine", std::invalid_argument);
    auto distant = identity();
    distant[12] = std::numeric_limits<float>::infinity();
    CHECK_THROWS_WITH_AS(follower->set_binding("tool", {0, distant}),
                         "Attachment binding frame must be finite and affine", std::invalid_argument);
    follower->sync();
    CHECK(item.local_matrix() == placed); // The rejected bindings changed nothing.
    // Replacing the owner's mesh, even with one compiled from the same model, stops the follower until the original
    // returns.
    owner.renderer().set_mesh(Mesh::compile(*fixture.body));
    CHECK_THROWS_WITH_AS(follower->sync(), "Attachment follower requires its original owner mesh",
                         std::invalid_argument);
    owner.renderer().set_mesh(mesh);
    follower->sync();
    // An item without a renderer is rejected before any item changes.
    owner.renderer().set_visible(false);
    item.remove_mesh();
    CHECK_THROWS_WITH_AS(follower->sync(), "GameObject has no MeshRenderer", std::logic_error);
    CHECK(scene.instance(follower->object("spare").id()).visible);
    // An item destroyed directly is skipped, and removing the follower destroys the other, wherever it was moved.
    item.destroy();
    follower->sync();
    auto spare = follower->object("spare");
    CHECK_FALSE(scene.instance(spare.id()).visible);
    auto shelf = scene.create("Shelf");
    spare.set_parent(shelf);
    CHECK(owner.remove_component<AttachmentFollower>());
    CHECK_FALSE(spare.valid());
    CHECK(scene.size() == 3);
    // A follower outlives its owner: it then fails to sync, and its destruction changes nothing.
    auto standalone = std::make_unique<AttachmentFollower>(owner, set);
    CHECK(scene.size() == 5);
    owner.destroy();
    CHECK(scene.size() == 2);
    CHECK_THROWS_WITH_AS(standalone->sync(), "Expired GameObject handle", std::out_of_range);
    standalone.reset();
    CHECK(scene.size() == 2);
}

TEST_CASE("Layer clips compose over a base clip on disjoint masks, and overlapping masks are rejected") {
    const MotionFixture fixture;
    const auto runtime = fixture.runtime();
    const std::array<std::string_view, 2> both{"layer.limb", "layer.side"};
    const auto composed = runtime.compose_layers("base", .5, both);
    const auto limb_only = runtime.compose("base", .5, "layer.limb");
    // Each layer clip is sampled halfway, an eighth turn about +Z, on its own mask only.
    constexpr float eighth_turn_cosine = .70710678F;
    CHECK(composed.world[1][0] == Near{eighth_turn_cosine, pose_tolerance});
    CHECK(composed.world[4][0] == Near{eighth_turn_cosine, pose_tolerance});
    CHECK(limb_only.world[1][0] == Near{eighth_turn_cosine, pose_tolerance});
    CHECK(limb_only.world[4][0] == Near{1, pose_tolerance});
    CHECK(composed.world[0][14] == Near{.5, pose_tolerance}); // The base clip still moves the root.
    const std::array<std::string_view, 2> overlapping{"layer.limb", "layer.limb"};
    CHECK_THROWS_WITH_AS(runtime.validate_layers(overlapping), "Motion layers have overlapping joint ownership",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(runtime.compose_layers("base", .5, overlapping),
                         "Motion layers have overlapping joint ownership", std::invalid_argument);
    CHECK_THROWS_WITH_AS(runtime.layer_mask("absent"), "Unknown motion layer: absent", std::out_of_range);
}

TEST_CASE("A layer lists exactly its mask's joints and their parents outside it, and animates only those joints") {
    const MotionFixture fixture;
    const auto contract = [&](std::string_view from, std::string_view to) {
        (void)MotionRuntime(fixture.body, fixture.manifest, replaced(fixture.contract, from, to));
    };
    constexpr auto declared = "Invalid motion layer ownership/context";
    // The limb mask owns the upper, middle and end joints, whose parent outside the mask is the root.
    CHECK_THROWS_WITH_AS(contract(R"("owned_joints":["end","middle","upper"])", R"("owned_joints":["middle","upper"])"),
                         declared, std::invalid_argument);
    CHECK_THROWS_WITH_AS(contract(R"("context_joints":["root"])", R"("context_joints":[])"), declared,
                         std::invalid_argument);
    // A clip cannot be both a base clip and a layer clip.
    CHECK_THROWS_WITH_AS(contract(R"("layer.side":{)", R"("base":{)"), declared, std::invalid_argument);
    // The side layer's clip turns the side joint, which the limb mask does not own.
    CHECK_THROWS_WITH_AS(contract(R"("layer.side":{"mask":"side","owned_joints":["side","tip"])",
                                  R"("layer.side":{"mask":"limb","owned_joints":["end","middle","upper"])"),
                         "Motion layer animates joints it does not own", std::invalid_argument);
}

TEST_CASE("An evaluated layer clip keeps the mask its contract declares") {
    const MotionFixture fixture;
    const auto runtime = fixture.runtime();
    MotionLayer layer;
    layer.clip = "layer.limb";
    layer.mask = "side";
    MotionControls controls;
    controls.layers.push_back(layer);
    CHECK_THROWS_WITH_AS(runtime.evaluate(runtime.sample("base", 0), controls),
                         "Layer control exceeds the resource's declared ownership", std::invalid_argument);
}

TEST_CASE("An evaluated additive layer adds its change from the reference clip it names, and an override has none") {
    constexpr std::size_t upper = 1;
    // The layer clip turns the upper joint a quarter turn about +Z over a second: an eighth turn halfway through, and a
    // sixteenth a quarter of the way.
    constexpr float eighth_turn_cosine = .70710678F, eighth_turn_sine = .70710678F;
    constexpr float sixteenth_turn_cosine = .92387953F, sixteenth_turn_sine = .38268343F;
    const MotionFixture fixture;
    const auto runtime = fixture.runtime();
    const auto base = runtime.sample("base", 0);
    MotionLayer layer;
    layer.clip = "layer.limb";
    layer.mask = "limb";
    layer.time = .5;
    layer.mode = LayerMode::additive;
    MotionControls controls;
    controls.layers.push_back(layer);
    CHECK_THROWS_WITH_AS(runtime.evaluate(base, controls), "Additive motion layer needs a reference clip",
                         std::invalid_argument);
    // The base clip leaves the upper joint at rest, so relative to it the layer clip adds its whole eighth turn.
    controls.layers.front().reference_clip = "base";
    controls.layers.front().reference_time = .5;
    const auto from_base = runtime.evaluate(base, controls).pose;
    CHECK(from_base.world[upper][0] == Near{eighth_turn_cosine, pose_tolerance});
    CHECK(from_base.world[upper][1] == Near{eighth_turn_sine, pose_tolerance});
    // Relative to its own sixteenth turn, the layer clip adds the other sixteenth.
    controls.layers.front().reference_clip = "layer.limb";
    controls.layers.front().reference_time = .25;
    const auto from_layer = runtime.evaluate(base, controls).pose;
    CHECK(from_layer.world[upper][0] == Near{sixteenth_turn_cosine, pose_tolerance});
    CHECK(from_layer.world[upper][1] == Near{sixteenth_turn_sine, pose_tolerance});
    controls.layers.front().mode = LayerMode::override_pose;
    CHECK_THROWS_WITH_AS(runtime.evaluate(base, controls), "Override layer cannot have an additive reference",
                         std::invalid_argument);
    controls.layers.front() = {.clip = "base", .mask = "absent"};
    CHECK_THROWS_WITH_AS(runtime.evaluate(base, controls), "Unknown motion mask: absent", std::out_of_range);
}

TEST_CASE("An evaluated layer with an empty mask covers every joint, and nodes outside the rig follow its clip") {
    constexpr std::size_t root = 0, end = 3;
    const Vec3 moved_offset{.3F, 0, 0};
    const MotionFixture fixture;
    const auto model = std::make_shared<const Asset>(helper_model());
    const MotionRuntime runtime(model, fixture.manifest, fixture.contract);
    // The source carries the socket node below the limb's end away from its rest offset, which the clip keeps.
    auto local = runtime.sample("base", 0).local;
    local[helper_socket].translation = moved_offset;
    const auto source = pose_from_local(*model, local);
    MotionControls controls;
    controls.layers.push_back({.clip = "base", .mask = "", .time = .5, .weight = .5F});
    const auto evaluated = runtime.evaluate(source, controls).pose;
    // The root, which no mask names, moves halfway to the clip's half-second position.
    CHECK(evaluated.world[root][14] == Near{.25, pose_tolerance});
    CHECK(length(point(evaluated.world[helper_socket], {}) - point(evaluated.world[end], helper_offset)) <
          pose_tolerance);
    CHECK(evaluated.world == runtime.blend(source, runtime.sample("base", .5), .5F).world);
    // Under a masked layer, the socket node keeps the source's offset.
    controls.layers.front().mask = "limb";
    const auto masked = runtime.evaluate(source, controls).pose;
    CHECK(masked.world[root][14] == Near{0, pose_tolerance});
    CHECK(length(point(masked.world[helper_socket], {}) - point(masked.world[end], moved_offset)) < pose_tolerance);
    // A layer clip keeps its declared mask, so it cannot cover every joint.
    controls.layers.front() = {.clip = "layer.limb", .mask = ""};
    CHECK_THROWS_WITH_AS(runtime.evaluate(source, controls), "Layer control exceeds the resource's declared ownership",
                         std::invalid_argument);
}

TEST_CASE("A motion contract rejects a base clip whose keys all sit at time 0") {
    const MotionFixture fixture;
    // The base clip samples a single key of each accessor, at time 0, so the motion GLB gives it a duration of 0.
    write_motion(fixture.directory.path / "pose.glb",
                 replaced(replaced(motion_document, R"({"name":"base","samplers":[{"input":0,"output":1}])",
                                   R"({"name":"base","samplers":[{"input":3,"output":4}])"),
                          R"({"bufferView":2,"componentType":5126,"count":2,"type":"VEC4"}])",
                          R"({"bufferView":2,"componentType":5126,"count":2,"type":"VEC4"},
                     {"bufferView":0,"componentType":5126,"count":1,"type":"SCALAR","min":[0],"max":[0]},
                     {"bufferView":1,"componentType":5126,"count":1,"type":"VEC3"}])"));
    CHECK_THROWS_WITH_AS(
        MotionRuntime(fixture.body, fixture.manifest,
                      replaced(fixture.contract, R"("resource":"motion.glb")", R"("resource":"pose.glb")")),
        "Motion requires a positive duration", std::invalid_argument);
}

TEST_CASE("A contact chain answers for every model node below its start joint, evaluation joint or not") {
    constexpr std::size_t root = 0, end = 3;
    // Distance the socket node must travel; the limb's end joint travels about .47 to reach the target.
    constexpr float minimum_travel = .1F;
    const Vec3 target{.3F, .5F, .2F};
    const MotionFixture fixture;
    const auto model = helper_model();
    const MotionRuntime runtime(std::make_shared<const Asset>(model), fixture.manifest, fixture.contract);
    CHECK(runtime.contact_affects_node("limb", end));
    CHECK(runtime.contact_affects_node("limb", helper_socket));
    CHECK(runtime.contact_affects_node("limb", helper_socket_tip));
    CHECK_FALSE(runtime.contact_affects_node("other", helper_socket));
    CHECK_FALSE(runtime.contact_affects_node("limb", root));
    CHECK_FALSE(runtime.contact_affects_node("limb", helper_marker));
    CHECK_THROWS_WITH_AS(runtime.contact_affects_node("limb", model.nodes.size()), "Unknown model node",
                         std::out_of_range);
    CHECK_THROWS_WITH_AS(runtime.contact_affects_node("absent", helper_socket), "Unknown motion chain: absent",
                         std::out_of_range);
    // Solving the chain carries the socket node with the end joint and leaves the marker where it was.
    const auto base = runtime.sample("base", 0);
    MotionControls controls;
    controls.contacts.push_back({"limb", target, {0, 0, 2}, 1, {}});
    const auto solved = runtime.evaluate(base, controls).pose;
    CHECK(length(point(solved.world[helper_socket], {}) - point(solved.world[end], helper_offset)) < pose_tolerance);
    CHECK(length(point(solved.world[helper_socket], {}) - point(base.world[helper_socket], {})) > minimum_travel);
    for (std::size_t i = 0; i < base.world[helper_marker].size(); ++i) {
        CAPTURE(i);
        CHECK(solved.world[helper_marker][i] == Near{base.world[helper_marker][i], pose_tolerance});
    }
}

TEST_CASE("A contact chain answers from the hierarchy the model had when the runtime was constructed") {
    constexpr int end = 3;
    const Vec3 target{.3F, .5F, .2F};
    const MotionFixture fixture;
    const auto model = std::make_shared<Asset>(helper_model());
    const MotionRuntime runtime(model, fixture.manifest, fixture.contract);
    // Moving the marker below the limb's end afterward changes neither the answer nor what solving the limb does to it.
    model->nodes[helper_marker].parent = end;
    CHECK_FALSE(runtime.contact_affects_node("limb", helper_marker));
    const auto base = runtime.sample("base", 0);
    MotionControls controls;
    controls.contacts.push_back({"limb", target, {0, 0, 2}, 1, {}});
    const auto solved = runtime.evaluate(base, controls).pose;
    for (std::size_t i = 0; i < base.world[helper_marker].size(); ++i) {
        CAPTURE(i);
        CHECK(solved.world[helper_marker][i] == Near{base.world[helper_marker][i], pose_tolerance});
    }
}

TEST_CASE("A contact chain answers by the evaluation rig where its hierarchy differs from the model's") {
    constexpr std::size_t tip = 5, tip_socket = rig_joints;
    // Distance the socket node must travel; it travels about .84 with the tip.
    constexpr float minimum_travel = .1F;
    const Vec3 tip_offset{.1F, 0, 0}, target{.3F, .5F, .2F};
    const MotionFixture fixture;
    // The rig hangs the tip from the limb's middle joint while the model keeps it below the side joint, as a flattened
    // export may, so the limb's layer owns the tip and the side's does not.
    auto contract = replaced(fixture.contract, R"("tip":"side")", R"("tip":"middle")");
    contract = replaced(contract, R"("owned_joints":["end","middle","upper"])",
                        R"("owned_joints":["end","middle","tip","upper"])");
    contract = replaced(contract, R"("owned_joints":["side","tip"])", R"("owned_joints":["side"])");
    auto model = limb_model();
    add_node(model, "tip.socket", static_cast<int>(tip), tip_offset);
    const MotionRuntime runtime(std::make_shared<const Asset>(model), fixture.manifest, contract);
    CHECK(runtime.contact_affects_node("limb", tip));
    CHECK(runtime.contact_affects_node("limb", tip_socket));
    CHECK_FALSE(runtime.contact_affects_node("other", tip_socket));
    // Solving the limb carries the tip, and the socket node with it, though neither lies below the limb in the model.
    const auto base = runtime.sample("base", 0);
    MotionControls controls;
    controls.contacts.push_back({"limb", target, {0, 0, 2}, 1, {}});
    const auto solved = runtime.evaluate(base, controls).pose;
    CHECK(length(point(solved.world[tip_socket], {}) - point(solved.world[tip], tip_offset)) < pose_tolerance);
    CHECK(length(point(solved.world[tip_socket], {}) - point(base.world[tip_socket], {})) > minimum_travel);
}

TEST_CASE("Attachment and interaction sockets outside the evaluation rig pass unless a contact moves them") {
    constexpr std::size_t end = 3, other_end = 8;
    const MotionFixture fixture;
    const auto body = std::make_shared<const Asset>(helper_model());
    const auto motion = std::make_shared<const MotionRuntime>(body, fixture.manifest, fixture.contract);
    const auto catalog = braced_catalog();
    const auto other_contact =
        replaced(catalog, R"("chain":"limb","socket":"grip")", R"("chain":"other","socket":"other.grip")");
    const auto ownership = [&](const std::string &document, std::size_t hold) {
        const AttachmentLibrary library(decode_attachment_catalog(document, fixture.directory.path));
        const AttachmentSockets sockets{
            {"grip", {end, identity()}}, {"other.grip", {other_end, identity()}}, {"hold", {hold, identity()}}};
        validate_attachment_ownership(*motion, library, AttachmentSet::prepare(library, sockets, {{"tool", "brace"}}),
                                      sockets);
    };
    CHECK_THROWS_WITH_AS(ownership(catalog, helper_socket), "Attachment contact moves a primary socket",
                         std::invalid_argument);
    CHECK_NOTHROW(ownership(other_contact, helper_socket));
    CHECK_NOTHROW(ownership(catalog, helper_marker));
    const auto parent_motion = std::make_shared<const MotionRuntime>(fixture.runtime());
    const auto interaction = [&](const std::string &document, std::size_t anchor) {
        const InteractionRuntime::Actors actors{
            {"child", {motion, {{"anchor", {anchor, identity()}}}}},
            {"parent", {parent_motion, {{"anchor", {0, identity()}}, {"target", {end, identity()}}}}}};
        (void)InteractionRuntime(actors, document);
    };
    CHECK_THROWS_WITH_AS(interaction(meeting, helper_socket),
                         "Contact must target the placement owner without moving its child anchor",
                         std::invalid_argument);
    CHECK_NOTHROW(interaction(replaced(meeting, R"("chain":"limb")", R"("chain":"other")"), helper_socket));
    CHECK_NOTHROW(interaction(meeting, helper_marker));
}

TEST_CASE("Actions check the roles they require, and the caller chooses the handling profile") {
    constexpr auto unmet_role = "Missing or incompatible required action role: tool";
    const MotionFixture fixture;
    const auto motion = std::make_shared<const MotionRuntime>(fixture.runtime());
    const ActionRuntime actions(motion, action_catalog);
    // Roles the action does not require are ignored.
    CHECK_NOTHROW(actions.validate_roles("reach", {{"spare", "free"}, {"tool", "grip"}}));
    CHECK_THROWS_WITH_AS(actions.validate_roles("reach", {}), unmet_role, std::invalid_argument);
    CHECK_THROWS_WITH_AS(actions.validate_roles("reach", {{"tool", "free"}}), unmet_role, std::invalid_argument);
    CHECK_THROWS_WITH_AS(actions.validate_roles("absent", {}), "Unknown action: absent", std::out_of_range);

    const AttachmentLibrary library(decode_attachment_catalog(attachment_catalog(), fixture.directory.path));
    const auto held = AttachmentSet::prepare(library, fixture.sockets, {{"tool", "prop"}});
    CHECK_NOTHROW(validate_attachment_action(actions, library, held, "reach"));
    CHECK_THROWS_WITH_AS(validate_attachment_action(actions, library, AttachmentSet{}, "reach"), unmet_role,
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(validate_attachment_action(actions, library, held, "twirl"),
                         "Action requires an unavailable attachment role/track", std::invalid_argument);
    // Either accepted profile performs the action while the tool is held; the runtime does not pick one.
    const auto base = motion->sample("base", 0);
    for (const auto *handling : {"free", "grip"}) {
        CAPTURE(handling);
        CHECK(actions.sample(base, {"reach", .25, {}, {}}, handling).clock.phase == 0);
    }
    CHECK_THROWS_WITH_AS(actions.sample(base, {"reach", .25, {}, {}}, "other"),
                         "Action is incompatible with this handling profile", std::invalid_argument);
}

TEST_CASE("A requested duration rescales a timed action, and the sample's clock stays in declared seconds") {
    constexpr double time_tolerance = 1e-12; // Timeline arithmetic is exact up to rounding.
    constexpr auto invalid_duration = "Invalid fixed action duration";
    const MotionFixture fixture;
    const auto motion = std::make_shared<const MotionRuntime>(fixture.runtime());
    // twirl's single phase becomes held, so it keeps its declared timing.
    const ActionRuntime actions(motion, replaced(action_catalog, R"("id":"spin","duration":0.5,"held":false)",
                                                 R"("id":"spin","duration":0.5,"held":true)"));
    CHECK(actions.scale({.action = "reach"}) == 1);
    CHECK(actions.scale({.action = "twirl"}) == 1);
    // reach declares 0.5 seconds, so lasting 1 second plays it at half rate.
    CHECK(actions.scale({.action = "reach", .duration = 1.}) == Near{.5, time_tolerance});
    const auto base = motion->sample("base", 0);
    const auto clock = actions.sample(base, {.action = "reach", .elapsed = .5, .duration = 1.}, "free").clock;
    CHECK(clock.elapsed == Near{.25, time_tolerance});
    CHECK(clock.progress == Near{.5, time_tolerance});

    CHECK_THROWS_WITH_AS(actions.scale({.action = "x"}), "Unknown action: x", std::out_of_range);
    CHECK_THROWS_WITH_AS(actions.scale({.action = "x", .duration = 0.}), "Unknown action: x", std::out_of_range);
    for (const double duration : {0., -1., std::numeric_limits<double>::infinity()}) {
        CAPTURE(duration);
        CHECK_THROWS_WITH_AS(actions.scale({.action = "reach", .duration = duration}), invalid_duration,
                             std::invalid_argument);
    }
    CHECK_THROWS_WITH_AS(actions.scale({.action = "twirl", .duration = 1.}), invalid_duration, std::invalid_argument);
}

TEST_CASE("An action layer that plays a layer clip uses that clip's mask") {
    const MotionFixture fixture;
    const auto motion = std::make_shared<const MotionRuntime>(fixture.runtime());
    constexpr auto wrong_mask = "Action layer must use its layer clip's mask";
    CHECK_THROWS_WITH_AS(ActionRuntime(motion, replaced(action_catalog, R"("mask":"limb")", R"("mask":"side")")),
                         wrong_mask, std::invalid_argument);
    // A null mask makes the layer full-body, which a layer clip is not.
    CHECK_THROWS_WITH_AS(ActionRuntime(motion, replaced(action_catalog, R"("mask":"limb")", R"("mask":null)")),
                         wrong_mask, std::invalid_argument);
}

TEST_CASE("An action catalog requires every field, with null, [] and {} declaring none") {
    const MotionFixture fixture;
    const auto motion = std::make_shared<const MotionRuntime>(fixture.runtime());
    // Each name is first a field of reach, its phase, its layer or its prop track.
    for (const auto *name :
         {"roles", "held", "cues", "props", "contacts", "mask", "mode", "weight", "reference", "required"}) {
        CAPTURE(name);
        const auto missing = "Missing JSON field: " + std::string(name);
        CHECK_THROWS_WITH_AS(ActionRuntime(motion, without(action_catalog, name)), missing.c_str(),
                             std::invalid_argument);
    }
    const ActionRuntime actions(motion, action_catalog);
    CHECK(actions.definition("twirl").required_roles.empty());
    const auto &phase = actions.definition("reach").phases.at(0);
    CHECK(phase.contacts.empty());
    CHECK(actions.definition("reach").timeline.phases().at(0).cues.empty());
    REQUIRE(phase.layers.size() == 1);
    CHECK(phase.layers[0].interval.begin == 0);
    CHECK(phase.layers[0].interval.end == 1);
    CHECK(phase.layers[0].mode == LayerMode::override_pose);
    CHECK(phase.layers[0].reference.empty());
    REQUIRE(phase.props.size() == 1);
    CHECK_FALSE(phase.props[0].required);
    // An additive layer names its reference pose, and an override's is null.
    const auto additive = replaced(replaced(action_catalog, R"("mode":"override")", R"("mode":"additive")"),
                                   R"("reference":null)", R"("reference":{"clip":"base","at":0.5})");
    const ActionRuntime additive_actions(motion, additive);
    const auto &layer = additive_actions.definition("reach").phases.at(0).layers.at(0);
    CHECK(layer.mode == LayerMode::additive);
    CHECK(layer.reference == "base");
    CHECK(layer.reference_at == .5);
    CHECK_THROWS_WITH_AS(ActionRuntime(motion, replaced(action_catalog, R"("reference":null)",
                                                        R"("reference":{"clip":"base","at":0.5})")),
                         "Additive action layer needs exactly one reference pose", std::invalid_argument);
    // null is the one spelling of a full-body layer's mask.
    CHECK_THROWS_WITH_AS(ActionRuntime(motion, replaced(action_catalog, R"("mask":"limb")", R"("mask":"")")),
                         "Empty presentation identity/reference", std::invalid_argument);
    // A phase holds at most ActionRuntime::maximum_phase_layers layers.
    std::string extra_layers;
    for (std::size_t i = 0; i < ActionRuntime::maximum_phase_layers; ++i)
        extra_layers += R"({"clip":"layer.limb","mask":"limb","interval":[0,1],"mode":"override",)"
                        R"("weight":[[0,1],[1,1]],"reference":null},)";
    CHECK_THROWS_WITH_AS(
        ActionRuntime(motion, replaced(action_catalog, R"("layers":[)", "\"layers\":[" + extra_layers)),
        "Action needs 1..8 pose layers per phase", std::invalid_argument);
}

TEST_CASE("Handling profiles choose a layer clip per base clip, and ownership checks the chosen clips") {
    const MotionFixture fixture;
    const auto runtime = fixture.runtime();
    const AttachmentLibrary library(decode_attachment_catalog(attachment_catalog(), fixture.directory.path));
    const auto &grip = library.catalog().handling.at("grip");
    CHECK(grip.layer_for("base") == "layer.side");
    CHECK(grip.layer_for("other") == "layer.limb");
    CHECK(library.handling("").layer_for("base").empty()); // The empty handling layers nothing.
    // Over the base clip the prop layers the side joint and the brace the limb, so they can be held together.
    const auto disjoint = AttachmentSet::prepare(library, fixture.sockets, {{"first", "prop"}, {"second", "brace"}});
    CHECK_NOTHROW(
        validate_attachment_ownership(runtime, library, disjoint, fixture.sockets, PrimarySocketSharing::shared));
    // Both hold their props on one socket, which only shared primary sockets allow.
    CHECK_THROWS_WITH_AS(validate_attachment_ownership(runtime, library, disjoint, fixture.sockets),
                         "Attachment roles share an exclusive primary socket", std::invalid_argument);
    const auto doubled = AttachmentSet::prepare(library, fixture.sockets, {{"first", "prop"}, {"second", "prop"}});
    CHECK_THROWS_WITH_AS(
        validate_attachment_ownership(runtime, library, doubled, fixture.sockets, PrimarySocketSharing::shared),
        "Motion layers have overlapping joint ownership", std::invalid_argument);
}

TEST_CASE("An attachment catalog accepts only version 4 and none of the removed fields") {
    const MotionFixture fixture;
    const auto catalog = attachment_catalog();
    const auto decode = [&](const std::string &text) { (void)decode_attachment_catalog(text, fixture.directory.path); };
    // Another version is reported before the fields, so a removed field does not hide it.
    CHECK_THROWS_WITH_AS(decode(replaced(catalog, R"("version":4)", R"("version":3,"defaults":{})")),
                         "Unsupported attachment catalog version", std::invalid_argument);
    // Version 3 named its version schema_version.
    CHECK_THROWS_WITH_AS(decode(replaced(catalog, R"("version":4)", R"("schema_version":3)")),
                         "Missing JSON field: version", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(replaced(catalog, R"("units":"meters")", R"("units":"feet")")),
                         "Unsupported attachment catalog units", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(replaced(catalog, R"("empty_handling")", R"("defaults":{},"empty_handling")")),
                         "Unknown JSON field: defaults", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(replaced(catalog, R"({"id":"prop",)", R"({"id":"prop","category":"test",)")),
                         "Unknown JSON field: category", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(replaced(catalog, R"("layer":"")", R"("carry":"")")), "Missing JSON field: layer",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(replaced(catalog, R"({"base":"layer.side"})", R"({"base":""})")),
                         "Empty presentation identity/reference", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(replaced(catalog, R"({"base":"layer.side"})", R"({"":"layer.side"})")),
                         "Empty layer override base clip", std::invalid_argument);
}

TEST_CASE("An attachment catalog requires every field, with null, [] and {} declaring none") {
    const MotionFixture fixture;
    const auto decode = [&](const std::string &text) {
        return decode_attachment_catalog(text, fixture.directory.path);
    };
    const auto catalog = braced_catalog();
    // Each name is first a field of the free handling, the brace's contact or the prop's visual.
    for (const auto *name :
         {"layer_overrides", "support_contacts", "actions", "primary_node", "marker_nodes", "animation_tracks"}) {
        CAPTURE(name);
        const auto missing = "Missing JSON field: " + std::string(name);
        CHECK_THROWS_WITH_AS(decode(without(catalog, name)), missing.c_str(), std::invalid_argument);
    }
    const auto decoded = decode(catalog);
    const auto &free = decoded.handling.at("free");
    CHECK(free.layer_overrides.empty());
    CHECK(free.support_contacts.empty());
    REQUIRE(decoded.handling.at("brace").support_contacts.size() == 1);
    CHECK(decoded.handling.at("brace").support_contacts[0].actions.empty());
    const auto &prop = decoded.visuals.at("prop");
    CHECK(prop.primary_node.empty());
    CHECK(prop.marker_nodes.empty());
    CHECK(prop.animation_tracks.empty());
    // A named primary node decodes as named, and null is the one spelling of none.
    CHECK(decode(replaced(catalog, R"("primary_node":null)", R"("primary_node":"prop")"))
              .visuals.at("prop")
              .primary_node == "prop");
    CHECK_THROWS_WITH_AS(decode(replaced(catalog, R"("primary_node":null)", R"("primary_node":"")")),
                         "Empty presentation identity/reference", std::invalid_argument);
    constexpr auto contact_count = "Handling contacts require an array of at most 4 constraints";
    CHECK_THROWS_WITH_AS(decode(replaced(catalog, R"("support_contacts":[])", R"("support_contacts":{})")),
                         contact_count, std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        decode(replaced(catalog, R"("support_contacts":[])", R"("support_contacts":[{},{},{},{},{}])")), contact_count,
        std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(replaced(catalog, R"("actions":[])", R"("actions":null)")),
                         "Contact action coverage must be an array", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(replaced(catalog, R"("marker_nodes":{})", R"("marker_nodes":null)")),
                         "Prop bindings must be named references", std::invalid_argument);
}

TEST_CASE("A handling profile's layer overrides are an object keyed by base clip") {
    const MotionFixture fixture;
    CHECK_THROWS_WITH_AS(
        decode_attachment_catalog(replaced(attachment_catalog(), R"({"base":"layer.side"})", R"(["layer.side"])"),
                                  fixture.directory.path),
        "Layer overrides must map base clips to layer clips", std::invalid_argument);
}

TEST_CASE("Document file references stay inside their directory on every platform") {
    const MotionFixture fixture;
    const auto profile = fixture.directory.path / "actor.profile.json";
    // JSON strings for a root, a Windows drive and a Windows parent directory. Windows reads the colon and the
    // backslash as a drive and a separator, so every platform rejects them.
    for (const std::string reference : {"/x.glb", "C:x.glb", R"(..\\x.glb)"}) {
        CAPTURE(reference);
        const auto quoted = "\"" + reference + "\"";
        CHECK_THROWS_WITH_AS(
            decode_attachment_catalog(replaced(attachment_catalog(), "\"prop.glb\"", quoted), fixture.directory.path),
            "Attachment model must be a relative GLB inside the catalog directory", std::invalid_argument);
        CHECK_THROWS_WITH_AS(
            MotionRuntime(fixture.body, fixture.manifest, replaced(fixture.contract, "\"motion.glb\"", quoted)),
            "Motion resource must be a relative GLB inside its contract directory", std::invalid_argument);
        std::ofstream(profile) << R"({"version":2,"id":"actor","manifest":)" + quoted + R"(,"sockets":{}})";
        CHECK_THROWS_WITH_AS(ActorPresentation{profile}, "Actor manifest must be inside its profile directory",
                             std::invalid_argument);
    }
}

TEST_CASE("A catalog and a manifest name their models in UTF-8 on every platform") {
    const MotionFixture fixture;
    // "mod\u00e8le.glb" in UTF-8, which Windows would read in its code page as a narrow string, naming another file.
    const std::filesystem::path model(u8"mod\u00e8le.glb");
    write_prop(fixture.directory.path / model, false);
    const AttachmentLibrary library(decode_attachment_catalog(
        replaced(attachment_catalog(), "\"prop.glb\"", R"("mod\u00e8le.glb")"), fixture.directory.path));
    CHECK(library.visual("prop").model == model);
    CHECK(library.load("prop")->source->nodes.at(0).name == "prop");
    const auto manifest_path = fixture.directory.path / "prop.manifest.json";
    std::ofstream(manifest_path)
        << R"({"version":4,"units":"meters","asset_id":"prop","model":"mod\u00e8le.glb","motion_contract":null,)"
           R"("skeleton":{"id":"test.rig","bind_signature":")" +
               rig_signature + R"(","joint_count":1},"clips":[]})";
    const auto manifest = read_manifest(manifest_path);
    CHECK(manifest.model == model);
    CHECK(load_asset(manifest.directory / manifest.model)->nodes.at(0).name == "prop");
}

TEST_CASE("Presentation frames and poles hold exactly their count of numbers, each within the float range") {
    constexpr std::size_t end = 3;
    const MotionFixture fixture;
    const auto decode = [&](const std::string &text) { (void)decode_attachment_catalog(text, fixture.directory.path); };
    const auto catalog = braced_catalog();
    CHECK_NOTHROW(decode(catalog));
    // nlohmann's own conversion reads true as 1, so this grip would be the identity.
    CHECK_THROWS_WITH_AS(decode(replaced(catalog, R"("primary_grip":[1,)", R"("primary_grip":[true,)")),
                         "JSON value must be a number", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(replaced(catalog, R"("primary_grip":[1,)", R"("primary_grip":[1e39,)")),
                         "JSON number outside the float range", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(replaced(catalog, R"("primary_grip":[1,)", R"("primary_grip":[1,1,)")),
                         "Presentation transform requires 16 column-major values", std::invalid_argument);
    // A frame whose bottom row differs from (0, 0, 0, 1) in any element is not affine.
    for (std::size_t element = 3; element < 16; element += 4) {
        CAPTURE(element);
        std::array<int, 16> frame{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        ++frame[element];
        std::string text = R"("primary_grip":[)";
        for (std::size_t i = 0; i < frame.size(); ++i)
            text += (i ? "," : "") + std::to_string(frame[i]);
        CHECK_THROWS_WITH_AS(decode(replaced(catalog, std::string(R"("primary_grip":)") + identity_frame, text + "]")),
                             "Presentation transform must be affine", std::invalid_argument);
    }
    // nlohmann's own conversion ignores numbers past the third.
    CHECK_THROWS_WITH_AS(decode(replaced(catalog, R"("pole":[0,0,2])", R"("pole":[0,0,2,0])")),
                         "Support contact pole requires three numbers", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(replaced(catalog, R"("pole":[0,0,2])", R"("pole":[0,0,true])")),
                         "JSON value must be a number", std::invalid_argument);

    const auto motion = std::make_shared<const MotionRuntime>(fixture.runtime());
    // The limb's chain starts below the root, so solving it leaves the child's anchor in place.
    const InteractionRuntime::Actors actors{
        {"child", {motion, {{"anchor", {0, identity()}}}}},
        {"parent", {motion, {{"anchor", {0, identity()}}, {"target", {end, identity()}}}}}};
    const auto interaction = [&](const std::string &document) { (void)InteractionRuntime(actors, document); };
    CHECK_NOTHROW(interaction(meeting));
    CHECK_THROWS_WITH_AS(interaction(replaced(meeting, R"("pole":[0,0,2])", R"("pole":[0,0,true])")),
                         "JSON value must be a number", std::invalid_argument);
    CHECK_THROWS_WITH_AS(interaction(replaced(meeting, R"("pole":[0,0,2])", R"("pole":[0,0,2,0])")),
                         "Interaction pole requires three coordinates", std::invalid_argument);
}

TEST_CASE("Interaction roles take their model from their motion, and every socket named is checked at construction") {
    constexpr std::size_t end = 3;
    const MotionFixture fixture;
    const auto motion = std::make_shared<const MotionRuntime>(fixture.runtime());
    CHECK(motion->model() == fixture.body);
    const auto actors = [&](std::shared_ptr<const MotionRuntime> child, InteractionSocket target) {
        return InteractionRuntime::Actors{{"child", {std::move(child), {{"anchor", {0, identity()}}}}},
                                          {"parent", {motion, {{"anchor", {0, identity()}}, {"target", target}}}}};
    };
    const InteractionRuntime interaction(actors(motion, {end, identity()}), meeting);
    for (const auto &role : interaction.bindings().roles())
        CHECK(role.asset == fixture.body);
    CHECK_THROWS_WITH_AS(InteractionRuntime(actors(nullptr, {end, identity()}), meeting),
                         "Interaction role has no motion", std::invalid_argument);
    // The target socket is only a contact target, never an attachment socket, so the bindings do not check it.
    CHECK_THROWS_WITH_AS(InteractionRuntime(actors(motion, {fixture.body->nodes.size(), identity()}), meeting),
                         "Unknown interaction socket node", std::invalid_argument);
    auto collapsed = identity();
    collapsed[0] = 0;
    CHECK_THROWS_WITH_AS(InteractionRuntime(actors(motion, {end, collapsed}), meeting), "Affine transform is collapsed",
                         std::invalid_argument);
}

TEST_CASE("An attachment socket document maps names to rest frames and to sockets") {
    const MotionFixture fixture;
    const auto document = [](const std::string &rest_joints, const std::string &sockets) {
        return R"({"version":1,"skeleton":"test.rig","bind_signature":")" + rig_signature + R"(","rest_joints":)" +
               rest_joints + R"(,"sockets":)" + sockets + "}";
    };
    const auto decode = [&](const std::string &text) {
        return decode_attachment_sockets(text, fixture.manifest, *fixture.body);
    };
    // The root rests at the origin, and the grip socket sits on it.
    const auto rest_joints = std::string(R"({"root":)") + identity_frame + "}";
    const auto grip = std::string(R"({"node":"root","local":)") + identity_frame + "}";
    const auto valid = document(rest_joints, R"({"grip":)" + grip + "}");
    const auto sockets = decode(valid);
    REQUIRE(sockets.size() == 1);
    CHECK(sockets.at("grip").node == 0);
    // The version is checked first, so another version reports it rather than a field it lacks or adds.
    CHECK_THROWS_WITH_AS(decode(replaced(valid, R"("version":1)", R"("version":2,"removed":0)")),
                         "Unsupported attachment socket document version", std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(replaced(valid, R"("version":1,)", "")), "Missing JSON field: version",
                         std::invalid_argument);
    // Read as a map, a list would name its entries by their indices.
    CHECK_THROWS_WITH_AS(decode(document(rest_joints, "[" + grip + "]")), "JSON field must be an object: sockets",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(decode(document(std::string("[") + identity_frame + "]", R"({"grip":)" + grip + "}")),
                         "JSON field must be an object: rest_joints", std::invalid_argument);
}

TEST_CASE("A motion contract's maps are objects and its lists are arrays") {
    const MotionFixture fixture;
    const auto contract = [&](const std::string &text) { (void)MotionRuntime(fixture.body, fixture.manifest, text); };
    // Read as a map, a list would name its entries by their indices; read as a list, a map would yield its values, and
    // either would read null as empty.
    CHECK_THROWS_WITH_AS(contract(replaced(fixture.contract, R"("masks":{"limb":["upper"],"side":["side"]})",
                                           R"("masks":[["upper"],["side"]])")),
                         "JSON field must be an object: masks", std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        contract(replaced(
            replaced(fixture.contract, R"("chains":{"limb":{)", R"("chains":[{)"),
            R"("other":{"joints":["other.upper","other.middle","other.end"],"minimum_angle":0,"maximum_angle":3.1}})",
            R"({"joints":["other.upper","other.middle","other.end"],"minimum_angle":0,"maximum_angle":3.1}])")),
        "JSON field must be an object: chains", std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        contract(replaced(fixture.contract,
                          R"("clips":[{"name":"base","loop":true,"reference_speed":null,"events":[]}])",
                          R"("clips":{"base":{"name":"base","loop":true,"reference_speed":null,"events":[]}})")),
        "JSON field must be an array: clips", std::invalid_argument);
    CHECK_THROWS_WITH_AS(contract(replaced(fixture.contract, R"("events":[])", R"("events":null)")),
                         "JSON field must be an array: events", std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        contract(replaced(replaced(fixture.contract, R"("layers":{"layer.limb":{)", R"("layers":[{)"),
                          R"("layer.side":{"mask":"side","owned_joints":["side","tip"],"context_joints":["root"]}})",
                          R"({"mask":"side","owned_joints":["side","tip"],"context_joints":["root"]}])")),
        "JSON field must be an object: layers", std::invalid_argument);
}

TEST_CASE("A motion contract requires a reference speed for each base clip, null for none") {
    const MotionFixture fixture;
    CHECK_FALSE(fixture.runtime().metadata("base").reference_speed);
    const MotionRuntime travel(fixture.body, fixture.manifest,
                               replaced(fixture.contract, R"("reference_speed":null)", R"("reference_speed":2)"));
    CHECK(travel.metadata("base").reference_speed == 2);
    CHECK_THROWS_WITH_AS(MotionRuntime(fixture.body, fixture.manifest, without(fixture.contract, "reference_speed")),
                         "Missing JSON field: reference_speed", std::invalid_argument);
}

TEST_CASE("Motion, action, actor and interaction documents report another version before their fields") {
    const MotionFixture fixture;
    // Each document also has a field that its version lacks, which must not hide the version.
    const auto contract = [&](const std::string &from, const std::string &to) {
        (void)MotionRuntime(fixture.body, fixture.manifest, replaced(fixture.contract, from, to));
    };
    CHECK_THROWS_WITH_AS(contract(R"({"version":4,)", R"({"version":3,"removed":0,)"),
                         "Unsupported motion contract version", std::invalid_argument);
    CHECK_THROWS_WITH_AS(contract(R"("evaluation":{"version":1,)", R"("evaluation":{"version":2,"removed":0,)"),
                         "Unsupported motion evaluation version", std::invalid_argument);
    const auto motion = std::make_shared<const MotionRuntime>(fixture.runtime());
    CHECK_THROWS_WITH_AS(ActionRuntime(motion, R"({"version":1,"removed":0,"actions":[]})"),
                         "Unsupported action catalog version", std::invalid_argument);
    // Version 1 named its version schema_version.
    CHECK_THROWS_WITH_AS(ActionRuntime(motion, R"({"schema_version":1,"actions":[]})"), "Missing JSON field: version",
                         std::invalid_argument);
    const auto profile = fixture.directory.path / "actor.profile.json";
    std::ofstream(profile) << R"({"version":1,"capabilities":[]})";
    CHECK_THROWS_WITH_AS(ActorPresentation{profile}, "Unsupported actor presentation profile version",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(InteractionRuntime({}, R"({"version":1,"removed":0})"),
                         "Unsupported coordinated interaction version", std::invalid_argument);
}

TEST_CASE("Coordinated interaction phases and cues must be arrays") {
    // Phases are read before the roles' actors are checked, so an actor without a motion reaches them.
    const InteractionRuntime::Actors actors{{"lead", {}}};
    const auto document = [](const std::string &phases) {
        return R"({"version":2,"id":"meet","phases":)" + phases +
               R"(,"roles":{"lead":{}},"attachments":[],"contacts":[]})";
    };
    CHECK_THROWS_WITH_AS(InteractionRuntime(actors, document(R"({"approach":{"duration":1}})")),
                         "Coordinated interaction phases must be an array", std::invalid_argument);
    CHECK_THROWS_WITH_AS(InteractionRuntime(actors, document(R"([{"id":"approach","duration":1,"held":false,
                                                                  "cues":{"id":"touch","at":0.5}}])")),
                         "Coordinated interaction cues must be an array", std::invalid_argument);
}

TEST_CASE("A coordinated interaction requires every field") {
    constexpr std::size_t end = 3;
    const MotionFixture fixture;
    const auto motion = std::make_shared<const MotionRuntime>(fixture.runtime());
    const InteractionRuntime::Actors actors{
        {"child", {motion, {{"anchor", {0, identity()}}}}},
        {"parent", {motion, {{"anchor", {0, identity()}}, {"target", {end, identity()}}}}}};
    CHECK_NOTHROW(InteractionRuntime(actors, meeting));
    // Each name is first a field of the phase, the child's layer or the contact; role layers are action layers.
    for (const auto *name : {"held", "cues", "mask", "orientation"}) {
        CAPTURE(name);
        const auto missing = "Missing JSON field: " + std::string(name);
        CHECK_THROWS_WITH_AS(InteractionRuntime(actors, without(meeting, name)), missing.c_str(),
                             std::invalid_argument);
    }
}

TEST_CASE("An attachment instance replaces its item in a scene") {
    const MotionFixture fixture;
    const AttachmentLibrary library(decode_attachment_catalog(attachment_catalog(), fixture.directory.path));
    Scene scene;
    AttachmentInstance held;
    CHECK(held.replace(scene, library, fixture.sockets, "prop"));
    REQUIRE(held.instance);
    const auto first = *held.instance;
    CHECK(scene.contains(first));
    CHECK_FALSE(held.replace(scene, library, fixture.sockets, "prop")); // Already attached.
    CHECK(held.replace(scene, library, fixture.sockets, "brace"));
    CHECK(held.item_id == "brace");
    CHECK_FALSE(scene.contains(first));
    CHECK_THROWS_WITH_AS(held.replace(scene, library, fixture.sockets, "absent"),
                         "Missing presentation reference: absent", std::out_of_range);
    CHECK(held.item_id == "brace"); // A failed replacement keeps the attached item.
    CHECK(held.replace(scene, library, fixture.sockets, ""));
    CHECK_FALSE(held.instance);
    CHECK(scene.instances().empty());
}

TEST_CASE("Attachment tracks and markers report invalid requests") {
    const MotionFixture fixture;
    const AttachmentLibrary library(decode_attachment_catalog(attachment_catalog(), fixture.directory.path));
    CHECK_THROWS_WITH_AS(library.load("spinner"), "Animated attachment model needs declared tracks",
                         std::invalid_argument);
    const auto prop = library.load("prop");
    const auto &visual = library.visual("prop");
    CHECK_THROWS_WITH_AS(sample_attachment_pose(*prop, visual, "spin", 2), "Invalid attachment track progress",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(sample_attachment_pose(*prop, visual, "spin", .5),
                         "Attachment visual lacks required track: spin", std::invalid_argument);
    CHECK(sample_attachment_pose(*prop, visual, "spin", .5, TrackRequirement::optional).world.size() ==
          1); // An optional track rests.
    AttachmentVisual following;
    following.markers.emplace("tip", identity());
    following.marker_nodes.emplace("tip", "prop");
    CHECK_THROWS_WITH_AS(attachment_marker(following, "tip"),
                         "Animated attachment marker requires the sampled prop pose", std::invalid_argument);
    const auto rest = sample_attachment_pose(*prop, visual);
    CHECK(attachment_marker(following, "tip", PropPose{*prop->source, rest}) == rest.world.at(0));
    CHECK_THROWS_WITH_AS(
        decode_attachment_catalog(replaced(attachment_catalog(), R"("handling":"grip")", R"("handling":"free")"),
                                  fixture.directory.path),
        "Attachment items require a handling profile with a socket", std::invalid_argument);
}

TEST_CASE("Support contacts solve together on a pose that hides a joint by scaling it to zero") {
    // Distance a solved contact may leave between its chain's end and its marker.
    constexpr float reach_tolerance = 2e-5F;
    constexpr std::size_t root = 0, end = 3, side = 4, tip = 5, other_end = 8;
    const MotionFixture fixture;
    const auto runtime = fixture.runtime();
    // Hiding the side joint collapses the tip below it, whose local transform only the source pose holds.
    auto local = runtime.sample("base", 0).local;
    local[side].scale = {0, 0, 0};
    const auto source = pose_from_local(*fixture.body, local);
    // An item held at the root braces both limbs, each at a marker of its own.
    const Vec3 limb_target{.3F, .5F, .2F}, other_target{-.7F, .5F, .2F};
    const auto marker = [](Vec3 position) {
        Transform frame;
        frame.translation = position;
        return matrix(frame);
    };
    AttachmentVisual visual;
    visual.markers = {{"limb.hold", marker(limb_target)}, {"other.hold", marker(other_target)}};
    AttachmentHandling handling{.id = "brace", .socket = "hold"};
    handling.support_contacts = {{"limb", "grip", "limb.hold", {0, 0, 2}, {"base"}, {"reach"}},
                                 {"other", "other.grip", "other.hold", {0, 0, 2}, {"base"}, {"reach"}}};
    const AttachmentSockets sockets{
        {"hold", {root, identity()}}, {"grip", {end, identity()}}, {"other.grip", {other_end, identity()}}};
    const auto primary = bind_attachment(sockets.at("hold"), visual);
    const auto solved = apply_attachment_contacts(runtime, source, "base", handling, visual, primary, sockets);
    REQUIRE(solved.contacts.size() == 2);
    CHECK(solved.contacts[0].chain == "limb");
    CHECK(solved.contacts[1].chain == "other");
    for (const auto &contact : solved.contacts) {
        CAPTURE(contact.chain);
        CHECK(contact.reachable);
        CHECK(contact.error < reach_tolerance);
    }
    CHECK(length(point(solved.pose.world[end], {}) - limb_target) < reach_tolerance);
    CHECK(length(point(solved.pose.world[other_end], {}) - other_target) < reach_tolerance);
    // The hidden joints stay where the source put them.
    for (const auto node : {side, tip})
        for (std::size_t i = 0; i < source.world[node].size(); ++i) {
            CAPTURE(node);
            CAPTURE(i);
            CHECK(solved.pose.world[node][i] == Near{source.world[node][i], pose_tolerance});
        }
    // Held at the hidden side joint, the item collapses with it, so a contact frame has no rotation to reach for.
    // Options name the action and weights by field; a zero weight skips its contact.
    const std::map<std::string, float, std::less<>> without_other{{"other", 0.F}};
    const auto weighted = apply_attachment_contacts(runtime, source, "idle", handling, visual, primary, sockets,
                                                    {.action = "reach", .weights = &without_other});
    REQUIRE(weighted.contacts.size() == 1);
    CHECK(weighted.contacts[0].chain == "limb");
    const std::map<std::string, float, std::less<>> excessive{{"limb", 2.F}};
    CHECK_THROWS_WITH_AS(
        apply_attachment_contacts(runtime, source, "base", handling, visual, primary, sockets, {.weights = &excessive}),
        "Invalid item contact weight", std::invalid_argument);
    const auto hidden = bind_attachment({side, identity()}, visual);
    CHECK_THROWS_WITH_AS(apply_attachment_contacts(runtime, source, "base", handling, visual, hidden, sockets),
                         "Affine transform is collapsed", std::invalid_argument);
}

TEST_CASE("Action layers evaluate together on a pose that hides a joint by scaling it to zero") {
    constexpr std::size_t root = 0, upper = 1;
    // The limb's layer clip turns the upper joint an eighth turn about +Z halfway through, and half of that is a
    // sixteenth.
    constexpr float eighth_turn_cosine = .70710678F, sixteenth_turn_cosine = .92387953F;
    const MotionFixture fixture;
    const auto hidden = hidden_side(fixture);
    // Each action's one phase lasts a second. "masked" turns the limb and then blends it halfway back to the base
    // clip's rest; "blended" blends the whole body halfway to the base clip's half-second pose and then turns the limb;
    // "resting" turns the limb at weight 0.
    const ActionRuntime actions(hidden.motion, R"({"version":2,"actions":[
        {"id":"masked","handling":["free"],"roles":{},"phases":[{"id":"reach","duration":1,"held":false,"layers":[
          {"clip":"layer.limb","mask":"limb","interval":[0,1],"mode":"override","weight":[[0,1],[1,1]],
           "reference":null},
          {"clip":"base","mask":"limb","interval":[0,1],"mode":"override","weight":[[0,0.5],[1,0.5]],
           "reference":null}],"cues":[],"props":[],"contacts":{}}]},
        {"id":"blended","handling":["free"],"roles":{},"phases":[{"id":"reach","duration":1,"held":false,"layers":[
          {"clip":"base","mask":null,"interval":[0,1],"mode":"override","weight":[[0,0.5],[1,0.5]],
           "reference":null},
          {"clip":"layer.limb","mask":"limb","interval":[0,1],"mode":"override","weight":[[0,1],[1,1]],
           "reference":null}],"cues":[],"props":[],"contacts":{}}]},
        {"id":"resting","handling":["free"],"roles":{},"phases":[{"id":"reach","duration":1,"held":false,"layers":[
          {"clip":"layer.limb","mask":"limb","interval":[0,1],"mode":"override","weight":[[0,0],[1,0]],
           "reference":null}],"cues":[],"props":[],"contacts":{}}]}]})");
    const auto base = hidden.motion->sample("base", 0);
    ActionSample masked, blended;
    CHECK_NOTHROW(masked = actions.sample(base, {"masked", .5, {}, {}}, "free"));
    CHECK(masked.pose.world[root][14] == Near{0, pose_tolerance});
    CHECK(masked.pose.world[upper][0] == Near{sixteenth_turn_cosine, pose_tolerance});
    check_hidden_side(masked.pose);
    CHECK_NOTHROW(blended = actions.sample(base, {"blended", .5, {}, {}}, "free"));
    CHECK(blended.pose.world[root][14] == Near{.25, pose_tolerance});
    CHECK(blended.pose.world[upper][0] == Near{eighth_turn_cosine, pose_tolerance});
    check_hidden_side(blended.pose);
    // A layer at weight 0 is skipped, so the base pose returns exactly, local transforms and all.
    const auto resting = actions.sample(base, {"resting", .5, {}, {}}, "free");
    CHECK(resting.pose.world == base.world);
    REQUIRE(resting.pose.local.size() == base.local.size());
    for (std::size_t i = 0; i < base.local.size(); ++i) {
        CAPTURE(i);
        CHECK(matrix(resting.pose.local[i]) == matrix(base.local[i]));
    }
}

TEST_CASE("An interaction role solves its layers and contacts together on a pose that hides a joint") {
    // Distance a solved contact may leave between its chain's end and its target.
    constexpr float reach_tolerance = 2e-5F;
    constexpr std::size_t root = 0, end = 3, other_end = 8;
    const MotionFixture fixture;
    const auto hidden = hidden_side(fixture);
    const auto parent_motion = std::make_shared<const MotionRuntime>(fixture.runtime());
    // The child stands on the parent's root and reaches both limbs to targets just off the parent's limb ends.
    Transform offset;
    offset.translation = {.1F, .1F, 0};
    const InteractionRuntime::Actors actors{{"child", {hidden.motion, {{"anchor", {root, identity()}}}}},
                                            {"parent",
                                             {parent_motion,
                                              {{"anchor", {root, identity()}},
                                               {"target", {end, matrix(offset)}},
                                               {"other.target", {other_end, matrix(offset)}}}}}};
    const auto both = replaced(meeting, R"("contacts":[)", R"("contacts":[
        {"child":"child","parent":"parent","chain":"other","target_socket":"other.target","pole":[0,0,2],
         "weights":{"hold":[[0,1],[1,1]]},"orientation":false},)");
    // The child's layers then end with a masked one, so they give a world-only pose. Only the child's base layer, the
    // last of its role, is followed by the end of its role and a comma.
    const auto layered = replaced(both, R"("weight":[[0,1],[1,1]],"reference":null}]}},)",
                                  R"("weight":[[0,1],[1,1]],"reference":null},
        {"clip":"layer.limb","mask":"limb","interval":[0,1],"mode":"override","weight":[[0,1],[1,1]],
         "reference":null}]}},)");
    const std::map<std::string, Mat4, std::less<>> free_worlds{{"child", identity()}, {"parent", identity()}};
    for (const auto &document : {both, layered}) {
        CAPTURE(document);
        const InteractionRuntime interaction(actors, document);
        InteractionSample sampled;
        CHECK_NOTHROW(sampled = interaction.sample(.5, {}, free_worlds));
        REQUIRE(sampled.contacts.size() == 2);
        CHECK(sampled.contacts[0].chain == "other");
        CHECK(sampled.contacts[1].chain == "limb");
        for (const auto &contact : sampled.contacts) {
            CAPTURE(contact.chain);
            CHECK(contact.role == "child");
            CHECK(contact.reachable);
            CHECK(contact.error < reach_tolerance);
        }
        check_hidden_side(sampled.frames[interaction.bindings().role("child")].pose);
    }
}

TEST_CASE("An actor presentation profile reports a value of the wrong JSON type as an invalid argument") {
    const MotionFixture fixture;
    const auto profile = fixture.directory.path / "actor.profile.json";
    std::ofstream(profile) << R"({"version":2,"id":5,"manifest":"actor.manifest.json","sockets":{}})";
    CHECK_THROWS_WITH_AS(ActorPresentation{profile},
                         "[json.exception.type_error.302] type must be string, but is number", std::invalid_argument);
}
