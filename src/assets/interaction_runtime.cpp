#include "presentation_data.hpp"
#include <anima/assets/interaction_runtime.hpp>
namespace anima {
namespace {
// A document may hold this many contacts per actor, counted over the whole document.
constexpr std::size_t contacts_per_actor = 8;
} // namespace
struct InteractionRuntime::Impl {
  public:
    using Actors = std::map<std::string, InteractionActor, std::less<>>;
    Impl(Actors actors, const nlohmann::json &document) : actors_(std::move(actors)) {
        using namespace presentation_data;
        anima::detail::json_fields(document, {"version", "id", "phases", "roles", "attachments", "contacts"});
        if (document.at("version") != 1 || actors_.empty() || actors_.size() > 64 ||
            !document.at("roles").is_object() || document.at("roles").size() != actors_.size())
            throw std::invalid_argument("Invalid coordinated interaction");
        id_ = text(document.at("id"));
        std::vector<anima::ActionPhase> phases;
        for (const auto &phase : document.at("phases")) {
            anima::detail::json_fields(phase, {"id", "duration"}, {"held", "cues"});
            anima::ActionPhase result{
                text(phase.at("id")), phase.at("duration").get<double>(), phase.value("held", false), {}};
            if (phase.contains("cues"))
                for (const auto &cue : phase.at("cues")) {
                    anima::detail::json_fields(cue, {"id", "at"});
                    result.cues.push_back({text(cue.at("id")), cue.at("at").get<double>()});
                }
            phases.push_back(std::move(result));
        }
        timeline_ = std::make_unique<anima::ActionTimeline>(std::move(phases));
        std::vector<anima::InteractionRole> roles;
        for (const auto &[id, actor] : actors_) {
            if (!actor.asset)
                throw std::invalid_argument("Interaction role has no asset");
            const auto &definition = document.at("roles").at(id);
            if (!definition.is_object() || definition.size() != timeline_->phases().size())
                throw std::invalid_argument("Every interaction role needs every phase");
            nlohmann::json action{{"id", id_}, {"handling", {id}}, {"phases", nlohmann::json::array()}};
            for (const auto &phase : document.at("phases")) {
                const auto &layers = definition.at(phase.at("id").get<std::string>());
                // Other prop roles are explicit interaction participants. They
                // must not bypass the role graph through a hidden held-item path.
                anima::detail::json_fields(layers, {"layers"});
                auto entry = phase;
                entry["layers"] = layers.at("layers");
                action["phases"].push_back(std::move(entry));
            }
            runtimes_.emplace(id,
                              std::make_shared<ActionRuntime>(
                                  actor.motion, nlohmann::json{{"schema_version", 1}, {"actions", {action}}}.dump()));
            roles.push_back({id, actor.asset});
        }
        std::vector<anima::InteractionAttachment> attachments;
        if (!document.at("attachments").is_array())
            throw std::invalid_argument("Interaction attachments must be an array");
        for (const auto &entry : document.at("attachments")) {
            anima::detail::json_fields(entry, {"child", "parent", "child_socket", "parent_socket", "weights"});
            const auto child = text(entry.at("child")), parent = text(entry.at("parent"));
            attachments.push_back({child, parent, socket(child, text(entry.at("child_socket"))),
                                   socket(parent, text(entry.at("parent_socket")))});
            placement_weights_.push_back(weights(entry.at("weights")));
        }
        bindings_ = std::make_unique<anima::InteractionBindings>(std::move(roles), std::move(attachments));
        if (!document.at("contacts").is_array() || document.at("contacts").size() > actors_.size() * contacts_per_actor)
            throw std::invalid_argument("Invalid interaction contact count");
        std::set<std::pair<std::string, std::string>> owned_chains;
        for (const auto &entry : document.at("contacts")) {
            anima::detail::json_fields(entry, {"child", "parent", "chain", "target_socket", "pole", "weights"},
                                       {"orientation"});
            Contact contact{
                text(entry.at("child")),          text(entry.at("parent")),
                text(entry.at("chain")),          socket(text(entry.at("parent")), text(entry.at("target_socket"))),
                vector(entry.at("pole")),         weights(entry.at("weights")),
                entry.value("orientation", false)};
            const auto &child = actors_.at(contact.child);
            if (!child.motion || !owned_chains.emplace(contact.child, contact.chain).second)
                throw std::invalid_argument("Contact needs one owner and a compatible evaluation rig");
            (void)child.motion->contact_end_node(contact.chain);
            const auto attachment =
                std::find_if(bindings_->attachments().begin(), bindings_->attachments().end(), [&](const auto &value) {
                    return value.child == contact.child && value.parent == contact.parent;
                });
            if (attachment == bindings_->attachments().end() ||
                child.motion->contact_affects_node(contact.chain, attachment->child_socket.node))
                throw std::invalid_argument("Contact must target the placement owner without moving its child anchor");
            contacts_.push_back(std::move(contact));
        }
    }
    const anima::ActionTimeline &timeline() const { return *timeline_; }
    const anima::InteractionBindings &bindings() const { return *bindings_; }
    const std::string &id() const { return id_; }

    InteractionSample sample(double elapsed, std::optional<double> released,
                             const std::map<std::string, anima::Mat4, std::less<>> &free_worlds) const {
        using anima::operator*;
        if (free_worlds.size() != actors_.size())
            throw std::invalid_argument("Every interaction role needs a free placement");
        InteractionSample result{timeline_->sample(elapsed, released), {}, {}, {}};
        std::vector<anima::InteractionFrame> frames;
        for (const auto &role : bindings_->roles()) {
            const auto &actor = actors_.at(role.id);
            auto sampled = runtimes_.at(role.id)->sample(anima::sample_pose(*actor.asset),
                                                         ActionRequest{id_, 1, elapsed, released, {}}, role.id);
            frames.push_back({std::move(sampled.pose), free_worlds.at(role.id)});
        }
        std::vector<anima::InteractionPlacement> placements;
        for (const auto &curves : placement_weights_) {
            placements.push_back({curves.at(result.clock.phase).sample(result.clock.progress)});
            result.attachment_weights.push_back(placements.back().weight);
        }
        result.frames = bindings_->sample(frames, placements);
        // Parent contacts finish before dependent child roles. Body roots never
        // change during this pass, so attachments remain stable after solving.
        for (const auto index : bindings_->role_order()) {
            const auto &role = bindings_->roles()[index];
            auto &child = result.frames[index];
            // Parent contact solves can move an attachment socket. Resolve this
            // role after its parent has finished, always from its free placement.
            for (std::size_t i = 0; i < bindings_->attachments().size(); ++i) {
                const auto &binding = bindings_->attachments()[i];
                if (binding.child == role.id)
                    child.world =
                        anima::interaction_world(result.frames[bindings_->role(binding.parent)], binding.parent_socket,
                                                 child.pose, binding.child_socket, frames[index].world, placements[i]);
            }
            for (const auto &contact : contacts_)
                if (contact.child == role.id) {
                    const auto &parent = result.frames[bindings_->role(contact.parent)];
                    const auto target = anima::inverse(child.world) * parent.world *
                                        anima::interaction_socket(parent.pose, contact.target);
                    MotionControls controls;
                    const auto weight = contact.weights.at(result.clock.phase).sample(result.clock.progress);
                    controls.contacts.push_back(
                        {contact.chain, anima::point(target, {}), contact.pole, weight,
                         contact.orientation ? std::optional(anima::affine_rotation(target)) : std::nullopt});
                    auto evaluated = actors_.at(role.id).motion->evaluate(child.pose, controls);
                    for (const auto &value : evaluated.contacts)
                        result.contacts.push_back({role.id, value.chain, weight, value.error, value.reachable});
                    child.pose = std::move(evaluated.pose);
                }
        }
        return result;
    }

  private:
    struct Contact {
        std::string child, parent, chain;
        anima::InteractionSocket target;
        anima::Vec3 pole;
        std::vector<ActionWeight> weights;
        bool orientation{};
    };
    const anima::InteractionSocket &socket(std::string_view actor, std::string_view name) const {
        return presentation_data::lookup(presentation_data::lookup(actors_, actor).sockets, name);
    }
    std::vector<ActionWeight> weights(const nlohmann::json &value) const {
        if (!value.is_object() || value.size() != timeline_->phases().size())
            throw std::invalid_argument("Contact/attachment needs a curve for every interaction phase");
        std::vector<ActionWeight> result;
        for (const auto &phase : timeline_->phases())
            result.push_back(ActionRuntime::weight(value.at(phase.id).dump()));
        return result;
    }
    static anima::Vec3 vector(const nlohmann::json &value) {
        if (!value.is_array() || value.size() != 3)
            throw std::invalid_argument("Interaction pole requires three coordinates");
        const auto coordinates = value.get<std::array<float, 3>>();
        if (!std::all_of(coordinates.begin(), coordinates.end(), [](float n) { return std::isfinite(n); }))
            throw std::invalid_argument("Interaction pole must be finite");
        return {coordinates[0], coordinates[1], coordinates[2]};
    }
    Actors actors_;
    std::string id_;
    std::unique_ptr<anima::ActionTimeline> timeline_;
    std::unique_ptr<anima::InteractionBindings> bindings_;
    std::map<std::string, std::shared_ptr<const ActionRuntime>, std::less<>> runtimes_;
    std::vector<std::vector<ActionWeight>> placement_weights_;
    std::vector<Contact> contacts_;
};
InteractionRuntime::InteractionRuntime(Actors actors, std::string_view document)
    : impl_(presentation_data::decode_step(
          [&] { return std::make_shared<Impl>(std::move(actors), presentation_data::parse(document)); })) {}
const ActionTimeline &InteractionRuntime::timeline() const { return impl_->timeline(); }
const InteractionBindings &InteractionRuntime::bindings() const { return impl_->bindings(); }
const std::string &InteractionRuntime::id() const { return impl_->id(); }
InteractionSample InteractionRuntime::sample(double elapsed, std::optional<double> released,
                                             const std::map<std::string, Mat4, std::less<>> &free_worlds) const {
    return impl_->sample(elapsed, released, free_worlds);
}
} // namespace anima
