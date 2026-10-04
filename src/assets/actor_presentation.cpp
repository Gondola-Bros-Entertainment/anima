#include "presentation_data.hpp"
#include <anima/assets/actor_presentation.hpp>
namespace anima {
ActorPresentation::ActorPresentation(const std::filesystem::path &profile,
                                     std::optional<std::filesystem::path> manifest_override) {
    using namespace presentation_data;
    const auto document = read(profile);
    anima::detail::json_version(document, "version", 2, "Unsupported actor presentation profile version");
    anima::detail::json_fields(document, {"version", "id", "manifest", "sockets"});
    if (!document.at("sockets").is_object())
        throw std::invalid_argument("Invalid actor presentation profile");
    id = text(document.at("id"));
    const auto relative = std::filesystem::path(text(document.at("manifest")));
    if (relative.is_absolute() ||
        std::any_of(relative.begin(), relative.end(), [](const auto &part) { return part == ".."; }))
        throw std::invalid_argument("Actor manifest must be inside its profile directory");
    manifest = anima::read_manifest(manifest_override.value_or(profile.parent_path() / relative));
    actor.asset = anima::load_asset(manifest.directory / manifest.model);
    anima::validate_manifest(manifest, *actor.asset);
    actor.motion = MotionRuntime::load(actor.asset, manifest);
    render = anima::Mesh::compile(*actor.asset);
    const auto rest = anima::sample_pose(*actor.asset);
    for (const auto &[name, socket] : document.at("sockets").items()) {
        anima::detail::json_fields(socket, {"node", "frame"});
        const auto node_name = text(socket.at("node"));
        std::optional<std::size_t> node;
        for (std::size_t i = 0; i < actor.asset->nodes.size(); ++i)
            if (actor.asset->nodes[i].name == node_name) {
                if (node)
                    throw std::invalid_argument("Ambiguous actor socket node");
                node = i;
            }
        if (!node || name.empty())
            throw std::invalid_argument("Unknown or unnamed actor socket");
        // frame is a rigid model-space bind frame. null uses the joint's bind
        // position with the model's axes, for a socket that ignores the joint's rotation.
        const auto frame = socket.at("frame").is_null()
                               ? anima::matrix(anima::Transform{anima::point(rest.world[*node], {})})
                               : presentation_data::matrix(socket.at("frame"), true);
        const auto local = anima::inverse(rest.world[*node]) * frame;
        actor.sockets.emplace(name, anima::InteractionSocket{*node, local});
    }
}
} // namespace anima
