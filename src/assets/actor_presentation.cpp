#include "presentation_data.hpp"
#include <anima/assets/actor_presentation.hpp>
namespace anima {
ActorPresentation::ActorPresentation(const std::filesystem::path &profile,
                                     std::optional<std::filesystem::path> manifest_override) {
    using namespace presentation_data;
    using anima::operator*;
    const auto document = read(profile);
    anima::detail::json_fields(document, {"version", "id", "manifest", "capabilities", "sockets"});
    if (document.at("version") != 1 || !document.at("capabilities").is_array() || !document.at("sockets").is_object())
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
    for (const auto &capability : document.at("capabilities"))
        if (!capabilities.insert(text(capability)).second)
            throw std::invalid_argument("Duplicate actor capability");
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
        // frame is a rigid model-space bind frame. null uses the joint's
        // bind position with actor-forward orientation, useful for a pelvis.
        const auto frame = socket.at("frame").is_null()
                               ? anima::matrix(anima::Transform{anima::point(rest.world[*node], {})})
                               : presentation_data::matrix(socket.at("frame"), true);
        const auto local = anima::inverse(rest.world[*node]) * frame;
        actor.sockets.emplace(name, anima::InteractionSocket{*node, local});
    }
}
struct ActionSetCatalog::Impl {
  public:
    explicit Impl(const nlohmann::json &document, const ActionRuntime &actions) {
        using namespace presentation_data;
        anima::detail::json_fields(document, {"version", "sets"});
        if (document.at("version") != 1 || !document.at("sets").is_object())
            throw std::invalid_argument("Invalid action-set catalog");
        for (const auto &[name, slots] : document.at("sets").items()) {
            if (name.empty() || !slots.is_object() || slots.empty())
                throw std::invalid_argument("Invalid action set");
            for (const auto &[slot, variants] : slots.items()) {
                if (slot.empty() || !variants.is_array() || variants.empty() || variants.size() > 64)
                    throw std::invalid_argument("Invalid action variants");
                auto &destination = sets_[name][slot];
                for (const auto &variant : variants) {
                    anima::detail::json_fields(variant, {"action", "requires"});
                    ActionVariant value{text(variant.at("action")), {}};
                    (void)actions.definition(value.action);
                    if (!variant.at("requires").is_array())
                        throw std::invalid_argument("Action requirements must be an array");
                    for (const auto &capability : variant.at("requires"))
                        if (!value.requirements.insert(text(capability)).second)
                            throw std::invalid_argument("Duplicate action capability");
                    destination.push_back(std::move(value));
                }
            }
        }
    }
    const std::string &resolve(std::string_view set, std::string_view slot,
                               const std::set<std::string, std::less<>> &capabilities) const {
        const auto &variants = presentation_data::lookup(presentation_data::lookup(sets_, set), slot);
        return resolve_action(variants, capabilities);
    }

  private:
    std::map<std::string, std::map<std::string, std::vector<ActionVariant>, std::less<>>, std::less<>> sets_;
};
ActionSetCatalog::ActionSetCatalog(std::string_view document, const ActionRuntime &actions)
    : impl_(detail::json_step([&] { return std::make_shared<Impl>(presentation_data::parse(document), actions); })) {}
const std::string &ActionSetCatalog::resolve(std::string_view set, std::string_view slot,
                                             const Capabilities &capabilities) const {
    return impl_->resolve(set, slot, capabilities);
}
} // namespace anima
