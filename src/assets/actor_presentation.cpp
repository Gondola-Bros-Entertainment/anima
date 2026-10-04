#include "presentation_data.hpp"
#include "texel_hold.hpp"
#include <anima/assets/actor_presentation.hpp>
namespace anima {
namespace {
// The one actor presentation profile version ActorPresentation accepts.
constexpr int profile_version = 2;
} // namespace
ActorPresentation::ActorPresentation(const std::filesystem::path &profile,
                                     std::optional<std::filesystem::path> manifest_override,
                                     TexelRetention texel_retention, const StagingOptions &options) {
    using namespace presentation_data;
    using anima::operator*;
    // The JSON library's own failures, such as a value of the wrong type, become std::invalid_argument; the readers
    // this calls convert theirs.
    anima::detail::json_step([&] {
        const auto document = read(profile);
        anima::detail::json_version(document, "version", profile_version,
                                    "Unsupported actor presentation profile version");
        anima::detail::json_fields(document, {"version", "id", "manifest", "sockets"});
        if (!document.at("sockets").is_object())
            throw std::invalid_argument("Invalid actor presentation profile");
        id = text(document.at("id"));
        const auto relative = relative_document_path(text(document.at("manifest")), {},
                                                     "Actor manifest must be inside its profile directory");
        manifest = anima::read_manifest(manifest_override.value_or(profile.parent_path() / relative));
        auto model = anima::load_asset(manifest.directory / manifest.model, options);
        anima::validate_manifest(manifest, *model);
        render = anima::Mesh::compile(*model, texel_retention);
        // The motion runtime holds the model, which the interaction actor reads through it, so it gets the copy that
        // keeps none of the texels that the Mesh lets go.
        model = anima::detail::without_texels(std::move(model), *render);
        actor.motion = MotionRuntime::load(model, manifest, options);
        const auto rest = anima::sample_pose(*model);
        for (const auto &[name, socket] : document.at("sockets").items()) {
            anima::detail::json_fields(socket, {"node", "frame"});
            const auto node_name = text(socket.at("node"));
            std::optional<std::size_t> node;
            for (std::size_t i = 0; i < model->nodes.size(); ++i)
                if (model->nodes[i].name == node_name) {
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
    });
}
} // namespace anima
