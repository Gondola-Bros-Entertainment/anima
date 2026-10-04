#include "near.hpp"
#include <anima/assets/fitted.hpp>
#include <anima/assets/preview.hpp>
// This suite supplies its own main, which reads the optional manifest argument.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <locale>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using namespace anima;
namespace {
constexpr double tolerance = 1e-5; // Absolute error allowed in poses, times and quaternion norms.
constexpr auto preview_test = "An exported manifest previews every declared clip";
constexpr auto valid_manifest =
    R"({"schema_version":3,"units":"meters","asset_id":"two-joint-body","model":"body.glb",)"
    R"("skeleton":{"id":"test.rig","joint_count":2,)"
    R"("bind_signature":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},)"
    R"("clips":[{"name":"test","loop":false,"events":[{"time":0.53,"name":"pulse\uD83D\uDCA1"}]}]})";
constexpr auto speed_range = "Clip reference speed must be finite and positive";
constexpr auto invalid_blend = "Pose blend requires matching local poses and a weight in [0,1]";

// The manifest that the asset_preview test names on the command line.
std::optional<std::filesystem::path> exported_manifest;

float deviation(const Pose &a, const Pose &b) {
    float d = 0;
    for (std::size_t i = 0; i < a.world.size(); ++i)
        for (unsigned k = 0; k < 16; ++k)
            d = std::max(d, std::abs(a.world[i][k] - b.world.at(i)[k]));
    return d;
}
std::string changed(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to);
}
// A manifest file in the temporary directory, rewritten by each read and removed on destruction.
struct ManifestFile {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("anima-manifest-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    ManifestFile() = default;
    ManifestFile(const ManifestFile &) = delete;
    ManifestFile &operator=(const ManifestFile &) = delete;
    ~ManifestFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    Manifest read(std::string_view json) const {
        std::ofstream(path, std::ios::binary) << json;
        return read_manifest(path);
    }
};
// A root, a hand joint carrying a socket, and an unanimated mesh node whose triangle the hand skins. The clip
// moves the root and turns the hand half a turn about +Z.
Asset fixture() {
    Asset asset;
    asset.nodes.resize(4);
    asset.nodes[0].name = "root";
    asset.nodes[1].name = "hand";
    asset.nodes[1].parent = 0;
    asset.nodes[1].rest.translation = {0, 1, 0};
    asset.nodes[2].name = "mesh";
    asset.nodes[2].rest.translation = {99, 0, 0};
    asset.nodes[3].name = "socket";
    asset.nodes[3].parent = 1;
    asset.nodes[3].rest.translation = {0, .5F, 0};
    AssetSkin skin;
    skin.joints = {0, 1};
    skin.inverse_bind = {identity(), identity()};
    skin.inverse_bind[1][13] = -1;
    asset.skins.push_back(skin);
    SourcePrimitive primitive;
    primitive.node = 2;
    primitive.skin = 0;
    primitive.mesh_name = "triangle";
    for (auto p : {Vec3{0, 2, 0}, Vec3{1, 2, 0}, Vec3{0, 3, 0}}) {
        SourceVertex v;
        v.position = p;
        v.normal = {0, 0, 1};
        v.joints = {1, 0, 0, 0};
        v.weights = {1, 0, 0, 0};
        primitive.vertices.push_back(v);
    }
    asset.primitives.push_back(primitive);
    asset.mesh_nodes = 1;
    Animation clip;
    clip.name = "test";
    clip.duration = 1;
    clip.channels.push_back({0, ChannelPath::translation, Interpolation::linear, {0, 1}, {{0, 0, 0, 0}, {2, 0, 0, 0}}});
    clip.channels.push_back({1, ChannelPath::rotation, Interpolation::linear, {0, 1}, {{0, 0, 0, 1}, {0, 0, 1, 0}}});
    asset.animations.push_back(clip);
    return asset;
}
// A clip whose keys all sit at time 0, as load_asset imports it: a pose of zero duration with the root at +3 X.
Animation pose_clip() {
    Animation clip;
    clip.name = "brace";
    clip.channels.push_back({0, ChannelPath::translation, Interpolation::linear, {0}, {{3, 0, 0, 0}}});
    return clip;
}
} // namespace

TEST_CASE("A manifest keeps its body, clip policy, travel speed and events") {
    const auto asset = fixture();
    const ManifestFile file;
    const auto manifest = file.read(valid_manifest);
    REQUIRE_NOTHROW(validate_manifest(manifest, asset));
    CHECK(manifest.joint_count == 2);
    REQUIRE(manifest.clips.size() == 1);
    CHECK_FALSE(manifest.clips[0].loop);
    CHECK(manifest.clips[0].events.at(0).name == "pulse\xF0\x9F\x92\xA1"); // The escaped surrogate pair.
    CHECK_FALSE(manifest.clips[0].reference_speed); // A clip without travel metadata has no implicit speed.
    const auto travel = file.read(changed(valid_manifest, "\"loop\":false", "\"loop\":false,\"reference_speed\":3.2"));
    REQUIRE_NOTHROW(validate_manifest(travel, asset));
    REQUIRE(travel.clips.at(0).reference_speed);
    CHECK(*travel.clips[0].reference_speed == Near{3.2, tolerance});
}

TEST_CASE("Manifest numbers do not depend on the process locale") {
    struct Comma : std::numpunct<char> {
        char do_decimal_point() const override { return ','; }
    };
    struct RestoreLocale {
        std::locale previous = std::locale();
        ~RestoreLocale() { std::locale::global(previous); }
    } restore;
    std::locale::global(std::locale(std::locale::classic(), new Comma));
    const ManifestFile file;
    CHECK(file.read(valid_manifest).clips.at(0).events.at(0).time == Near{.53, tolerance});
}

TEST_CASE("Invalid manifests are rejected with their reason") {
    const auto asset = fixture();
    const ManifestFile file;
    for (const auto speed : {"0", "-1"}) {
        CAPTURE(speed);
        CHECK_THROWS_WITH_AS(file.read(changed(valid_manifest, "\"loop\":false",
                                               std::string("\"loop\":false,\"reference_speed\":") + speed)),
                             speed_range, std::invalid_argument);
    }
    auto infinite_speed =
        file.read(changed(valid_manifest, "\"loop\":false", "\"loop\":false,\"reference_speed\":3.2"));
    infinite_speed.clips.at(0).reference_speed = std::numeric_limits<double>::infinity();
    CHECK_THROWS_WITH_AS(validate_manifest(infinite_speed, asset), speed_range, std::invalid_argument);

    CHECK_THROWS_WITH_AS(file.read(changed(valid_manifest, "body.glb", "../body.glb")),
                         "Manifest model must be a filename beside the manifest: ../body.glb", std::invalid_argument);
    CHECK_THROWS_WITH_AS(file.read(changed(valid_manifest, "body.glb", "C:/body.glb")),
                         "Manifest model must be a filename beside the manifest: C:/body.glb", std::invalid_argument);
    // what() ends at the NUL that the escape decodes to.
    CHECK_THROWS_WITH_AS(file.read(changed(valid_manifest, "body.glb", R"(body\u0000.glb)")),
                         "Manifest model must be a filename beside the manifest: body", std::invalid_argument);
    CHECK_THROWS_WITH_AS(file.read(changed(valid_manifest, "body.glb", R"(folder\\body.glb)")),
                         "Manifest model must be a filename beside the manifest: folder\\body.glb",
                         std::invalid_argument);

    CHECK_THROWS_WITH_AS(
        file.read(changed(valid_manifest, "\"schema_version\":3", "\"schema_version\":3,\"schema_version\":3")),
        "Invalid manifest JSON: Duplicate JSON document field", std::invalid_argument);
    CHECK_THROWS_WITH_AS(file.read(changed(valid_manifest, "0.53", "1e999")),
                         "Invalid manifest JSON: [json.exception.out_of_range.406] number overflow parsing '1e999'",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(file.read(changed(valid_manifest, "0.53", "01")),
                         "Invalid manifest JSON: [json.exception.parse_error.101] parse error at line 1, column 270: "
                         "syntax error while parsing object - unexpected number literal; expected '}'",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(file.read(changed(valid_manifest, R"(\uD83D\uDCA1)", R"(\uD83D)")),
                         "Invalid manifest JSON: [json.exception.parse_error.101] parse error at line 1, column 293: "
                         "syntax error while parsing value - invalid string: surrogate U+D800..U+DBFF must be "
                         "followed by U+DC00..U+DFFF; last read: '\"pulse\\uD83D\"'",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(file.read(std::string(valid_manifest) + "false"),
                         "Invalid manifest JSON: [json.exception.parse_error.101] parse error at line 1, column 309: "
                         "syntax error while parsing value - unexpected false literal; expected end of input",
                         std::invalid_argument);
    // The JSON library quotes the ill-formed byte as it read it.
    CHECK_THROWS_WITH_AS(file.read(changed(valid_manifest, "two-joint-body", "invalid-\xC0\xAF")),
                         "Invalid manifest JSON: [json.exception.parse_error.101] parse error at line 1, column 58: "
                         "syntax error while parsing value - invalid string: ill-formed UTF-8 byte; last read: "
                         "'\"invalid-\xC0'",
                         std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        file.read(changed(valid_manifest, "\"clips\":[",
                          "\"nested\":" + std::string(34, '[') + "0" + std::string(34, ']') + ",\"clips\":[")),
        "Invalid manifest JSON: JSON document exceeds nesting limit", std::invalid_argument);

    // Earlier versions have no compatibility reader, and the version is checked first, so a version 2 manifest
    // reports its version rather than the fields that version had.
    constexpr auto unsupported = "Unsupported model manifest version";
    for (const auto *version :
         {"\"schema_version\":2,\"stage\":\"art\"", "\"schema_version\":3.0", "\"schema_version\":\"3\""}) {
        CAPTURE(version);
        CHECK_THROWS_WITH_AS(file.read(changed(valid_manifest, "\"schema_version\":3", version)), unsupported,
                             std::invalid_argument);
    }
    CHECK_THROWS_WITH_AS(file.read(changed(valid_manifest, "\"meters\"", "\"feet\"")), "Manifest units must be meters",
                         std::invalid_argument);
    // The event name without its escaped emoji, so the replacements below are plain ASCII.
    const auto plain = changed(valid_manifest, R"(\uD83D\uDCA1)", "");
    // Unknown fields are rejected at every level, including the event fields version 2 used.
    CHECK_THROWS_WITH_AS(file.read(changed(valid_manifest, "\"units\"", "\"stage\":\"art\",\"units\"")),
                         "Unknown JSON field: stage", std::invalid_argument);
    CHECK_THROWS_WITH_AS(file.read(changed(valid_manifest, "\"joint_count\"", "\"root\":\"hips\",\"joint_count\"")),
                         "Unknown JSON field: root", std::invalid_argument);
    CHECK_THROWS_WITH_AS(file.read(changed(valid_manifest, "\"loop\":false", "\"loop\":false,\"speed\":1")),
                         "Unknown JSON field: speed", std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        file.read(changed(valid_manifest, "{\"time\":0.53,\"name\"", "{\"time_seconds\":0.53,\"event\"")),
        "Missing JSON field: time", std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        file.read(changed(plain, R"("clips":[{"name":"test","loop":false,"events":[{"time":0.53,"name":"pulse"}]}])",
                          R"("clips":{})")),
        "Manifest clips must be an array", std::invalid_argument);
    CHECK_THROWS_WITH_AS(file.read(changed(plain, R"("events":[{"time":0.53,"name":"pulse"}])", R"("events":{})")),
                         "Manifest clip events must be an array", std::invalid_argument);
    for (const auto *event : {"{\"time\":-0.1,\"name\":\"pulse\"}", "{\"time\":0.53,\"name\":\"\"}"}) {
        CAPTURE(event);
        CHECK_THROWS_WITH_AS(file.read(changed(plain, R"({"time":0.53,"name":"pulse"})", event)),
                             "Invalid manifest clip event", std::invalid_argument);
    }
    CHECK_THROWS_WITH_AS(
        file.read(changed(valid_manifest, "\"clips\"", "\"motion_contract\":\"../motion.json\",\"clips\"")),
        "Manifest motion_contract must be a filename beside the manifest: ../motion.json", std::invalid_argument);
    CHECK_THROWS_WITH_AS(file.read(changed(valid_manifest, "\"joint_count\":2", "\"joint_count\":2.5")),
                         "Invalid manifest joint count", std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        validate_manifest(file.read(changed(valid_manifest, "\"joint_count\":2", "\"joint_count\":3")), asset),
        "Model skin does not match manifest joint count", std::invalid_argument);
    CHECK_THROWS_WITH_AS(validate_manifest(file.read(changed(valid_manifest, "0.53", "1.1")), asset),
                         "Manifest clip event outside its clip: test", std::invalid_argument);
    CHECK_THROWS_WITH_AS(
        validate_manifest(file.read(changed(valid_manifest, "\"name\":\"test\"", "\"name\":\"missing\"")), asset),
        "Manifest clip must name exactly one animation: missing", std::invalid_argument);
    // A clip name that two animations share names neither.
    auto doubled = asset;
    doubled.animations.push_back(doubled.animations[0]);
    auto listed = file.read(valid_manifest);
    listed.clips.push_back(listed.clips.at(0));
    listed.clips.back().name = "other";
    CHECK_THROWS_WITH_AS(validate_manifest(listed, doubled), "Manifest clip must name exactly one animation: test",
                         std::invalid_argument);
}

TEST_CASE("Lookups report a missing name as std::out_of_range and an ambiguous one as std::invalid_argument") {
    const auto asset = fixture();
    CHECK_THROWS_WITH_AS(find_animation(asset, "missing"), "Missing animation: missing", std::out_of_range);
    CHECK_THROWS_WITH_AS(unique_node(asset, "missing"), "Missing node: missing", std::out_of_range);
    auto doubled = asset;
    doubled.animations.push_back(doubled.animations[0]);
    doubled.nodes[2].name = "hand";
    CHECK_THROWS_WITH_AS(find_animation(doubled, "test"), "Ambiguous animation name: test", std::invalid_argument);
    CHECK_THROWS_WITH_AS(unique_node(doubled, "hand"), "Ambiguous node name: hand", std::invalid_argument);
}

TEST_CASE("A joint scaled to zero collapses the part it skins") {
    // A clip that scales a joint to zero hides the part it skins, as engines hide bones.
    const auto asset = fixture();
    auto hide = asset.animations[0];
    hide.channels.push_back({1, ChannelPath::scale, Interpolation::linear, {0, 1}, {{1, 1, 1, 0}, {0, 0, 0, 0}}});
    const auto hidden = make_mesh_snapshot(asset, sample_pose(asset, &hide, 1));
    REQUIRE_FALSE(hidden.vertices.empty());
    for (const auto &vertex : hidden.vertices)
        CHECK(length(vertex.position - hidden.vertices.front().position) == Near{0, tolerance});
}

TEST_CASE("A pose too short for a primitive's node is rejected before the snapshot indexes it") {
    const auto asset = fixture();
    auto short_pose = sample_pose(asset);
    short_pose.world.resize(1);
    CHECK_THROWS_WITH_AS(make_mesh_snapshot(asset, short_pose), "Pose does not match asset nodes", std::runtime_error);
}

TEST_CASE("Sampling moves the hierarchy and skin without changing the asset or other poses") {
    const auto asset = fixture();
    const auto rest = sample_pose(asset);
    const auto half = sample_pose(asset, &asset.animations[0], .5);
    CHECK(half.world[0][12] == Near{1, tolerance}); // The root's translation.
    // The socket follows the hand's rotation.
    CHECK(half.world[3][12] == Near{.5, tolerance});
    CHECK(half.world[3][13] == Near{1, tolerance});
    // The skin applies the joint's pose through its inverse bind.
    const auto scene = make_mesh_snapshot(asset, half);
    CHECK(scene.vertices.at(0).position.x == Near{0, tolerance});
    CHECK(scene.vertices.at(0).position.y == Near{1, tolerance});
    // The source vertices and rest transforms are unchanged.
    CHECK(asset.primitives[0].vertices[0].position.y == Near{2, tolerance});
    CHECK(asset.nodes[1].rest.translation.y == Near{1, tolerance});
    CHECK(deviation(rest, sample_pose(asset)) < tolerance); // Poses share no mutable state.
}

TEST_CASE("Blending local poses keeps the hierarchy, the shorter arc and matrix nodes") {
    const auto asset = fixture();
    const auto rest = sample_pose(asset);
    const auto half = sample_pose(asset, &asset.animations[0], .5);
    const auto end = sample_pose(asset, &asset.animations[0], 1);
    CHECK(deviation(blend_pose(asset, rest, end, .5F), half) < tolerance);
    CHECK(deviation(blend_pose(asset, rest, end, 0), rest) < tolerance);
    CHECK(deviation(blend_pose(asset, rest, end, 1), end) < tolerance);
    // A 180-degree endpoint has two equally short arcs. Use 90 degrees
    // here so this verifies sign invariance of the unique shortest arc.
    auto equivalent = half;
    for (auto &transform : equivalent.local)
        for (auto &value : transform.rotation)
            value = -value;
    CHECK(deviation(blend_pose(asset, rest, equivalent, .5F), blend_pose(asset, rest, half, .5F)) < tolerance);
    // A matrix node keeps its rest transform.
    auto matrix_asset = asset;
    matrix_asset.nodes[2].has_matrix = true;
    matrix_asset.nodes[2].rest_matrix = identity();
    matrix_asset.nodes[2].rest_matrix[12] = 7;
    CHECK(blend_pose(matrix_asset, rest, end, .5F).world[2][12] == Near{7, tolerance});
}

TEST_CASE("Blends with an invalid weight or an incomplete pose are rejected") {
    const auto asset = fixture();
    const auto rest = sample_pose(asset);
    const auto end = sample_pose(asset, &asset.animations[0], 1);
    for (const float weight : {-1.F, 2.F, std::numeric_limits<float>::quiet_NaN()}) {
        CAPTURE(weight);
        CHECK_THROWS_WITH_AS(blend_pose(asset, rest, end, weight), invalid_blend, std::invalid_argument);
    }
    auto incomplete = rest;
    incomplete.local.pop_back();
    CHECK_THROWS_WITH_AS(blend_pose(asset, incomplete, end, .5F), invalid_blend, std::invalid_argument);
}

TEST_CASE("Quaternion interpolation takes the shorter arc and normalizes its result") {
    const auto antipodal = slerp({0, 0, 0, 1}, {0, 0, 0, -1}, .5F);
    CHECK(std::abs(antipodal[3]) == Near{1, tolerance});
    const auto unit = slerp({0, 0, 0, 2}, {0, 0, .001F, 1}, .5F);
    double norm = 0;
    for (const double v : unit)
        norm += v * v;
    CHECK(norm == Near{1, tolerance});
}

TEST_CASE("Quaternions that cannot be normalized are rejected with their code") {
    // A MathError's message is its code's math_error_message, so matching the message matches the code.
    CHECK_THROWS_WITH_AS(unit_quaternion({0, 0, 0, 0}), math_error_message(MathErrorCode::zero_quaternion), MathError);
    CHECK_THROWS_WITH_AS(unit_quaternion({0, 0, 0, std::numeric_limits<float>::quiet_NaN()}),
                         math_error_message(MathErrorCode::nonfinite_quaternion), MathError);
}

TEST_CASE("STEP sampling holds each key, and sampling clamps after the last key") {
    const auto asset = fixture();
    auto step = asset.animations[0];
    step.channels.resize(1);
    step.channels[0].interpolation = Interpolation::step;
    step.channels[0].times = {0, .5, 1};
    step.channels[0].values = {{0, 0, 0, 0}, {3, 0, 0, 0}, {5, 0, 0, 0}};
    CHECK(sample_pose(asset, &step, .499).world[0][12] == Near{0, tolerance});
    CHECK(sample_pose(asset, &step, .5).world[0][12] == Near{3, tolerance});
    CHECK(sample_pose(asset, &step, 10).world[0][12] == Near{5, tolerance});
}

TEST_CASE("Playback crosses each event once through pause, restart, seek and resume") {
    const auto asset = fixture();
    Playback player;
    player.select(asset.animations[0], {"test", false, {{.53, "marker"}}});
    CHECK(player.advance(.529).empty());
    CHECK(player.advance(.001).size() == 1);
    CHECK(player.advance(.001).empty());
    player.pause();
    const auto paused = player.time();
    CHECK(player.advance(10).empty());
    CHECK(player.time() == Near{paused, tolerance});
    player.resume();
    // A clip that does not loop stops at its end and holds the end pose.
    CHECK(player.advance(1).empty());
    CHECK(player.finished());
    CHECK_FALSE(player.playing());
    CHECK(player.time() == Near{1, tolerance});
    CHECK(player.advance(10).empty());
    player.restart();
    CHECK(player.advance(.54).size() == 1);
    player.seek(.7);
    CHECK(player.advance(.1).empty()); // A seek does not replay the event it passed.
    player.seek(1);
    player.resume();
    CHECK(player.time() == Near{0, tolerance}); // Resuming a finished clip restarts it.
}

TEST_CASE("A clip that does not loop accepts any finite step, and a looping step is bounded") {
    const auto asset = fixture();
    const auto &clip = asset.animations[0];
    Playback once;
    once.select(clip, {"test", false, {{.53, "marker"}}});
    CHECK(once.advance(1e9).size() == 1);
    CHECK(once.finished());
    Playback looping;
    looping.select(clip, {"test", true, {}});
    CHECK_THROWS_WITH_AS(looping.advance(clip.duration * 10'001),
                         "Playback step exceeds 10000 loops; split large offline advances", std::invalid_argument);
}

TEST_CASE("Negative and nonfinite playback steps are rejected") {
    constexpr auto invalid_step = "Playback elapsed time must be finite and nonnegative";
    const auto asset = fixture();
    Playback player;
    player.select(asset.animations[0], {"test", false, {{.53, "marker"}}});
    CHECK_THROWS_WITH_AS(player.advance(-1), invalid_step, std::invalid_argument);
    CHECK_THROWS_WITH_AS(player.advance(std::numeric_limits<double>::infinity()), invalid_step, std::invalid_argument);
}

TEST_CASE("Looping playback crosses events in every loop and at the loop boundary") {
    const auto asset = fixture();
    Playback player;
    player.select(asset.animations[0], {"test", true, {{.53, "loop_event"}}});
    CHECK(player.advance(2.6).size() == 3);
    CHECK(player.time() == Near{.6, tolerance});
    player.select(asset.animations[0], {"test", true, {{0, "start"}, {1, "end"}}});
    CHECK(player.advance(0).empty());
    CHECK(player.advance(.2).size() == 1);
    CHECK(player.advance(.8).size() == 2); // The end of one loop and the start of the next.
    CHECK(player.advance(.01).empty());
}

TEST_CASE("Playback names the metadata of another clip and rejects a negative or nonfinite duration") {
    auto clip = fixture().animations[0];
    Playback player;
    CHECK_THROWS_WITH_AS(player.select(clip, {"other", false, {}}), "Clip metadata names another clip: other",
                         std::invalid_argument);
    for (const auto duration :
         {-1., std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        CAPTURE(duration);
        clip.duration = duration;
        CHECK_THROWS_WITH_AS(player.select(clip, {"test", false, {}}),
                             "Clip duration must be finite and nonnegative: test", std::invalid_argument);
    }
    const auto pose = pose_clip();
    CHECK_THROWS_WITH_AS(player.select(pose, {"brace", false, {{.1, "late"}}}),
                         "Preview event outside animation duration", std::invalid_argument);
    CHECK_FALSE(player.animation());
}

TEST_CASE("A clip of zero duration plays as a pose that reports its events once") {
    const auto clip = pose_clip();
    Playback once;
    once.select(clip, {"brace", false, {{0, "set"}, {0, "hold"}}});
    CHECK(once.advance(0).empty());
    CHECK(once.playing());
    // The first nonzero step reports the events at 0, in their order, and ends a clip that does not loop.
    const auto events = once.advance(.1);
    REQUIRE(events.size() == 2);
    CHECK(events[0].name == "set");
    CHECK(events[1].name == "hold");
    CHECK(once.finished());
    CHECK_FALSE(once.playing());
    CHECK(once.time() == 0);
    once.resume(); // Resuming a finished clip restarts it, so its events are reported again.
    CHECK(once.advance(1e9).size() == 2);
    CHECK(once.finished());
    once.restart();
    once.seek(0); // Its start is its end.
    CHECK(once.finished());
    CHECK_FALSE(once.playing());

    // A looping pose holds time 0 through any finite step, without the loop bound, and reports its events once.
    Playback looping;
    looping.select(clip, {"brace", true, {{0, "set"}}});
    CHECK(looping.advance(.1).size() == 1);
    CHECK(looping.advance(1e9).empty());
    CHECK(looping.time() == 0);
    CHECK(looping.playing());
    CHECK_FALSE(looping.finished());
    looping.seek(2.5);
    CHECK(looping.time() == 0);
    CHECK(looping.playing());
    looping.restart();
    CHECK(looping.advance(.1).size() == 1);
}

TEST_CASE("An Animator plays a clip of zero duration as its pose") {
    auto asset = fixture();
    asset.animations.push_back(pose_clip());
    const auto source = std::make_shared<const Asset>(std::move(asset));
    Scene scene;
    const auto object = scene.create("body", Mesh::compile(*source));
    Animator animator(object, source);
    // Root translation along X of the published pose, checked against what the renderer holds.
    const auto root_x = [&] {
        const auto published = animator.pose().world.at(0)[12];
        CHECK(scene.instance(object.id()).palette.at(0)[12] == published);
        return published;
    };
    CHECK(root_x() == Near{0, tolerance});
    animator.play("brace", false);
    CHECK(root_x() == Near{3, tolerance});
    CHECK(animator.update(.5).empty());
    CHECK(animator.playback().finished());
    CHECK(root_x() == Near{3, tolerance});
    animator.play("test");
    (void)animator.update(.5);
    CHECK(root_x() == Near{1, tolerance});
    animator.play("brace");
    CHECK(animator.update(.5).empty());
    CHECK(animator.playback().playing());
    CHECK(root_x() == Near{3, tolerance});
}

TEST_CASE("A fitted model binds to the body's joints by name") {
    const auto asset = fixture();
    CHECK(compatible_skin(asset, asset).size() == 2);
    // Joint indices may be ordered differently.
    auto reordered = asset;
    std::swap(reordered.skins[0].joints[0], reordered.skins[0].joints[1]);
    std::swap(reordered.skins[0].inverse_bind[0], reordered.skins[0].inverse_bind[1]);
    CHECK(compatible_skin(asset, reordered).size() == 2);
}

TEST_CASE("A fitted model that does not match the body's rig is rejected with its reason") {
    const auto asset = fixture();
    auto fitted = asset;
    fitted.primitives[0].skin = -1;
    CHECK_THROWS_WITH_AS(compatible_skin(asset, fitted),
                         "Fitted model contains an unskinned mesh; bind it to the body's rig", std::runtime_error);
    fitted = asset;
    fitted.skins[0].inverse_bind[1][12] = .1F;
    CHECK_THROWS_WITH_AS(compatible_skin(asset, fitted),
                         "Fitted inverse-bind mismatch at hand (max error 0.100000); export the fitted model for this "
                         "body",
                         std::runtime_error);
    fitted = asset;
    fitted.nodes[1].parent = -1;
    CHECK_THROWS_WITH_AS(compatible_skin(asset, fitted),
                         "Fitted hierarchy mismatch at hand; export against the body's rig", std::runtime_error);
    fitted = asset;
    fitted.nodes[1].rest.translation.y = 2;
    CHECK_THROWS_WITH_AS(compatible_skin(asset, fitted),
                         "Fitted rest-pose mismatch at hand; use the body's rest transforms", std::runtime_error);
    fitted = asset;
    fitted.nodes[1].name = "another_hand";
    CHECK_THROWS_WITH_AS(compatible_skin(asset, fitted), "Fitted joint missing from the body: another_hand",
                         std::runtime_error);
}

TEST_CASE("A fitted model follows the body pose, so it cannot bring clips of its own") {
    const auto asset = fixture();
    CHECK_THROWS_WITH_AS(FittedAsset(asset, std::make_shared<const Asset>(asset)),
                         "Fitted models follow the body pose and cannot own motion", std::invalid_argument);
    auto still = asset;
    still.animations.clear();
    CHECK(FittedAsset(asset, std::make_shared<const Asset>(still)).joints.size() == 2);
}

TEST_CASE("A fitted model compiled until upload keeps a source without texels") {
    const auto body = fixture();
    auto textured = body;
    textured.animations.clear();
    textured.materials.push_back({"cloth", {1, 1, 1}, 0});
    textured.primitives[0].material = 0;
    textured.textures.push_back({std::make_shared<Image>(Image{1, 1, {255, 128, 64, 255}}), {}});
    auto source = std::make_shared<const Asset>(textured);
    textured.textures.clear();
    const std::weak_ptr<const Image> authored = source->textures[0].image;
    const FittedAsset kept(body, source);
    CHECK(kept.source == source);
    CHECK(kept.render->texel_retention() == TexelRetention::keep);
    const FittedAsset fitted(body, std::move(source), TexelRetention::until_upload);
    REQUIRE(fitted.source->textures.size() == 1);
    // The source's textures are the mesh's, which describe the image without holding its texels.
    CHECK(fitted.source->textures[0].image == fitted.render->materials()->textures[0].image);
    CHECK(fitted.source->textures[0].image->rgba.empty());
    CHECK(fitted.source->primitives.size() == 1);
    CHECK(fitted.joints.size() == 2);
    fitted.render->release_texels();
    REQUIRE_FALSE(authored.expired()); // The kept fitted model still holds its source.
    CHECK(fitted.render->texel_images().at(0) == authored.lock());
}

TEST_CASE("Base-color mipmaps average in linear light and keep odd edges") {
    const Texture checker{std::make_shared<Image>(Image{2, 1, {0, 0, 0, 255, 255, 255, 255, 255}}), {}};
    const auto mips = base_color_mips(checker);
    REQUIRE(mips.size() == 2);
    CHECK(mips[1].rgba[0] == 188); // Averaged in encoded sRGB, this would be 128.
    CHECK(mips[1].rgba[3] == 255);
    const Texture odd{std::make_shared<Image>(Image{3, 1, {0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255}}), {}};
    CHECK(base_color_mips(odd).back().rgba[0] == 213);
}

TEST_CASE(preview_test) {
    REQUIRE(exported_manifest);
    const auto manifest = read_manifest(*exported_manifest);
    const auto actual = load_asset(manifest.directory / manifest.model);
    REQUIRE_NOTHROW(validate_manifest(manifest, *actual));
    AssetPreview preview(*exported_manifest);
    for (const auto &metadata : manifest.clips) {
        CAPTURE(metadata.name);
        const auto &clip = find_animation(*actual, metadata.name);
        preview.select(metadata.name);
        (void)preview.advance(clip.duration * .25);
        CHECK(deviation(preview.pose(), sample_pose(*actual, &clip, clip.duration * .25)) < tolerance);
        const auto held_pose = preview.pose();
        preview.toggle_play();
        CHECK(preview.advance(.2).empty()); // Paused.
        CHECK(deviation(held_pose, preview.pose()) < tolerance);
        preview.bind_pose();
        CHECK(deviation(preview.pose(), sample_pose(*actual)) < tolerance);
        CHECK_THROWS_WITH_AS(preview.seek(-1), "Invalid playback seek", std::invalid_argument);
        CHECK(preview.is_bind()); // The rejected seek kept bind mode.
        preview.restart();
        (void)preview.advance(clip.duration * .25);
        CHECK(deviation(preview.pose(), held_pose) < tolerance);
    }
    if (manifest.clips.empty()) {
        preview.toggle_play();
        preview.restart();
        CHECK(preview.advance(.25).empty());
        CHECK(preview.is_bind());
        CHECK(deviation(preview.pose(), sample_pose(*actual)) < tolerance);
    }
    MESSAGE("Fixture: ", actual->nodes.size(), " nodes, ", actual->skins[0].joints.size(), " joints; ",
            manifest.clips.size(), " declared clips");
}

int main(int argc, char **argv) {
    doctest::Context context(argc, argv);
    // asset_preview names an exported manifest: the first argument that is not a doctest option.
    for (int i = 1; i < argc; ++i)
        if (argv[i][0] != '-') {
            exported_manifest = argv[i];
            break;
        }
    if (!exported_manifest)
        context.addFilter("test-case-exclude", preview_test);
    return context.run();
}
