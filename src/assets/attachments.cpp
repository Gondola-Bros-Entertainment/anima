#include "mesh_limits.hpp"
#include "presentation_data.hpp"
#include <anima/assets/attachments.hpp>
namespace anima {
namespace {
AttachmentCatalog decode_catalog(std::string_view document, const std::filesystem::path &directory) {
    using namespace presentation_data;
    const auto json = presentation_data::parse(document);
    anima::detail::json_fields(
        json, {"schema_version", "units", "empty_handling", "defaults", "handling", "visuals", "items"});
    if (json.at("schema_version") != 2 || json.at("units") != "meters")
        throw std::invalid_argument("Unsupported attachment catalog version or units");
    for (const auto name : {"handling", "visuals", "items"})
        if (!json.at(name).is_array() || json.at(name).empty())
            throw std::invalid_argument("Attachment catalog collections require nonempty arrays");
    AttachmentCatalog result;
    result.directory = directory;
    result.empty_handling = text(json.at("empty_handling"));
    for (const auto &entry : json.at("handling")) {
        anima::detail::json_fields(entry, {"id", "socket", "carry"}, {"carry_overrides", "support_contacts"});
        AttachmentHandling motion;
        motion.id = text(entry.at("id"));
        motion.socket = entry.at("socket").get<std::string>();
        motion.carry = entry.at("carry").get<std::string>();
        if (entry.contains("carry_overrides")) {
            if (!entry.at("carry_overrides").is_object())
                throw std::invalid_argument("Carry overrides must be named motions");
            for (const auto &[name, value] : entry.at("carry_overrides").items()) {
                anima::detail::json_fields(value, {"layer", "reason"});
                if (name.empty())
                    throw std::invalid_argument("Empty carry override motion");
                motion.carry_overrides.emplace(
                    name, AttachmentHandling::CarryOverride{text(value.at("layer")), text(value.at("reason"))});
            }
        }
        if (entry.contains("support_contacts")) {
            const auto &contacts = entry.at("support_contacts");
            if (!contacts.is_array() || contacts.empty() || contacts.size() > 4)
                throw std::invalid_argument("Handling contacts require one to four explicit constraints");
            std::set<std::string> chains;
            for (const auto &value : contacts) {
                anima::detail::json_fields(value, {"chain", "socket", "marker", "pole", "clips", "reason"},
                                           {"actions"});
                AttachmentContact contact;
                contact.chain = text(value.at("chain"));
                contact.socket = text(value.at("socket"));
                contact.marker = text(value.at("marker"));
                contact.reason = text(value.at("reason"));
                const auto pole = value.at("pole").get<std::array<float, 3>>();
                if (std::any_of(pole.begin(), pole.end(), [](float x) { return !std::isfinite(x); }))
                    throw std::invalid_argument("Non-finite support contact pole");
                contact.pole = {pole[0], pole[1], pole[2]};
                if (contact.socket == motion.socket || !chains.insert(contact.chain).second)
                    throw std::invalid_argument("Support contacts need distinct chains and a secondary socket");
                const auto &active = value.at("clips");
                if (!active.is_array())
                    throw std::invalid_argument("Support contacts need explicit clip coverage");
                for (const auto &clip : active) {
                    const auto name = text(clip);
                    if (!contact.clips.insert(name).second)
                        throw std::invalid_argument("Unknown/duplicate support contact clip");
                }
                if (value.contains("actions")) {
                    if (!value.at("actions").is_array())
                        throw std::invalid_argument("Contact action coverage must be an array");
                    for (const auto &id : value.at("actions"))
                        if (!contact.actions.insert(text(id)).second)
                            throw std::invalid_argument("Duplicate contact action");
                }
                if (contact.clips.empty() && contact.actions.empty())
                    throw std::invalid_argument("Support contact has no active clips/actions");
                motion.support_contacts.push_back(std::move(contact));
            }
        }
        insert(result.motions, std::move(motion));
    }
    if (!lookup(result.motions, result.empty_handling).socket.empty())
        throw std::invalid_argument("Empty handling must not declare an attachment socket");
    for (const auto &entry : json.at("visuals")) {
        anima::detail::json_fields(entry, {"id", "model", "primary_grip", "markers"},
                                   {"primary_node", "marker_nodes", "animation_tracks"});
        AttachmentVisual visual;
        visual.id = text(entry.at("id"));
        visual.model = text(entry.at("model"));
        if (visual.model.is_absolute() || visual.model.extension() != ".glb" ||
            std::any_of(visual.model.begin(), visual.model.end(), [](const auto &part) { return part == ".."; }))
            throw std::invalid_argument("Attachment model must be a relative GLB inside the catalog directory");
        visual.primary_grip = matrix(entry.at("primary_grip"), true);
        if (!entry.at("markers").is_object())
            throw std::invalid_argument("Attachment markers must be named frames");
        for (const auto &[name, value] : entry.at("markers").items()) {
            if (name.empty() || name == "primary")
                throw std::invalid_argument("Invalid additional grip marker name");
            visual.markers.emplace(name, matrix(value, true));
        }
        if (entry.contains("primary_node"))
            visual.primary_node = text(entry.at("primary_node"));
        for (const auto name : {"marker_nodes", "animation_tracks"})
            if (entry.contains(name)) {
                if (!entry.at(name).is_object())
                    throw std::invalid_argument("Prop bindings must be named references");
                for (const auto &[key, value] : entry.at(name).items()) {
                    if (key.empty())
                        throw std::invalid_argument("Empty prop binding");
                    if (std::string_view(name) == "marker_nodes") {
                        if (!visual.markers.contains(key))
                            throw std::invalid_argument("Animated marker needs a local frame");
                        visual.marker_nodes.emplace(key, text(value));
                    } else
                        visual.animation_tracks.emplace(key, text(value));
                }
            }
        insert(result.visuals, std::move(visual));
    }
    for (const auto &entry : json.at("items")) {
        anima::detail::json_fields(entry, {"id", "category", "visual", "handling"}, {"action_override"});
        AttachmentDefinition item{text(entry.at("id")),
                                  text(entry.at("visual")),
                                  text(entry.at("handling")),
                                  text(entry.at("category")),
                                  {},
                                  {}};
        if (lookup(result.motions, item.handling).socket.empty())
            throw std::invalid_argument("Attachment items require a held handling profile");
        (void)lookup(result.visuals, item.visual);
        for (const auto &contact : lookup(result.motions, item.handling).support_contacts)
            (void)lookup(lookup(result.visuals, item.visual).markers, contact.marker);
        if (entry.contains("action_override")) {
            anima::detail::json_fields(entry.at("action_override"), {"action", "reason"});
            item.action = text(entry.at("action_override").at("action"));
            item.action_reason = text(entry.at("action_override").at("reason"));
        }
        insert(result.items, std::move(item));
    }
    if (!json.at("defaults").is_object())
        throw std::invalid_argument("Review defaults must be named items");
    std::set<std::string> review_items;
    for (const auto &[name, value] : json.at("defaults").items()) {
        if (name.empty())
            throw std::invalid_argument("Empty review group");
        const auto id = text(value);
        (void)lookup(result.items, id);
        if (!review_items.insert(id).second)
            throw std::invalid_argument("Duplicate review item");
    }
    // Preserve authored item order; neither groups nor column ordinals drive gameplay.
    for (const auto &item : json.at("items")) {
        const auto id = text(item.at("id"));
        if (review_items.contains(id))
            result.defaults.push_back(id);
    }
    return result;
}
std::map<std::string, AttachmentSocket, std::less<>>
decode_sockets(std::string_view document, const anima::Manifest &manifest, const anima::Asset &body) {
    using namespace presentation_data;
    const auto adapter = presentation_data::parse(document);
    anima::detail::json_fields(adapter, {"skeleton", "bind_signature", "rest_joints", "sockets"});
    if (adapter.at("skeleton") != manifest.skeleton_id || adapter.at("bind_signature") != manifest.bind_signature)
        throw std::invalid_argument("Attachment sockets belong to a different body bind contract");
    const auto rest = anima::sample_pose(body);
    for (const auto &[name, value] : adapter.at("rest_joints").items()) {
        const auto expected = matrix(value, false);
        const auto &actual = rest.world.at(anima::unique_node(body, name));
        for (std::size_t i = 0; i < actual.size(); ++i)
            if (std::abs(actual[i] - expected[i]) > anima::mesh_limits::rest_pose_tolerance)
                throw std::invalid_argument("Body attachment rest frame changed: " + name);
    }
    std::map<std::string, AttachmentSocket, std::less<>> result;
    for (const auto &[name, value] : adapter.at("sockets").items()) {
        anima::detail::json_fields(value, {"node", "local"});
        const auto bone = text(value.at("node"));
        if (name.empty() || !adapter.at("rest_joints").contains(bone))
            throw std::invalid_argument("Socket requires a named, checked rest joint");
        result.emplace(name, AttachmentSocket{anima::unique_node(body, bone), matrix(value.at("local"), false)});
    }
    return result;
}
} // namespace
AttachmentCatalog decode_attachment_catalog(std::string_view document, const std::filesystem::path &directory) {
    return presentation_data::decode_step([&] { return decode_catalog(document, directory); });
}
std::map<std::string, AttachmentSocket, std::less<>>
decode_attachment_sockets(std::string_view document, const anima::Manifest &manifest, const anima::Asset &body) {
    return presentation_data::decode_step([&] { return decode_sockets(document, manifest, body); });
}
AttachmentLibrary::AttachmentLibrary(AttachmentCatalog definition) : catalog(std::move(definition)) {}
const AttachmentDefinition &AttachmentLibrary::item(std::string_view id) const {
    return presentation_data::lookup(catalog.items, id);
}
const AttachmentVisual &AttachmentLibrary::visual(std::string_view id) const {
    return presentation_data::lookup(catalog.visuals, id);
}
const AttachmentHandling &AttachmentLibrary::motion(std::string_view item_id) const {
    return presentation_data::lookup(catalog.motions,
                                     item_id.empty() ? catalog.empty_handling : item(item_id).handling);
}
std::shared_ptr<const AttachmentAsset> AttachmentLibrary::load(std::string_view visual_id) const {
    const auto &definition = visual(visual_id);
    const auto path = std::filesystem::weakly_canonical(catalog.directory / definition.model);
    const std::scoped_lock lock(mutex_);
    auto &cached = models_[path];
    if (auto source = cached.source.lock())
        if (auto render = cached.render.lock()) {
            validate(*source, definition);
            return std::make_shared<AttachmentAsset>(AttachmentAsset{source, render});
        }
    auto source = anima::load_asset(path);
    validate(*source, definition);
    auto render = anima::Mesh::compile(*source);
    cached = {source, render};
    return std::make_shared<AttachmentAsset>(AttachmentAsset{std::move(source), std::move(render)});
}
std::vector<std::shared_ptr<const anima::Mesh>> AttachmentLibrary::resident_assets() const {
    const std::scoped_lock lock(mutex_);
    std::vector<std::shared_ptr<const anima::Mesh>> result;
    for (const auto &[path, cached] : models_) {
        (void)path;
        if (auto render = cached.render.lock())
            result.push_back(std::move(render));
    }
    return result;
}
void AttachmentLibrary::validate(const anima::Asset &source, const AttachmentVisual &visual) {
    if (!source.animations.empty() && visual.animation_tracks.empty())
        throw std::invalid_argument("Animated equipment needs declared semantic tracks");
    // A model that lacks a node or clip its visual names, or has several, fails to load.
    try {
        if (!visual.primary_node.empty())
            (void)anima::unique_node(source, visual.primary_node);
        for (const auto &[marker, node] : visual.marker_nodes) {
            (void)marker;
            (void)anima::unique_node(source, node);
        }
        for (const auto &[track, clip] : visual.animation_tracks) {
            (void)track;
            (void)anima::find_animation(source, clip);
        }
    } catch (const std::out_of_range &error) {
        throw std::runtime_error(error.what());
    } catch (const std::invalid_argument &error) {
        throw std::runtime_error(error.what());
    }
}
AttachmentBinding bind_attachment(const AttachmentSocket &socket, const AttachmentVisual &visual) {
    return {socket.node, anima::operator*(socket.local, anima::inverse(visual.primary_grip))};
}
anima::Mat4 attachment_placement(const anima::Pose &pose, const AttachmentBinding &binding, const anima::Mat4 &actor) {
    return anima::operator*(anima::operator*(actor, pose.world.at(binding.node)), binding.local);
}
anima::Pose sample_attachment_pose(const AttachmentAsset &asset, const AttachmentVisual &visual, std::string_view track,
                                   double progress, bool required) {
    if (!std::isfinite(progress) || progress < 0 || progress > 1)
        throw std::invalid_argument("Invalid equipment track progress");
    if (track.empty())
        return asset.render->rest_pose();
    const auto found = visual.animation_tracks.find(track);
    if (found == visual.animation_tracks.end()) {
        if (required)
            throw std::invalid_argument("Equipment lacks required action track: " + std::string(track));
        return asset.render->rest_pose();
    }
    const auto &animation = anima::find_animation(*asset.source, found->second);
    return anima::sample_pose(*asset.source, &animation, animation.duration * progress);
}
AttachmentBinding animated_attachment_binding(const AttachmentBinding &binding, const AttachmentVisual &visual,
                                              const anima::Asset &asset, const anima::Pose &pose) {
    if (visual.primary_node.empty())
        return binding;
    using anima::operator*;
    const auto primary = pose.world.at(anima::unique_node(asset, visual.primary_node)) * visual.primary_grip;
    return {binding.node, binding.local * visual.primary_grip * anima::inverse(primary)};
}
anima::Mat4 attachment_marker(const AttachmentVisual &visual, const anima::Asset *asset, const anima::Pose *pose,
                              std::string_view name) {
    auto local = presentation_data::lookup(visual.markers, name);
    const auto found = visual.marker_nodes.find(name);
    if (found == visual.marker_nodes.end())
        return local;
    if (!asset || !pose)
        throw std::invalid_argument("Animated equipment marker requires the sampled prop pose");
    return anima::operator*(pose->world.at(anima::unique_node(*asset, found->second)), local);
}
bool AttachmentInstance::equip(anima::Scene &scene, const AttachmentLibrary &library,
                               const std::map<std::string, AttachmentSocket, std::less<>> &sockets,
                               std::string_view id) {
    if (item_id == id)
        return false;
    AttachmentInstance next;
    next.item_id = id;
    if (!id.empty()) {
        const auto &item = library.item(id);
        next.category = item.category;
        next.binding =
            bind_attachment(presentation_data::lookup(sockets, library.motion(id).socket), library.visual(item.visual));
        next.asset = library.load(item.visual);
        next.instance = scene.add(next.asset->render);
    }
    if (instance)
        scene.remove(*instance);
    *this = std::move(next);
    return true;
}
bool AttachmentSet::matches(const std::map<std::string, std::string, std::less<>> &desired) const {
    if (roles.size() != desired.size())
        return false;
    for (const auto &[role, id] : desired) {
        const auto found = roles.find(role);
        if (found == roles.end() || found->second.item_id != id)
            return false;
    }
    return true;
}
AttachmentSet AttachmentSet::prepare(const AttachmentLibrary &library,
                                     const std::map<std::string, AttachmentSocket, std::less<>> &sockets,
                                     const std::map<std::string, std::string, std::less<>> &desired) {
    constexpr std::size_t maximum_held_roles = 8;
    if (desired.size() > maximum_held_roles)
        throw std::invalid_argument("Too many held attachment roles");
    AttachmentSet result;
    for (const auto &[role, id] : desired) {
        if (role.empty() || id.empty())
            throw std::invalid_argument("Empty attachment role/item");
        const auto &item = library.item(id);
        AttachmentInstance held;
        held.item_id = id;
        held.category = item.category;
        held.binding =
            bind_attachment(presentation_data::lookup(sockets, library.motion(id).socket), library.visual(item.visual));
        held.asset = library.load(item.visual);
        result.roles.emplace(role, std::move(held));
    }
    return result;
}
void AttachmentSet::add(anima::Scene &scene) { add_to(scene, nullptr); }
void AttachmentSet::add(GameObject owner) { add_to(owner.scene(), &owner); }
void AttachmentSet::add_to(Scene &scene, const GameObject *owner) {
    for (const auto &[role, item] : roles) {
        (void)role;
        if (item.instance || !item.asset || !item.asset->render)
            throw std::invalid_argument("Attachment set must be prepared and not already added");
    }
    // Prepare current socket placements before scene creation can grow storage.
    std::map<std::string, Mat4, std::less<>> placements;
    bool visible = true;
    if (owner) {
        const auto &body = scene.slot(owner->id());
        if (!body.value.asset)
            throw std::invalid_argument("Attachment owner requires a mesh");
        const auto &pose = body.pose ? *body.pose : body.value.asset->rest_pose();
        visible = body.value.visible;
        for (const auto &[role, item] : roles)
            placements.emplace(role, attachment_placement(pose, item.binding));
    }
    try {
        for (auto &[role, item] : roles) {
            item.instance = scene.add(item.asset->render);
            if (owner) {
                auto object = scene.object(*item.instance);
                object.set_parent(*owner, ReparentMode::keep_local);
                object.set_local_matrix(placements.at(role));
                object.renderer().set_visible(visible);
            }
        }
    } catch (...) {
        remove(scene);
        throw;
    }
}
void AttachmentSet::remove(anima::Scene &scene) {
    for (const auto &[role, item] : roles) {
        (void)role;
        if (item.instance && item.instance->owner != scene.owner_)
            throw std::invalid_argument("Attachment set belongs to another scene");
    }
    for (auto &[role, item] : roles) {
        (void)role;
        if (item.instance && scene.contains(*item.instance))
            scene.remove(*item.instance);
        item.instance.reset();
    }
}
void AttachmentSet::set_visible(Scene &scene, bool visible) const {
    // Validate every handle before publishing any visibility changes.
    for (const auto &[role, item] : roles) {
        (void)role;
        if (item.instance)
            (void)scene.instance(*item.instance);
    }
    for (const auto &[role, item] : roles) {
        (void)role;
        if (item.instance)
            scene.set_visible(*item.instance, visible);
    }
}
MotionEvaluation apply_attachment_contacts(const MotionRuntime &runtime, const anima::Pose &source,
                                           std::string_view clip, const AttachmentHandling &handling,
                                           const AttachmentVisual &visual, const AttachmentBinding &primary,
                                           const std::map<std::string, AttachmentSocket, std::less<>> &sockets,
                                           const anima::Asset *prop_asset, const anima::Pose *prop_pose,
                                           std::string_view action,
                                           const std::map<std::string, float, std::less<>> *weights) {
    using namespace anima;
    MotionEvaluation result{source, {}};
    // Resolve against the primary item frame once. A support chain must not
    // contain the primary node; asset loading verifies that ownership rule.
    const auto item = attachment_placement(source, primary);
    for (const auto &rule : handling.support_contacts) {
        if (!rule.clips.contains(clip) && !rule.actions.contains(action))
            continue;
        const auto found =
            weights ? weights->find(rule.chain) : std::map<std::string, float, std::less<>>::const_iterator{};
        const float weight = weights && found != weights->end() ? found->second : 1;
        if (!std::isfinite(weight) || weight < 0 || weight > 1)
            throw std::invalid_argument("Invalid item contact weight");
        if (weight == 0)
            continue;
        const auto &socket = presentation_data::lookup(sockets, rule.socket);
        const auto frame = item * attachment_marker(visual, prop_asset, prop_pose, rule.marker) * inverse(socket.local);
        MotionControls controls;
        controls.contacts.push_back({rule.chain, point(frame, {}), rule.pole, weight, affine_rotation(frame)});
        auto solved = runtime.evaluate(result.pose, controls);
        result.pose = std::move(solved.pose);
        result.contacts.insert(result.contacts.end(), solved.contacts.begin(), solved.contacts.end());
    }
    return result;
}
void validate_attachment_ownership(const MotionRuntime &runtime, const AttachmentLibrary &library,
                                   const AttachmentSet &attachments,
                                   const std::map<std::string, AttachmentSocket, std::less<>> &sockets,
                                   bool exclusive_primary) {
    for (const auto &[clip, metadata] : runtime.clips()) {
        (void)metadata;
        std::vector<std::string_view> carries;
        for (const auto &[role, item] : attachments.roles) {
            (void)role;
            carries.push_back(library.motion(item.item_id).layer(clip));
        }
        runtime.validate_carries(carries);
    }
    std::set<std::size_t> nodes;
    std::set<std::string> chains;
    for (const auto &[role, item] : attachments.roles) {
        if (exclusive_primary && !nodes.insert(item.binding.node).second)
            throw std::invalid_argument("Attachment roles share an exclusive primary socket");
        for (const auto &contact : library.motion(item.item_id).support_contacts) {
            if (runtime.contact_end_node(contact.chain) != presentation_data::lookup(sockets, contact.socket).node)
                throw std::invalid_argument("Contact does not end at the declared socket");
            for (const auto &previous : chains)
                if (runtime.contacts_overlap(previous, contact.chain))
                    throw std::invalid_argument("Attachment roles have overlapping contact chains");
            chains.insert(contact.chain);
            for (const auto &[other_role, other] : attachments.roles) {
                (void)other_role;
                if (runtime.contact_affects_node(contact.chain, other.binding.node))
                    throw std::invalid_argument("Attachment contact moves a primary socket");
            }
        }
        (void)role;
    }
}
std::string validate_attachment_action(const ActionRuntime &runtime, const AttachmentLibrary &library,
                                       const AttachmentSet &attachments, std::string_view action) {
    std::map<std::string, std::string, std::less<>> roles;
    for (const auto &[role, item] : attachments.roles)
        roles.emplace(role, library.motion(item.item_id).id);
    const auto handling = runtime.loadout_handling(action, roles, library.catalog.empty_handling);
    for (const auto &phase : runtime.definition(action).phases)
        for (const auto &prop : phase.props) {
            const auto found = attachments.roles.find(prop.role);
            const bool bound =
                found != attachments.roles.end() &&
                library.visual(library.item(found->second.item_id).visual).animation_tracks.contains(prop.track);
            if (prop.required && !bound)
                throw std::invalid_argument("Action requires an unavailable attachment role/track");
        }
    return handling;
}
} // namespace anima
