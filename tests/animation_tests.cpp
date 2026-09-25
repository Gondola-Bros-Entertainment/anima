#include <anima/assets/preview.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <locale>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
void near(double a, double b, const char *message, double tolerance = 1e-5) {
    require(std::abs(a - b) < tolerance, message);
}
template <class F> void rejects(F action, const std::string &expected = "") {
    try {
        action();
    } catch (const std::exception &error) {
        if (expected.empty() || std::string(error.what()).find(expected) != std::string::npos)
            return;
        throw;
    }
    throw std::runtime_error("Expected rejection: " + expected);
}
float deviation(const anima::Pose &a, const anima::Pose &b) {
    float d = 0;
    for (std::size_t i = 0; i < a.world.size(); ++i)
        for (unsigned k = 0; k < 16; ++k)
            d = std::max(d, std::abs(a.world[i][k] - b.world[i][k]));
    return d;
}
anima::Asset fixture() {
    anima::Asset asset;
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
    anima::AssetSkin skin;
    skin.joints = {0, 1};
    skin.inverse_bind = {anima::identity(), anima::identity()};
    skin.inverse_bind[1][13] = -1;
    asset.skins.push_back(skin);
    anima::SourcePrimitive primitive;
    primitive.node = 2;
    primitive.skin = 0;
    primitive.mesh_name = "triangle";
    for (auto p : {anima::Vec3{0, 2, 0}, anima::Vec3{1, 2, 0}, anima::Vec3{0, 3, 0}}) {
        anima::SourceVertex v;
        v.position = p;
        v.normal = {0, 0, 1};
        v.joints = {1, 0, 0, 0};
        v.weights = {1, 0, 0, 0};
        primitive.vertices.push_back(v);
    }
    asset.primitives.push_back(primitive);
    asset.mesh_nodes = 1;
    anima::Animation clip;
    clip.name = "test";
    clip.duration = 1;
    clip.channels.push_back(
        {0, anima::ChannelPath::translation, anima::Interpolation::linear, {0, 1}, {{0, 0, 0, 0}, {2, 0, 0, 0}}});
    clip.channels.push_back(
        {1, anima::ChannelPath::rotation, anima::Interpolation::linear, {0, 1}, {{0, 0, 0, 1}, {0, 0, 1, 0}}});
    asset.animations.push_back(clip);
    return asset;
}
void manifest_tests(const anima::Asset &asset) {
    struct Temp {
        std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("anima-manifest-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
        ~Temp() {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } temp;
    const std::string valid = R"({"schema_version":1,"units":"meters","asset_id":"two-joint-body",
        "model":"body.glb","skeleton":{"id":"humanoid","joint_count":2,
        "bind_signature":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"},
        "clips":[{"name":"test","loop":false,"events":[{"time_seconds":0.53,"event":"swing\uD83D\uDDE1"}]}],
        "equipment":[]})";
    const auto read = [&](const std::string &json) {
        {
            std::ofstream out(temp.path, std::ios::binary);
            out << json;
        }
        return anima::read_manifest(temp.path);
    };
    const auto changed = [&](const std::string &from, const std::string &to) {
        auto json = valid;
        json.replace(json.find(from), from.size(), to);
        return json;
    };
    const auto manifest = read(valid);
    anima::validate_manifest(manifest, asset);
    require(manifest.joint_count == 2 && !manifest.clips[0].loop, "Manifest body count/loop policy lost");
    require(manifest.clips[0].events[0].name == "swing\xF0\x9F\x97\xA1", "Manifest Unicode escape decoding");
    require(!manifest.clips[0].reference_speed, "Clip without travel metadata gained an implicit travel speed");
    const auto travel = read(changed("\"loop\":false", "\"loop\":false,\"reference_speed\":3.2"));
    anima::validate_manifest(travel, asset);
    near(*travel.clips[0].reference_speed, 3.2, "Authored travel speed was lost");
    for (const auto speed : {"0", "-1"})
        rejects(
            [&] { (void)read(changed("\"loop\":false", std::string("\"loop\":false,\"reference_speed\":") + speed)); },
            "finite and positive");
    auto invalid_speed = travel;
    invalid_speed.clips[0].reference_speed = std::numeric_limits<double>::infinity();
    rejects([&] { anima::validate_manifest(invalid_speed, asset); }, "finite and positive");
    {
        struct Comma : std::numpunct<char> {
            char do_decimal_point() const override { return ','; }
        };
        struct RestoreLocale {
            std::locale previous = std::locale();
            ~RestoreLocale() { std::locale::global(previous); }
        } restore;
        std::locale::global(std::locale(std::locale::classic(), new Comma));
        near(read(valid).clips[0].events[0].time, .53, "Manifest numbers depend on process locale");
    }
    for (const auto &bad : {changed("body.glb", "../body.glb"), changed("body.glb", "C:/body.glb"),
                            changed("body.glb", R"(body\u0000.glb)"), changed("body.glb", R"(folder\\body.glb)")})
        rejects([&] { (void)read(bad); }, "filename beside");
    for (const auto &bad : {changed("\"schema_version\":1", "\"schema_version\":1,\"schema_version\":1"),
                            changed("0.53", "1e999"), changed("0.53", "01"), changed(R"(\uD83D\uDDE1)", R"(\uD83D)"),
                            valid + "false", changed("two-joint-body", std::string("invalid-\xC0\xAF")),
                            changed("\"equipment\":[]", "\"equipment\":[],\"nested\":" + std::string(34, '[') + "0" +
                                                            std::string(34, ']'))})
        rejects([&] { (void)read(bad); }, "Invalid manifest JSON");
    rejects([&] { (void)read(changed("\"schema_version\":1", "\"schema_version\":2")); }, "schema_version");
    rejects([&] { (void)read(changed("\"joint_count\":2", "\"joint_count\":2.5")); }, "joint count");
    rejects([&] { anima::validate_manifest(read(changed("\"joint_count\":2", "\"joint_count\":3")), asset); },
            "joint count");
    rejects([&] { anima::validate_manifest(read(changed("0.53", "1.1")), asset); }, "outside clip");
    rejects([&] { anima::validate_manifest(read(changed("\"name\":\"test\"", "\"name\":\"missing\"")), asset); },
            "Missing animation");
}
} // namespace
int main(int argc, char **argv) {
    try {
        auto asset = fixture();
        manifest_tests(asset);
        const auto rest = anima::sample_pose(asset);
        const auto half = anima::sample_pose(asset, &asset.animations[0], .5);
        const auto end = anima::sample_pose(asset, &asset.animations[0], 1);
        const auto blended = anima::blend_pose(asset, rest, end, .5F);
        near(deviation(blended, half), 0, "Local-pose blending distorted joint rotation or child hierarchy");
        near(deviation(anima::blend_pose(asset, rest, end, 0), rest), 0, "Blend start endpoint");
        near(deviation(anima::blend_pose(asset, rest, end, 1), end), 0, "Blend end endpoint");
        // A 180-degree endpoint has two equally short arcs. Use 90 degrees
        // here so this verifies sign invariance of the unique shortest arc.
        auto equivalent = half;
        for (auto &transform : equivalent.local)
            for (auto &value : transform.rotation)
                value = -value;
        near(deviation(anima::blend_pose(asset, rest, equivalent, .5F), anima::blend_pose(asset, rest, half, .5F)), 0,
             "Blend took a long quaternion arc");
        for (float invalid : {-1.F, 2.F, std::numeric_limits<float>::quiet_NaN()})
            rejects([&] { (void)anima::blend_pose(asset, rest, end, invalid); }, "Pose blend");
        auto incomplete = rest;
        incomplete.local.pop_back();
        rejects([&] { (void)anima::blend_pose(asset, incomplete, end, .5F); }, "matching");
        auto matrix_asset = asset;
        matrix_asset.nodes[2].has_matrix = true;
        matrix_asset.nodes[2].rest_matrix = anima::identity();
        matrix_asset.nodes[2].rest_matrix[12] = 7;
        near(anima::blend_pose(matrix_asset, rest, end, .5F).world[2][12], 7, "Matrix-node rest transform was blended");
        near(half.world[0][12], 1, "Parent translation sampling");
        near(half.world[3][12], .5, "Moving socket hierarchy x");
        near(half.world[3][13], 1, "Moving socket hierarchy y");
        const auto scene = anima::make_mesh_snapshot(asset, half);
        near(scene.vertices[0].position.x, 0, "Quaternion skin/inverse-bind x");
        near(scene.vertices[0].position.y, 1, "Quaternion skin/inverse-bind y");
        near(asset.primitives[0].vertices[0].position.y, 2, "Source vertex mutated");
        near(asset.nodes[1].rest.translation.y, 1, "Rest transform mutated");
        const auto separate = anima::sample_pose(asset);
        near(deviation(rest, separate), 0, "Instances share mutable pose state");
        const auto antipodal = anima::slerp({0, 0, 0, 1}, {0, 0, 0, -1}, .5F);
        near(std::abs(antipodal[3]), 1, "Antipodal quaternion interpolation");
        const auto unit = anima::slerp({0, 0, 0, 2}, {0, 0, .001F, 1}, .5F);
        double norm = 0;
        for (auto v : unit)
            norm += v * v;
        near(norm, 1, "Quaternion normalization");
        rejects([] { (void)anima::unit_quaternion({0, 0, 0, 0}); }, "Zero quaternion");
        auto step = asset.animations[0];
        step.channels.resize(1);
        step.channels[0].interpolation = anima::Interpolation::step;
        step.channels[0].times = {0, .5, 1};
        step.channels[0].values = {{0, 0, 0, 0}, {3, 0, 0, 0}, {5, 0, 0, 0}};
        near(anima::sample_pose(asset, &step, .499).world[0][12], 0, "STEP changed early");
        near(anima::sample_pose(asset, &step, .5).world[0][12], 3, "STEP boundary wrong");
        near(anima::sample_pose(asset, &step, 10).world[0][12], 5, "Sampler did not clamp last key");
        anima::Playback player;
        const anima::ClipMetadata attack{"test", false, {{.53, "weapon_swing"}}};
        player.select(asset.animations[0], attack);
        require(player.advance(.529).empty(), "Event fired early");
        require(player.advance(.001).size() == 1, "Event crossing missing");
        require(player.advance(.001).empty(), "Event fired twice");
        player.pause();
        const auto paused = player.time();
        require(player.advance(10).empty(), "Paused event fired");
        near(player.time(), paused, "Pause advanced time");
        player.resume();
        require(player.advance(1).empty() && player.finished() && !player.playing(), "Attack did not stop at end");
        near(player.time(), 1, "Attack end pose not held");
        require(player.advance(10).empty(), "Finished attack fired again");
        player.restart();
        require(player.advance(.54).size() == 1, "Restart missed attack event");
        player.seek(.7);
        require(player.advance(.1).empty(), "Seek replayed crossed event");
        player.seek(1);
        player.resume();
        near(player.time(), 0, "Resume ended attack did not restart");
        rejects([&] { (void)player.advance(-1); });
        rejects([&] { (void)player.advance(std::numeric_limits<double>::infinity()); });
        const anima::ClipMetadata loop{"test", true, {{.53, "loop_event"}}};
        player.select(asset.animations[0], loop);
        require(player.advance(2.6).size() == 3, "Multi-loop events lost");
        near(player.time(), .6, "Loop remainder wrong");
        player.select(asset.animations[0], {"test", true, {{0, "start"}, {1, "end"}}});
        require(player.advance(0).empty(), "Zero step fired events");
        require(player.advance(.2).size() == 1, "Start event missing");
        require(player.advance(.8).size() == 2, "Loop boundary events wrong");
        require(player.advance(.01).empty(), "Loop boundary repeated");
        auto compatible = asset;
        require(anima::compatible_skin(asset, compatible).size() == 2, "Variable joint-count rig rejected");
        compatible.skins[0].inverse_bind[1][12] = .1F;
        rejects([&] { (void)anima::compatible_skin(asset, compatible); }, "inverse-bind");
        compatible = asset;
        compatible.nodes[1].parent = -1;
        rejects([&] { (void)anima::compatible_skin(asset, compatible); }, "hierarchy");
        compatible = asset;
        compatible.nodes[1].rest.translation.y = 2;
        rejects([&] { (void)anima::compatible_skin(asset, compatible); }, "rest-pose");
        compatible = asset;
        compatible.nodes[1].name = "another_hand";
        rejects([&] { (void)anima::compatible_skin(asset, compatible); }, "missing");
        auto reordered = asset;
        std::swap(reordered.skins[0].joints[0], reordered.skins[0].joints[1]);
        std::swap(reordered.skins[0].inverse_bind[0], reordered.skins[0].inverse_bind[1]);
        require(anima::compatible_skin(asset, reordered).size() == 2, "Joint index order treated as identity");
        const anima::Texture checker{2, 1, {0, 0, 0, 255, 255, 255, 255, 255}, {}};
        const auto mips = anima::base_color_mips(checker);
        require(mips.size() == 2 && mips[1].rgba[0] == 188 && mips[1].rgba[3] == 255,
                "Mip RGB averaged in encoded sRGB");
        const anima::Texture odd{3, 1, {0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255}, {}};
        require(anima::base_color_mips(odd).back().rgba[0] == 213, "Odd mip edge dropped");
        if (argc > 1) {
            const std::filesystem::path path = argv[1];
            const auto manifest = anima::read_manifest(path);
            const auto actual = anima::load_asset(manifest.directory / manifest.model);
            anima::validate_manifest(manifest, *actual);
            anima::AssetPreview preview(path);
            for (const auto &metadata : manifest.clips) {
                const auto &clip = anima::find_animation(*actual, metadata.name);
                preview.select(metadata.name);
                (void)preview.advance(clip.duration * .25);
                near(deviation(preview.pose(), anima::sample_pose(*actual, &clip, clip.duration * .25)), 0,
                     "Preview advanced to the wrong pose");
                const auto held_pose = preview.pose();
                preview.toggle_play();
                require(preview.advance(.2).empty(), "Paused fixture emitted event");
                near(deviation(held_pose, preview.pose()), 0, "Paused fixture moved");
                preview.bind_pose();
                near(deviation(preview.pose(), anima::sample_pose(*actual)), 0,
                     "Bind mode failed to restore rest hierarchy");
                bool rejected = false;
                try {
                    preview.seek(-1);
                } catch (const std::invalid_argument &) {
                    rejected = true;
                }
                require(rejected && preview.is_bind(), "Invalid seek changed bind mode");
                preview.restart();
                (void)preview.advance(clip.duration * .25);
                near(deviation(preview.pose(), held_pose), 0, "Preview restart changed sampled pose");
            }
            if (manifest.clips.empty()) {
                preview.toggle_play();
                preview.restart();
                require(preview.advance(.25).empty() && preview.is_bind(), "Static manifest started playback");
                near(deviation(preview.pose(), anima::sample_pose(*actual)), 0,
                     "Static manifest changed rest hierarchy");
            }
            std::cout << "Fixture: " << actual->nodes.size() << " nodes, " << actual->skins[0].joints.size()
                      << " joints; " << manifest.clips.size() << " declared clips; manifest preview passed\n";
        }
        std::cout << "PASS: hierarchy, quaternion LINEAR/STEP, independent poses, loops, pause/restart/seek, event "
                     "crossings, body-specific equipment compatibility, linear-light mipmaps\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
