#pragma once
// Consume the independent Blender project's real compiled output using only
// public engine headers. Run after tests/authoring/consumer.py.
#include "presentation.hpp"
#include <anima/assets/fitted.hpp>
namespace compiled_presentation_test {
using namespace anima;
using presentation_test::check;
using presentation_test::rejects;
inline std::string read(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    check(bool(stream), "Missing compiled consumer document");
    return {std::istreambuf_iterator<char>(stream), {}};
}
inline int run(const std::filesystem::path &project) {
    for (const std::string name : {"pilot", "crawler"}) {
        const auto directory = project / name;
        ActorPresentation actor(directory / "actor.profile.json");
        check(actor.manifest.joint_count == (name == "pilot" ? 7U : 13U), "Compiled anatomy changed");
        const auto &motion = actor.actor.motion;
        const std::array<std::string_view, 1> carries{"carry.tool"};
        const auto posed = motion->compose_loadout("drift", .15, carries);
        FittedLibrary fitted(actor.actor.asset, actor.manifest, name, read(directory / "compiled/compiled-fits.json"));
        check(fitted.size() == 1 && fitted.known("shell"), "Compiled fitted mesh was not registered");
        auto shell = fitted.load("shell");
        check(shell->slot == "surface" && shell == fitted.load("shell") && fitted.resident_assets().size() == 1,
              "Fitted slot semantics or shared residency changed");
        const auto fit_pose = shell->pose(posed);
        check(fit_pose.local.empty() && !shell->joints.empty(), "Fitted mesh invented local animation");
        for (const auto &[fit, body] : shell->joints)
            check(fit_pose.world.at(fit) == posed.world.at(body), "Fitted mesh diverged from its owner pose");
        FittedAsset extra("another-slot", *actor.actor.asset,
                          load_asset(directory / "fitted" / (name + ".extra-shell.glb")));
        check(!extra.joints.empty(), "Separately exported fitted mesh did not bind");
        auto wrong = actor.manifest;
        wrong.bind_signature = std::string(64, 'b');
        rejects([&] {
            FittedLibrary invalid(actor.actor.asset, wrong, name, read(directory / "compiled/compiled-fits.json"));
        });
        rejects([&] { fitted.load("missing"); });
        auto animated = std::make_shared<Asset>(*shell->source);
        animated->animations.emplace_back();
        rejects([&] { FittedAsset invalid("surface", *actor.actor.asset, animated); });
        auto foreign = std::make_shared<Asset>(*shell->source);
        foreign->skins.front().inverse_bind.front()[12] += .1F;
        rejects([&] { FittedAsset invalid("surface", *actor.actor.asset, foreign); });
        shell.reset();
        check(fitted.resident_assets().empty(), "Fitted cache retained unused geometry");
        AttachmentLibrary library(decode_attachment_catalog(read(directory / "attachments.json"), directory));
        std::map<std::string, AttachmentSocket, std::less<>> sockets;
        for (const auto &[id, socket] : actor.actor.sockets)
            sockets.emplace(id, AttachmentSocket{socket.node, socket.local});
        auto attachments = AttachmentSet::prepare(library, sockets, {{"tool", "probe"}});
        validate_attachment_ownership(*motion, library, attachments, sockets);
        ActionRuntime actions(actor.actor.asset, motion, read(directory / "actions.json"));
        const auto handling = validate_attachment_action(actions, library, attachments, "signal");
        const auto holding = actions.sample(posed, {"signal", 1, .35, {}, {}}, handling);
        check(holding.clock.phase == 1 && holding.props.size() == 1,
              "Compiled action lost its held phase or prop track");
        const auto released = actions.sample(posed, {"signal", 1, .9, .8, {}}, handling);
        check(released.clock.phase == 2, "Compiled action failed to release");
        const auto &visual = library.visual("instrument");
        const auto prop = library.load("instrument");
        const auto first = sample_attachment_pose(*prop, visual, "pulse", 0);
        const auto last = sample_attachment_pose(*prop, visual, "pulse", 1);
        check(attachment_marker(visual, prop->source.get(), &first, "tip") !=
                  attachment_marker(visual, prop->source.get(), &last, "tip"),
              "Compiled animated prop marker did not move");
        const auto binding =
            animated_attachment_binding(attachments.roles.at("tool").binding, visual, *prop->source, last);
        const auto placed = attachment_placement(holding.pose, binding);
        const auto expected = holding.pose.world.at(sockets.at("tool").node) * sockets.at("tool").local;
        const auto primary_node = std::find_if(prop->source->nodes.begin(), prop->source->nodes.end(),
                                               [&](const auto &node) { return node.name == visual.primary_node; });
        check(primary_node != prop->source->nodes.end(), "Compiled prop lost its declared primary node");
        const auto primary = placed *
                             last.world.at(static_cast<std::size_t>(primary_node - prop->source->nodes.begin())) *
                             visual.primary_grip;
        for (unsigned i = 0; i < 16; ++i)
            check(std::abs(primary[i] - expected[i]) < 1e-5F, "Compiled prop grip separated from its socket");
    }
    std::cout << "PASS Blender-to-public-C++ consumer: two anatomies, motion, "
                 "fitted binding/residency/rejections, "
                 "attachments, animated props and held/released actions\n";
    return 0;
}
} // namespace compiled_presentation_test
