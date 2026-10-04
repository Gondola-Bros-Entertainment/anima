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
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
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

// The limb model's nodes, with a base clip that moves the root 1 unit along +Z over a second, and a layer
// clip for each mask that turns its root joint a quarter turn about +Z.
void write_motion(const std::filesystem::path &file) {
    std::vector<char> binary;
    for (const float number :
         {0.F, 1.F, 0.F, 0.F, 0.F, 0.F, 0.F, 1.F, 0.F, 0.F, 0.F, 1.F, 0.F, 0.F, .70710678F, .70710678F})
        append_f32(binary, number);
    write_glb(file, R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
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
          {"name":"layer.side","samplers":[{"input":0,"output":2}],"channels":[{"sampler":0,"target":{"node":4,"path":"rotation"}}]}]})",
              binary);
}

// A one-triangle model for attachment visuals; an @p animated one has a clip that moves it.
void write_prop(const std::filesystem::path &file, bool animated) {
    std::vector<char> binary;
    for (const float number : {0.F, 0.F, 0.F, 1.F, 0.F, 0.F, 0.F, 1.F, 0.F, 0.F, 1.F, 0.F, 0.F, 0.F, 0.F, 0.F, 1.F})
        append_f32(binary, number);
    write_glb(file,
              std::string(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
        "nodes":[{"name":"prop","mesh":0}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],
        "buffers":[{"byteLength":68}],
        "bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":8},
                       {"buffer":0,"byteOffset":44,"byteLength":24}],
        "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
                     {"bufferView":1,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},
                     {"bufferView":2,"componentType":5126,"count":2,"type":"VEC3"}])") +
                  (animated ? R"(,"animations":[{"name":"turn","samplers":[{"input":1,"output":2}],
                         "channels":[{"sampler":0,"target":{"node":0,"path":"translation"}}]}]})"
                            : "}"),
              binary);
}

// An attachment catalog of two items held at the grip socket. Both layer the limb, except that the prop's
// grip profile layers the side joint over the base clip instead. The spinner visual's model is animated but
// declares no tracks, so it cannot load.
std::string attachment_catalog() {
    return std::string(R"({"schema_version":3,"units":"meters","empty_handling":"free",
        "handling":[{"id":"free","socket":"","layer":""},
                    {"id":"grip","socket":"grip","layer":"layer.limb","layer_overrides":{"base":"layer.side"}},
                    {"id":"brace","socket":"grip","layer":"layer.limb"}],
        "visuals":[{"id":"prop","model":"prop.glb","primary_grip":)") +
           identity_frame + R"(,"markers":{}},{"id":"spinner","model":"spinner.glb","primary_grip":)" + identity_frame +
           R"(,"markers":{}}],
        "items":[{"id":"prop","visual":"prop","handling":"grip"},{"id":"brace","visual":"prop","handling":"brace"}]})";
}
// @p text with its first @p from replaced by @p to; the text must contain @p from.
std::string replaced(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to);
}

// Two actions on the limb. reach needs a tool role held with the grip profile and may be performed with
// either profile; twirl needs the tool's visual to have a spin track, which the prop lacks.
constexpr auto action_catalog = R"({"schema_version":1,"actions":[
    {"id":"reach","handling":["free","grip"],"roles":{"tool":["grip"]},"phases":[{"id":"extend","duration":0.5,
      "layers":[{"clip":"layer.limb","mask":"limb","interval":[0,1]}],
      "props":[{"role":"tool","track":"spin","interval":[0,1],"required":false}]}]},
    {"id":"twirl","handling":["grip"],"phases":[{"id":"spin","duration":0.5,
      "layers":[{"clip":"layer.limb","mask":"limb","interval":[0,1]}],
      "props":[{"role":"tool","track":"spin","interval":[0,1]}]}]}]})";

// A limb model bound to its motion: the motion GLB on disk, the manifest and the contract that names it, and
// a prop model with a grip socket at the end of the limb.
struct MotionFixture {
    TempDirectory directory;
    std::shared_ptr<const Asset> body = std::make_shared<const Asset>(limb_model());
    Manifest manifest;
    std::string contract = R"({"version":3,"skeleton":{"id":"test.rig","bind_signature":")" + rig_signature +
                           R"(","joint_count":)" + std::to_string(rig_joints) + R"(},"resource":"motion.glb",
        "evaluation":{"version":1,"id":"test.evaluation",
          "parents":{"root":null,"upper":"root","middle":"upper","end":"middle","side":"root","tip":"side",
                     "other.upper":"root","other.middle":"other.upper","other.end":"other.middle"},
          "masks":{"limb":["upper"],"side":["side"]},
          "chains":{"limb":{"joints":["upper","middle","end"],"minimum_angle":0,"maximum_angle":3.1},
                    "other":{"joints":["other.upper","other.middle","other.end"],"minimum_angle":0,"maximum_angle":3.1}}},
        "clips":[{"name":"base","loop":true,"events":[]}],
        "layers":{"layer.limb":{"mask":"limb","owned_joints":["end","middle","upper"],"context_joints":["root"]},
                  "layer.side":{"mask":"side","owned_joints":["side","tip"],"context_joints":["root"]}}})";
    std::map<std::string, AttachmentSocket, std::less<>> sockets{{"grip", {3, identity()}}};
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
    CHECK(library.resident_assets().size() == 1); // Every role shares the one loaded model.
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
        CHECK(actions.sample(base, {"reach", 1, .25, {}, {}}, handling).clock.phase == 0);
    }
    CHECK_THROWS_WITH_AS(actions.sample(base, {"reach", 1, .25, {}, {}}, "other"),
                         "Action is incompatible with this handling profile", std::invalid_argument);
}

TEST_CASE("An action layer that plays a layer clip uses that clip's mask") {
    const MotionFixture fixture;
    const auto motion = std::make_shared<const MotionRuntime>(fixture.runtime());
    constexpr auto wrong_mask = "Action layer must use its layer clip's mask";
    CHECK_THROWS_WITH_AS(ActionRuntime(motion, replaced(action_catalog, R"("mask":"limb")", R"("mask":"side")")),
                         wrong_mask, std::invalid_argument);
    // Without a mask the layer is full-body, which a layer clip is not.
    CHECK_THROWS_WITH_AS(ActionRuntime(motion, replaced(action_catalog, R"("mask":"limb",)", "")), wrong_mask,
                         std::invalid_argument);
}

TEST_CASE("Handling profiles choose a layer clip per base clip, and ownership checks the chosen clips") {
    const MotionFixture fixture;
    const auto runtime = fixture.runtime();
    const AttachmentLibrary library(decode_attachment_catalog(attachment_catalog(), fixture.directory.path));
    const auto &grip = library.catalog.motions.at("grip");
    CHECK(grip.layer_for("base") == "layer.side");
    CHECK(grip.layer_for("other") == "layer.limb");
    CHECK(library.motion("").layer_for("base").empty()); // The empty handling layers nothing.
    // Over the base clip the prop layers the side joint and the brace the limb, so they can be held together.
    const auto disjoint = AttachmentSet::prepare(library, fixture.sockets, {{"first", "prop"}, {"second", "brace"}});
    CHECK_NOTHROW(validate_attachment_ownership(runtime, library, disjoint, fixture.sockets, false));
    const auto doubled = AttachmentSet::prepare(library, fixture.sockets, {{"first", "prop"}, {"second", "prop"}});
    CHECK_THROWS_WITH_AS(validate_attachment_ownership(runtime, library, doubled, fixture.sockets, false),
                         "Motion layers have overlapping joint ownership", std::invalid_argument);
}

TEST_CASE("An attachment catalog accepts only version 3 and none of the removed fields") {
    const MotionFixture fixture;
    const auto catalog = attachment_catalog();
    const auto decode = [&](const std::string &text) { (void)decode_attachment_catalog(text, fixture.directory.path); };
    // Another version is reported before the fields, so a removed field does not hide it.
    CHECK_THROWS_WITH_AS(decode(replaced(catalog, R"("schema_version":3)", R"("schema_version":2,"defaults":{})")),
                         "Unsupported attachment catalog version", std::invalid_argument);
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

TEST_CASE("A handling profile's layer overrides are an object keyed by base clip") {
    const MotionFixture fixture;
    CHECK_THROWS_WITH_AS(
        decode_attachment_catalog(replaced(attachment_catalog(), R"({"base":"layer.side"})", R"(["layer.side"])"),
                                  fixture.directory.path),
        "Layer overrides must map base clips to layer clips", std::invalid_argument);
}

TEST_CASE("Motion, action, actor and interaction documents report another version before their fields") {
    const MotionFixture fixture;
    // Each document also has a field that its version lacks, which must not hide the version.
    const auto contract = [&](const std::string &from, const std::string &to) {
        (void)MotionRuntime(fixture.body, fixture.manifest, replaced(fixture.contract, from, to));
    };
    CHECK_THROWS_WITH_AS(contract(R"({"version":3,)", R"({"version":2,"removed":0,)"),
                         "Unsupported motion contract version", std::invalid_argument);
    CHECK_THROWS_WITH_AS(contract(R"("evaluation":{"version":1,)", R"("evaluation":{"version":2,"removed":0,)"),
                         "Unsupported motion evaluation version", std::invalid_argument);
    const auto motion = std::make_shared<const MotionRuntime>(fixture.runtime());
    CHECK_THROWS_WITH_AS(ActionRuntime(motion, R"({"schema_version":2,"removed":0,"actions":[]})"),
                         "Unsupported action catalog version", std::invalid_argument);
    const auto profile = fixture.directory.path / "actor.profile.json";
    std::ofstream(profile) << R"({"version":1,"capabilities":[]})";
    CHECK_THROWS_WITH_AS(ActorPresentation{profile}, "Unsupported actor presentation profile version",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(InteractionRuntime({}, R"({"version":2,"removed":0})"),
                         "Unsupported coordinated interaction version", std::invalid_argument);
}

TEST_CASE("Coordinated interaction phases and cues must be arrays") {
    // Phases are read before the roles' actors are checked, so an actor without a model reaches them.
    const InteractionRuntime::Actors actors{{"lead", {}}};
    const auto document = [](const std::string &phases) {
        return R"({"version":1,"id":"meet","phases":)" + phases +
               R"(,"roles":{"lead":{}},"attachments":[],"contacts":[]})";
    };
    CHECK_THROWS_WITH_AS(InteractionRuntime(actors, document(R"({"approach":{"duration":1}})")),
                         "Coordinated interaction phases must be an array", std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        InteractionRuntime(actors, document(R"([{"id":"approach","duration":1,"cues":{"id":"touch","at":0.5}}])")),
        "Coordinated interaction cues must be an array", std::invalid_argument);
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
    CHECK(sample_attachment_pose(*prop, visual, "spin", .5, false).world.size() == 1); // An optional track rests.
    AttachmentVisual following;
    following.markers.emplace("tip", identity());
    following.marker_nodes.emplace("tip", "prop");
    CHECK_THROWS_WITH_AS(attachment_marker(following, nullptr, nullptr, "tip"),
                         "Animated attachment marker requires the sampled prop pose", std::invalid_argument);
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
    handling.support_contacts = {{"limb", "grip", "limb.hold", {0, 0, 2}, {"base"}, {}},
                                 {"other", "other.grip", "other.hold", {0, 0, 2}, {"base"}, {}}};
    const std::map<std::string, AttachmentSocket, std::less<>> sockets{
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
    const auto hidden = bind_attachment({side, identity()}, visual);
    CHECK_THROWS_WITH_AS(apply_attachment_contacts(runtime, source, "base", handling, visual, hidden, sockets),
                         "Affine transform is collapsed", std::invalid_argument);
}
