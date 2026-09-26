#pragma once
// This application's own generated actors and documents. No game assets,
// third-party JSON API, or engine implementation headers are used.
#include <anima/assets/actor_presentation.hpp>
#include <anima/assets/attachments.hpp>
#include <anima/assets/fitted.hpp>
#include <bit>
#include <chrono>
#include <fstream>
#include <locale>
#include <sstream>
#include <string_view>

namespace presentation_test {
using namespace anima;
inline void check(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
template <class F> void rejects(F operation) {
    try {
        operation();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error("Invalid presentation contract accepted");
}
// Requires operation to throw an Error whose message is exactly expected.
template <class Error, class F> void rejects_as(F operation, const std::string &expected) {
    try {
        operation();
    } catch (const Error &error) {
        if (error.what() == expected)
            return;
        throw std::runtime_error("Expected \"" + expected + "\", got \"" + error.what() + "\"");
    }
    throw std::runtime_error("Expected rejection: " + expected);
}
// text with its first from replaced by to; the fixture must contain from.
inline std::string with_first(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    check(at != std::string::npos, "Fixture text to replace is missing");
    return text.replace(at, from.size(), to);
}
inline std::string matrix_json(const Mat4 &matrix) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << '[';
    for (std::size_t i = 0; i < matrix.size(); ++i) {
        if (i)
            out << ',';
        out << matrix[i];
    }
    out << ']';
    return out.str();
}
inline void integer(std::vector<char> &data, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        data.push_back(static_cast<char>((value >> (i * 8)) & 255));
}
inline void scalar(std::vector<char> &data, float value) { integer(data, std::bit_cast<std::uint32_t>(value)); }
inline void glb(const std::filesystem::path &file, std::string json, const std::vector<char> &binary) {
    while (json.size() % 4)
        json += ' ';
    std::vector<char> bytes;
    integer(bytes, 0x46546c67);
    integer(bytes, 2);
    integer(bytes, static_cast<std::uint32_t>(28 + json.size() + binary.size()));
    integer(bytes, static_cast<std::uint32_t>(json.size()));
    integer(bytes, 0x4e4f534a);
    bytes.insert(bytes.end(), json.begin(), json.end());
    integer(bytes, static_cast<std::uint32_t>(binary.size()));
    integer(bytes, 0x004e4942);
    bytes.insert(bytes.end(), binary.begin(), binary.end());
    std::ofstream out(file, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    check(bool(out), "Cannot write independent actor fixture");
}
inline void text_file(const std::filesystem::path &file, const std::string &contents) {
    std::ofstream out(file, std::ios::binary);
    out << contents;
    check(bool(out), "Cannot write independent actor document");
}
struct Workspace {
    std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("anima-presentation-consumer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Workspace() { std::filesystem::create_directories(directory); }
    ~Workspace() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
};
// A node that no fixture clip animates; the motion resource authors it as a matrix.
constexpr unsigned matrix_node = 2;
struct Fixture {
    std::filesystem::path directory;
    std::vector<std::string> names;
    std::string contract;
    std::string sockets;
    std::string catalog;
};
inline Fixture actor_fixture(const std::filesystem::path &directory, unsigned limbs) {
    std::filesystem::create_directories(directory);
    Fixture result;
    result.directory = directory;
    const auto count = 1 + 3 * limbs;
    std::vector<Vec3> local(count), world(count);
    std::vector<int> parents(count, -1);
    for (unsigned i = 0; i < count; ++i)
        result.names.push_back((limbs == 2 ? "pilot." : "crawler.") + std::to_string(i));
    for (unsigned limb = 0; limb < limbs; ++limb) {
        const auto first = 1 + 3 * limb;
        local[first] = {limb % 2 ? .6F : -.6F, 1, limb < 2 ? 0.F : -1.F};
        local[first + 1] = {0, -.4F, .1F};
        local[first + 2] = {0, -.4F, -.1F};
        parents[first] = 0;
        parents[first + 1] = static_cast<int>(first);
        parents[first + 2] = static_cast<int>(first + 1);
    }
    for (unsigned i = 1; i < count; ++i)
        world[i] = world[static_cast<std::size_t>(parents[i])] + local[i];
    // The motion resource authors one unanimated node as a matrix, which must transfer as its TRS placement.
    std::ostringstream nodes, motion_nodes;
    nodes.imbue(std::locale::classic());
    motion_nodes.imbue(std::locale::classic());
    nodes << '[';
    motion_nodes << '[';
    for (unsigned i = 0; i < count; ++i) {
        if (i) {
            nodes << ',';
            motion_nodes << ',';
        }
        nodes << "{\"name\":\"" << result.names[i] << "\",\"translation\":[" << local[i].x << ',' << local[i].y << ','
              << local[i].z << ']';
        motion_nodes << "{\"name\":\"" << result.names[i] << '"';
        if (i == matrix_node)
            motion_nodes << ",\"matrix\":[1,0,0,0,0,1,0,0,0,0,1,0," << local[i].x << ',' << local[i].y << ','
                         << local[i].z << ",1]";
        else
            motion_nodes << ",\"translation\":[" << local[i].x << ',' << local[i].y << ',' << local[i].z << ']';
        std::string children;
        for (unsigned j = 1; j < count; ++j)
            if (parents[j] == static_cast<int>(i)) {
                if (!children.empty())
                    children += ',';
                children += std::to_string(j);
            }
        if (!children.empty()) {
            nodes << ",\"children\":[" << children << ']';
            motion_nodes << ",\"children\":[" << children << ']';
        }
        nodes << '}';
        motion_nodes << '}';
    }
    const auto root_nodes = motion_nodes.str() + ']';
    const auto model_nodes = nodes.str() + ",{\"name\":\"surface\",\"mesh\":0,\"skin\":0}]";
    std::vector<char> mesh;
    for (float n : {-.2F, 0.F, 0.F, .2F, 0.F, 0.F, 0.F, .4F, 0.F})
        scalar(mesh, n);
    for (unsigned i = 0; i < 3; ++i)
        for (float n : {0.F, 0.F, 1.F})
            scalar(mesh, n);
    for (unsigned i = 0; i < 3; ++i)
        for (float n : {1.F, 0.F, 0.F, 0.F})
            scalar(mesh, n);
    mesh.resize(132, 0);
    for (const auto position : world) {
        auto bind = identity();
        bind[12] = -position.x;
        bind[13] = -position.y;
        bind[14] = -position.z;
        for (float n : bind)
            scalar(mesh, n);
    }
    std::string joints;
    for (unsigned i = 0; i < count; ++i) {
        if (i)
            joints += ',';
        joints += std::to_string(i);
    }
    const auto geometry = [&](const std::vector<char> &binary, bool animated) {
        const auto motion_offset = 132 + count * 64;
        return std::string(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,)") + std::to_string(count) +
               "]}],\"nodes\":" + model_nodes + ",\"buffers\":[{\"byteLength\":" + std::to_string(binary.size()) +
               R"(}],"bufferViews":[
            {"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36},
            {"buffer":0,"byteOffset":72,"byteLength":48},{"buffer":0,"byteOffset":120,"byteLength":12},
            {"buffer":0,"byteOffset":132,"byteLength":)" +
               std::to_string(count * 64) + "}" +
               (animated ? ",{\"buffer\":0,\"byteOffset\":" + std::to_string(motion_offset) +
                               ",\"byteLength\":8},{\"buffer\":0,\"byteOffset\":" + std::to_string(motion_offset + 8) +
                               ",\"byteLength\":24}"
                         : "") +
               R"(],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
            {"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},
            {"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"},
            {"bufferView":3,"componentType":5121,"count":3,"type":"VEC4"},
            {"bufferView":4,"componentType":5126,"count":)" +
               std::to_string(count) + R"(,"type":"MAT4"})" +
               (animated ? R"(,{"bufferView":5,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},
            {"bufferView":6,"componentType":5126,"count":2,"type":"VEC3"})"
                         : "") +
               R"(],"skins":[{"joints":[)" + joints +
               R"(],"inverseBindMatrices":4}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"WEIGHTS_0":2,"JOINTS_0":3}}]}])" +
               (animated
                    ? R"(,"animations":[{"name":"extend","samplers":[{"input":5,"output":6}],"channels":[{"sampler":0,"target":{"node":1,"path":"translation"}}]}])"
                    : "") +
               "}";
    };
    glb(directory / "actor.glb", geometry(mesh, false), mesh);
    auto prop = mesh;
    scalar(prop, 0);
    scalar(prop, 1);
    for (float n : {-.6F, 1.F, 0.F, -.6F, 1.F, .25F})
        scalar(prop, n);
    glb(directory / "instrument.glb", geometry(prop, true), prop);
    std::vector<char> motion;
    for (float n : {0.F, 1.F, 0.F, 0.F, 0.F, .2F, 0.F, 0.F, 0.F, 0.F, 0.F, 1.F, 0.F, 0.F, .24740396F, .96891242F})
        scalar(motion, n);
    const auto animation = [](const char *name, unsigned node, unsigned output, const char *path) {
        return std::string("{\"name\":\"") + name +
               "\",\"samplers\":[{\"input\":0,\"output\":" + std::to_string(output) +
               "}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":" + std::to_string(node) + ",\"path\":\"" + path +
               "\"}}]}";
    };
    glb(directory / "motion.glb",
        std::string(R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":)") + root_nodes +
            R"(,"buffers":[{"byteLength":64}],
        "bufferViews":[{"buffer":0,"byteLength":8},{"buffer":0,"byteOffset":8,"byteLength":24},{"buffer":0,"byteOffset":32,"byteLength":32}],
        "accessors":[{"bufferView":0,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},
        {"bufferView":1,"componentType":5126,"count":2,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":2,"type":"VEC4"}],"animations":[)" +
            animation("drift", 0, 1, "translation") + "," + animation("signal", 1, 2, "rotation") + "," +
            animation("carry.port", 1, 2, "rotation") + "," + animation("carry.starboard", 4, 2, "rotation") + "]}",
        motion);
    const auto quoted = [&](unsigned n) { return '"' + result.names[n] + '"'; };
    const auto signature = std::string(64, 'a');
    const auto skeleton = "{\"id\":\"consumer.rig\",\"joint_count\":" + std::to_string(count) +
                          ",\"bind_signature\":\"" + signature + "\"}";
    std::string hierarchy;
    for (unsigned i = 0; i < count; ++i) {
        if (i)
            hierarchy += ',';
        hierarchy += quoted(i) + ":" + (parents[i] < 0 ? "null" : quoted(static_cast<unsigned>(parents[i])));
    }
    const auto ownership = [&](const char *mask, unsigned start) {
        return std::string("{\"mask\":\"") + mask + "\",\"owned_joints\":[" + quoted(start) + "," + quoted(start + 1) +
               "," + quoted(start + 2) + "],\"context_joints\":[" + quoted(0) + "]}";
    };
    result.contract =
        "{\"version\":3,\"skeleton\":" + skeleton +
        R"(,"resource":"motion.glb","evaluation":{"version":1,"id":"consumer.evaluation","parents":{)" + hierarchy +
        "},\"masks\":{\"port\":[" + quoted(1) + "],\"starboard\":[" + quoted(4) +
        "]},\"chains\":{\"starboard\":{\"joints\":[" + quoted(4) + "," + quoted(5) + "," + quoted(6) +
        "],\"minimum_angle\":0,\"maximum_angle\":3.13}}}," +
        R"("clips":[{"name":"drift","loop":true,"reference_speed":0.2,"events":[]},{"name":"signal","loop":false,"events":[]}],"layers":{"carry.port":)" +
        ownership("port", 1) + ",\"carry.starboard\":" + ownership("starboard", 4) + "}}";
    text_file(directory / "motion.json", result.contract);
    text_file(directory / "actor.asset.json",
              R"({"schema_version":1,"units":"meters","asset_id":"consumer.actor","model":"actor.glb","skeleton":)" +
                  skeleton + R"(,"clips":[],"equipment":[],"motion_contract":"motion.json"})");
    text_file(
        directory / "actor.profile.json",
        R"({"version":1,"id":"consumer.profile","manifest":"actor.asset.json","capabilities":["can.signal"],"sockets":{"port":{"node":)" +
            quoted(3) + R"(,"frame":null},"starboard":{"node":)" + quoted(6) + R"(,"frame":null}}})");
    auto port_frame = identity();
    port_frame[12] = world[3].x;
    port_frame[13] = world[3].y;
    port_frame[14] = world[3].z;
    auto starboard_frame = identity();
    starboard_frame[12] = world[6].x;
    starboard_frame[13] = world[6].y;
    starboard_frame[14] = world[6].z;
    const auto unit = matrix_json(identity());
    result.sockets = "{\"skeleton\":\"consumer.rig\",\"bind_signature\":\"" + signature + "\",\"rest_joints\":{" +
                     quoted(3) + ":" + matrix_json(port_frame) + "," + quoted(6) + ":" + matrix_json(starboard_frame) +
                     "},\"sockets\":{\"port\":{\"node\":" + quoted(3) + ",\"local\":" + unit +
                     "},\"starboard\":{\"node\":" + quoted(6) + ",\"local\":" + unit + "}}}";
    result.catalog =
        R"({"schema_version":2,"units":"meters","empty_handling":"free","defaults":{"instrument":"probe","light":"beacon"},"handling":[
        {"id":"free","socket":"","carry":""},{"id":"port","socket":"port","carry":"carry.port"},{"id":"starboard","socket":"starboard","carry":"carry.starboard"}],
        "visuals":[{"id":"instrument","model":"instrument.glb","primary_grip":)" +
        unit + ",\"markers\":{\"tip\":" + unit + "},\"primary_node\":" + quoted(0) +
        ",\"marker_nodes\":{\"tip\":" + quoted(3) + R"(},"animation_tracks":{"pulse":"extend"}}],"items":[
        {"id":"probe","category":"instrument","visual":"instrument","handling":"port"},{"id":"beacon","category":"navigation","visual":"instrument","handling":"starboard"}]})";
    return result;
}
inline void run() {
    Workspace workspace;
    for (unsigned limbs : {2U, 4U}) {
        const auto fixture = actor_fixture(workspace.directory / std::to_string(limbs), limbs);
        ActorPresentation actor(fixture.directory / "actor.profile.json");
        check(actor.manifest.joint_count == 1 + 3 * limbs && actor.actor.asset->animations.empty(),
              "Independent anatomy/body binding failed");
        auto motion = actor.actor.motion;
        const auto baseline = motion->sample("drift", .5);
        const auto rest = actor.actor.asset->nodes[matrix_node].rest.translation;
        const auto transferred = baseline.local[matrix_node].translation;
        check(std::abs(transferred.x - rest.x) < 1e-5F && std::abs(transferred.y - rest.y) < 1e-5F &&
                  std::abs(transferred.z - rest.z) < 1e-5F,
              "A matrix-authored motion node lost its placement in transfer");
        check(std::abs(baseline.world[0][12] - .1F) < 1e-6F, "Independent motion did not drive the actor");
        const auto fit_item = [&](std::string_view id, std::string_view model) {
            return std::string(R"({"id":")") + std::string(id) +
                   R"(","slot":"surface","fits":{"consumer.profile":{"model":")" + std::string(model) +
                   R"(","skeleton":"consumer.rig","bind_signature":")" + actor.manifest.bind_signature + R"("}}})";
        };
        const auto fits = std::string(R"({"version":1,"items":[)") + fit_item("shell", "actor.glb") + "," +
                          fit_item("shell.alt", "actor.glb") + "," + fit_item("shell.broken", "missing.glb") + "]}";
        FittedLibrary fitted(actor.actor.asset, actor.manifest, "consumer.profile", fits);
        auto shell = fitted.load("shell");
        check(shell == fitted.load("shell") && shell->slot == "surface" && fitted.resident_assets().size() == 1,
              "Independent fitted mesh slot/cache failed");
        const auto fit_pose = shell->pose(baseline);
        for (const auto &[fit, owner] : shell->joints)
            check(fit_pose.world.at(fit) == baseline.world.at(owner), "Fitted mesh lost its owner pose");
        auto animated_fit = std::make_shared<Asset>(*shell->source);
        animated_fit->animations.emplace_back();
        rejects([&] { FittedAsset invalid("surface", *actor.actor.asset, animated_fit); });
        auto wrong_fit = std::make_shared<Asset>(*shell->source);
        wrong_fit->skins.front().inverse_bind.front()[12] += .1F;
        rejects([&] { FittedAsset invalid("surface", *actor.actor.asset, wrong_fit); });
        auto wrong_manifest = actor.manifest;
        wrong_manifest.bind_signature = std::string(64, 'b');
        rejects([&] { FittedLibrary invalid(actor.actor.asset, wrong_manifest, "consumer.profile", fits); });
        {
            // A game-defined frame driver and native late follower compose without
            // depending on the order of component types inside a phase.
            struct PoseDriver {
                GameObject owner;
                Pose pose;
                void on_update(double) {
                    owner.set_position({4, 0, 0});
                    owner.renderer().set_pose(pose);
                }
            };
            Scene scene;
            auto body = scene.create("Component owner", actor.render);
            auto clothing = body.add_component<FittedSet>(fitted);
            clothing->replace({"shell"});
            const auto child = clothing->instances().front().object;
            (void)body.add_component<PoseDriver>(baseline);
            scene.update(.5);
            Scene reference;
            auto expected = reference.create("Expected", shell->render);
            expected.set_position({4, 0, 0});
            expected.renderer().set_pose(shell->pose(baseline));
            check(child.parent()->id() == body.id() &&
                      scene.instance(child.id()).palette == reference.instance(expected.id()).palette,
                  "Fitted component did not follow the final frame pose during late update");
            body.remove_component<FittedSet>();
            check(!clothing && !child.valid() && body.children().empty() && scene.size() == 1,
                  "Removing a fitted component left its scene-owned children alive");
        }
        {
            Scene scene;
            const auto body = scene.create("Owner", actor.render);
            auto world = identity();
            world[12] = 4;
            scene.set_pose(body.id(), baseline, world);
            auto set = std::make_unique<FittedSet>(body, fitted);
            set->replace({"shell"});
            const auto object = set->instances().front().object;
            Scene reference;
            const auto expected = reference.add(shell->render);
            reference.set_pose(expected, shell->pose(baseline), world);
            check(scene.size() == 2 && object.world_matrix() == world &&
                      scene.instance(object.id()).palette == reference.instance(expected).palette,
                  "Equipping did not immediately inherit the current body pose and placement");
            set->replace({"shell"});
            check(set->instances().front().object.id() == object.id() && scene.size() == 2,
                  "Unchanged fitted membership recreated its object");
            rejects([&] { set->replace({"shell.alt", "shell.broken"}); });
            check(scene.size() == 2 && set->instances().front().object.id() == object.id() &&
                      scene.instance(object.id()).palette == reference.instance(expected).palette,
                  "Failed replacement changed the accepted fitted set");

            auto second = scene.create("Independent owner", actor.render);
            FittedSet independent(second, fitted);
            independent.replace({"shell"});
            const auto second_object = independent.instances().front().object;
            const auto second_palette = scene.instance(second_object.id()).palette;
            check(second_object.renderer().mesh() == object.renderer().mesh(), "Fitted sets duplicated shared meshes");
            world[14] = 3;
            const auto later = motion->sample("drift", .75);
            scene.set_pose(body.id(), later, world);
            set->sync();
            reference.set_pose(expected, shell->pose(later), world);
            check(scene.instance(object.id()).palette == reference.instance(expected).palette &&
                      scene.instance(second_object.id()).palette == second_palette,
                  "Fitted pose synchronization lost placement or changed another owner");
            scene.set_visible(body.id(), false);
            set->sync();
            check(!scene.instance(object.id()).visible && scene.instance(second_object.id()).visible,
                  "Fitted visibility escaped its owner");
            set->replace({"shell.alt"});
            const auto replacement = set->instances().front().object;
            check(!object.valid() && !scene.instance(replacement.id()).visible &&
                      scene.instance(replacement.id()).palette == reference.instance(expected).palette,
                  "Re-equipping a hidden actor exposed a fitted mesh or reset its pose");
            world[12] = -2;
            scene.set_transform(body.id(), world);
            scene.set_visible(body.id(), true);
            set->sync();
            reference.set_pose(expected, shell->pose(later), world);
            check(scene.instance(replacement.id()).visible &&
                      scene.instance(replacement.id()).palette == reference.instance(expected).palette,
                  "Showing a fitted set failed to catch up to the current body transform");
            set->replace({});
            check(!replacement.valid() && scene.size() == 3, "Unequipping leaked fitted objects");
            set->replace({"shell"});
            const auto last_fit = set->instances().front().object;
            scene.remove(body.id());
            const auto reused = scene.create("Reused body slot", actor.render);
            rejects([&] { set->sync(); });
            rejects([&] { set->replace({}); });
            set.reset();
            check(reused.valid() && !last_fit.valid() && second_object.valid(),
                  "Fitted cleanup destroyed a reused owner slot or another set");

            auto wrong_body = std::make_shared<Asset>(*actor.actor.asset);
            wrong_body->nodes.front().name += ".other";
            auto foreign = scene.create("Incompatible owner", Mesh::compile(*wrong_body));
            rejects([&] { FittedSet invalid(foreign, fitted); });
            second.renderer().set_mesh(Mesh::compile(*actor.actor.asset));
            rejects([&] { independent.sync(); });
            rejects([&] { FittedSet invalid(GameObject{}, fitted); });
        }
        {
            auto scene = std::make_unique<Scene>();
            auto set = std::make_unique<FittedSet>(scene->create("Expiring owner", actor.render), fitted);
            set->replace({"shell"});
            const auto object = set->instances().front().object;
            scene.reset();
            check(!object.valid(), "Fitted objects extended scene lifetime");
            rejects([&] { set->sync(); });
            set.reset();
        }
        shell.reset();
        check(fitted.resident_assets().empty(), "Unused fitted mesh remained resident");
        const std::array<std::string_view, 2> carries{"carry.port", "carry.starboard"};
        const auto carried = motion->compose_loadout("drift", .5, carries);
        check(carried.world[0] == baseline.world[0], "Attachments replaced root locomotion");
        rejects([&] {
            const std::array<std::string_view, 2> overlap{"carry.port", "carry.port"};
            motion->validate_carries(overlap);
        });
        auto wrong = actor.manifest;
        wrong.bind_signature = std::string(64, 'b');
        rejects([&] { MotionRuntime invalid(actor.actor.asset, wrong, fixture.contract); });
        rejects([&] { MotionRuntime invalid({}, actor.manifest, fixture.contract); });
        // Unknown fields are rejected at every level, as in the other presentation documents.
        using Edit = std::pair<std::string_view, std::string_view>;
        const std::array misspellings{Edit{R"("reference_speed":0.2)", R"("reference_sped":0.2)"},
                                      Edit{R"({"version":3,)", R"({"version":3,"extra":1,)"}};
        for (const auto &[from, to] : misspellings) {
            auto contract = fixture.contract;
            const auto at = contract.find(from);
            check(at != std::string::npos, "Motion contract fixture changed");
            contract.replace(at, from.size(), to);
            bool rejected = false;
            try {
                MotionRuntime invalid(actor.actor.asset, actor.manifest, contract);
            } catch (const std::invalid_argument &) {
                rejected = true;
            }
            check(rejected, "Motion contract accepted an unknown field");
        }
        AttachmentLibrary library(decode_attachment_catalog(fixture.catalog, fixture.directory));
        const auto sockets = decode_attachment_sockets(fixture.sockets, actor.manifest, *actor.actor.asset);
        auto attachments = AttachmentSet::prepare(library, sockets, {{"probe", "probe"}, {"beacon", "beacon"}});
        validate_attachment_ownership(*motion, library, attachments, sockets);
        Scene scene;
        auto attachment_owner = scene.create("Attachment owner", actor.render);
        attachment_owner.set_position({3, 0, 0});
        attachment_owner.renderer().set_pose(baseline);
        attachment_owner.renderer().set_visible(false);
        attachments.add(attachment_owner);
        check(scene.instances().size() == 3, "Two named attachments were not independently assembled");
        const auto bound = *attachments.roles.at("probe").instance;
        check(scene.object(bound).parent()->id() == attachment_owner.id() && !scene.instance(bound).visible,
              "Held attachment did not inherit owner lifetime or initial visibility");
        const auto held_world = scene.object(bound).world_matrix();
        attachment_owner.set_position({5, 0, 0});
        check(std::abs(scene.object(bound).position().x - held_world[12] - 2) < 1e-5F,
              "Held attachment did not follow owner movement");
        rejects([&] { attachments.add(attachment_owner); });
        Scene wrong_scene;
        rejects([&] { attachments.remove(wrong_scene); });
        check(scene.contains(bound) && scene.instances().size() == 3,
              "Repeated/foreign attachment operation lost objects");
        const auto before = scene.instances().size();
        rejects([&] { AttachmentSet::prepare(library, sockets, {{"unknown", "missing"}}); });
        check(scene.instances().size() == before, "Invalid preparation changed the live scene");
        const auto &visual = library.visual("instrument");
        const auto resource = library.load("instrument");
        const auto rest_prop = sample_attachment_pose(*resource, visual);
        const auto end_prop = sample_attachment_pose(*resource, visual, "pulse", 1);
        const auto tip0 = point(attachment_marker(visual, resource->source.get(), &rest_prop, "tip"), {});
        const auto tip1 = point(attachment_marker(visual, resource->source.get(), &end_prop, "tip"), {});
        check(std::abs(tip1.z - tip0.z - .25F) < 1e-5F, "Animated attachment marker did not move");
        rejects([&] { sample_attachment_pose(*resource, visual, "missing", .5); });
        const auto binding =
            animated_attachment_binding(attachments.roles.at("probe").binding, visual, *resource->source, end_prop);
        const auto placement = attachment_placement(baseline, binding);
        const auto primary = placement * end_prop.world[0] * visual.primary_grip;
        const auto expected = baseline.world[sockets.at("port").node] * sockets.at("port").local;
        for (unsigned i = 0; i < 16; ++i)
            check(std::abs(primary[i] - expected[i]) < 1e-5F, "Animated grip lost its primary socket");
        constexpr auto actions =
            R"({"schema_version":1,"actions":[{"id":"signal","handling":["port"],"roles":{"probe":["port"],"beacon":["starboard"]},"phases":[
            {"id":"prepare","duration":0.2,"layers":[{"clip":"signal","mask":"port","interval":[0,0.5]}],"props":[{"role":"probe","track":"pulse","interval":[0,1]}]},
            {"id":"sustain","duration":0.4,"held":true,"layers":[{"clip":"signal","mask":"port","interval":[0.5,0.5]}]},
            {"id":"release","duration":0.2,"layers":[{"clip":"signal","mask":"port","interval":[0.5,1]}],"cues":[{"id":"signal.emit","at":0}]},
            {"id":"recover","duration":0.2,"layers":[{"clip":"signal","mask":"port","interval":[1,0]}]}]}]})";
        ActionRuntime runtime(actor.actor.asset, motion, actions);
        runtime.validate_timing("signal", "port", .2, .6);
        ActionSetCatalog choices(
            R"({"version":1,"sets":{"operator":{"use":[{"action":"signal","requires":["can.signal"]}]}}})", runtime);
        check(choices.resolve("operator", "use", actor.capabilities) == "signal",
              "Independent capabilities did not select an action");
        rejects([&] {
            resolve_action(std::array{ActionVariant{"a", {"x"}}, ActionVariant{"b", {"y"}}}, Capabilities{"x", "y"});
        });
        const auto handling = validate_attachment_action(runtime, library, attachments, "signal");
        const auto holding = runtime.sample(carried, {"signal", 1, .7, {}, {}}, handling);
        check(holding.clock.phase == 1 && holding.pose.world[0] == carried.world[0],
              "Held action changed base motion or failed to sustain");
        const auto released = runtime.sample(carried, {"signal", 1, .9, .8, {}}, handling);
        check(released.clock.phase == 2, "Action failed to release");
        ActionCueCursor cues;
        const auto &timeline = runtime.definition("signal").timeline;
        check(cues.advance("signal", 1, timeline, .7).empty(), "Held action emitted release cue");
        check(cues.advance("signal", 1, timeline, .9, .8).size() == 1 &&
                  cues.advance("signal", 1, timeline, .9, .8).empty(),
              "Release cue duplicated or disappeared");
        ActionCueCursor late;
        check(late.advance("signal", 1, timeline, .9, .8).empty(), "Late consumer replayed old cues");
        auto missing = attachments;
        missing.roles.erase("beacon");
        rejects([&] { validate_attachment_action(runtime, library, missing, "signal"); });
        rejects([&] {
            ActionRuntime invalid(actor.actor.asset, motion, R"({"schema_version":1,"schema_version":1,"actions":[]})");
        });
        rejects_as<std::out_of_range>([&] { (void)runtime.definition("absent"); }, "Unknown action: absent");
        rejects_as<std::out_of_range>([&] { (void)motion->clip("absent"); }, "Missing animation: absent");
        rejects_as<std::out_of_range>([&] { (void)motion->metadata("absent"); }, "Unknown base motion/action: absent");
        rejects_as<std::out_of_range>([&] { (void)motion->layer_mask("absent"); }, "Unknown handling layer: absent");
        rejects_as<std::out_of_range>([&] { (void)motion->contact_end_node("absent"); },
                                      "Unknown motion chain: absent");
        MotionLayer unknown_layer;
        unknown_layer.clip = "drift";
        unknown_layer.mask = "absent";
        MotionControls unknown_mask;
        unknown_mask.layers.push_back(unknown_layer);
        rejects_as<std::out_of_range>([&] { (void)motion->evaluate(baseline, unknown_mask); },
                                      "Unknown motion mask: absent");
        MotionControls unknown_joint;
        unknown_joint.offsets.push_back({.joint = "absent"});
        rejects_as<std::out_of_range>([&] { (void)motion->evaluate(baseline, unknown_joint); },
                                      "Unknown evaluation joint: absent");
        rejects_as<std::out_of_range>([&] { (void)choices.resolve("absent", "use", actor.capabilities); },
                                      "Missing presentation reference: absent");
        rejects_as<std::invalid_argument>(
            [&] {
                ActionRuntime unknown(actor.actor.asset, motion,
                                      with_first(actions, R"("clip":"signal")", R"("clip":"absent")"));
            },
            "Missing animation: absent");
        rejects_as<std::invalid_argument>(
            [&] {
                ActionRuntime unknown(
                    actor.actor.asset, motion,
                    with_first(actions, R"("props":[)", R"("contacts":{"absent":[[0,1],[1,1]]},"props":[)"));
            },
            "Unknown motion chain: absent");
        rejects_as<std::invalid_argument>(
            [&] {
                ActionSetCatalog unknown(
                    R"({"version":1,"sets":{"operator":{"use":[{"action":"absent","requires":[]}]}}})", runtime);
            },
            "Unknown action: absent");
        MotionControls contact;
        const auto end = point(baseline.world[sockets.at("starboard").node], {});
        contact.contacts.push_back({"starboard", end, {2, 0, 1}, 1, {}});
        const auto solved = motion->evaluate(baseline, contact);
        check(solved.contacts.size() == 1 && solved.contacts[0].reachable && solved.contacts[0].error < 1e-4F,
              "Generic contact solve failed on independent anatomy");
        attachments.remove(scene);
        check(scene.instances().size() == 1, "Attachment removal leaked scene instances");
        attachments.add(attachment_owner);
        attachment_owner.destroy();
        attachments.remove(scene);
        check(scene.size() == 0, "Actor destruction retained held attachments");
    }
    ActorPresentation pilot(workspace.directory / "2/actor.profile.json"),
        crawler(workspace.directory / "4/actor.profile.json");
    InteractionRuntime::Actors actors{{"pilot", pilot.actor}, {"crawler", crawler.actor}};
    const std::string layers = R"({"layers":[{"clip":"drift","interval":[0,1]}]})";
    const std::string roles = "{\"pilot\":{\"dock\":" + layers + ",\"link\":" + layers + ",\"undock\":" + layers +
                              "},\"crawler\":{\"dock\":" + layers + ",\"link\":" + layers + ",\"undock\":" + layers +
                              "}}";
    const std::string weights = R"({"dock":[[0,0],[1,1]],"link":[[0,1],[1,1]],"undock":[[0,1],[1,0]]})";
    const std::string edge =
        R"({"child":"pilot","parent":"crawler","child_socket":"port","parent_socket":"port","weights":)" + weights +
        "}";
    const auto interaction_document = [&](const std::string &edges) {
        return R"({"version":1,"id":"dock","phases":[{"id":"dock","duration":0.2},{"id":"link","duration":0.4,"held":true},{"id":"undock","duration":0.2}],"roles":)" +
               roles + ",\"attachments\":[" + edges +
               R"(],"contacts":[{"child":"pilot","parent":"crawler","chain":"starboard","target_socket":"starboard","pole":[2,0,1],"weights":)" +
               weights + "}]}";
    };
    InteractionRuntime interaction(actors, interaction_document(edge));
    rejects_as<std::invalid_argument>(
        [&] {
            InteractionRuntime unknown(
                actors, with_first(interaction_document(edge), R"("chain":"starboard")", R"("chain":"absent")"));
        },
        "Unknown motion chain: absent");
    auto parent_world = identity();
    parent_world[12] = 3;
    parent_world[14] = -2;
    std::map<std::string, Mat4, std::less<>> worlds{{"pilot", identity()}, {"crawler", parent_world}};
    const auto linked = interaction.sample(.5, {}, worlds);
    const auto &parent = linked.frames.at(interaction.bindings().role("crawler"));
    const auto &child = linked.frames.at(interaction.bindings().role("pilot"));
    const auto parent_socket = parent.world * interaction_socket(parent.pose, crawler.actor.sockets.at("port"));
    const auto child_socket = child.world * interaction_socket(child.pose, pilot.actor.sockets.at("port"));
    check(parent.world == parent_world && linked.clock.phase == 1,
          "Coordinated actors changed caller placement or lost held phase");
    for (unsigned i = 0; i < 16; ++i)
        check(std::abs(parent_socket[i] - child_socket[i]) < 1e-5F, "Independent actor docking anchors separated");
    check(linked.contacts.size() == 1 && linked.contacts[0].reachable && linked.contacts[0].error < 1e-4F,
          "Coordinated contact failed across independent anatomies");
    const auto unlinked = interaction.sample(1.1, .8, worlds);
    check(unlinked.frames.at(interaction.bindings().role("pilot")).world == worlds.at("pilot"),
          "Released actor did not return to caller placement");
    const std::string reverse =
        R"({"child":"crawler","parent":"pilot","child_socket":"port","parent_socket":"port","weights":)" + weights +
        "}";
    rejects([&] { InteractionRuntime cycle(actors, interaction_document(edge + "," + reverse)); });
    worlds.erase("pilot");
    rejects([&] { interaction.sample(.5, {}, worlds); });
    std::cout << "PASS standalone presentation: independent 7/13-joint actors, separate motion, two attachments, "
                 "capabilities, held/released actions, markers, contacts, coordinated actors and rejection gates\n";
}
} // namespace presentation_test
