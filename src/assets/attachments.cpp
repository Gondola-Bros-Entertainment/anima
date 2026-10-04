#include "mesh_limits.hpp"
#include "model_cache.hpp"
#include "presentation_data.hpp"
#include "texel_hold.hpp"
#include <anima/assets/attachments.hpp>
namespace anima {
namespace {
// The one attachment catalog version decode_attachment_catalog accepts.
constexpr int catalog_version = 4;
// The one socket document version decode_attachment_sockets accepts.
constexpr int sockets_version = 1;
// The most support contacts a handling entry may declare.
constexpr std::size_t maximum_support_contacts = 4;
AttachmentCatalog decode_catalog(std::string_view document, const std::filesystem::path &directory) {
    using namespace presentation_data;
    const auto json = presentation_data::parse(document);
    anima::detail::json_version(json, "version", catalog_version, "Unsupported attachment catalog version");
    anima::detail::json_fields(json, {"version", "units", "empty_handling", "handling", "visuals", "items"});
    if (json.at("units") != "meters")
        throw std::invalid_argument("Unsupported attachment catalog units");
    for (const auto name : {"handling", "visuals", "items"})
        if (!json.at(name).is_array() || json.at(name).empty())
            throw std::invalid_argument("Attachment catalog collections require nonempty arrays");
    AttachmentCatalog result;
    result.directory = directory;
    result.empty_handling = text(json.at("empty_handling"));
    for (const auto &entry : json.at("handling")) {
        anima::detail::json_fields(entry, {"id", "socket", "layer", "layer_overrides", "support_contacts"});
        AttachmentHandling profile;
        profile.id = text(entry.at("id"));
        profile.socket = entry.at("socket").get<std::string>();
        profile.layer = entry.at("layer").get<std::string>();
        if (!entry.at("layer_overrides").is_object())
            throw std::invalid_argument("Layer overrides must map base clips to layer clips");
        for (const auto &[name, value] : entry.at("layer_overrides").items()) {
            if (name.empty())
                throw std::invalid_argument("Empty layer override base clip");
            profile.layer_overrides.emplace(name, text(value));
        }
        const auto &contacts = entry.at("support_contacts");
        if (!contacts.is_array() || contacts.size() > maximum_support_contacts)
            throw std::invalid_argument("Handling contacts require an array of at most " +
                                        std::to_string(maximum_support_contacts) + " constraints");
        std::set<std::string> chains;
        for (const auto &value : contacts) {
            anima::detail::json_fields(value, {"chain", "socket", "marker", "pole", "clips", "actions"});
            AttachmentContact contact;
            contact.chain = text(value.at("chain"));
            contact.socket = text(value.at("socket"));
            contact.marker = text(value.at("marker"));
            contact.pole = vec3(value.at("pole"), "Support contact pole requires three numbers");
            if (contact.socket == profile.socket || !chains.insert(contact.chain).second)
                throw std::invalid_argument("Support contacts need distinct chains and a secondary socket");
            const auto &active = value.at("clips");
            if (!active.is_array())
                throw std::invalid_argument("Support contacts need explicit clip coverage");
            for (const auto &clip : active) {
                const auto name = text(clip);
                if (!contact.clips.insert(name).second)
                    throw std::invalid_argument("Unknown/duplicate support contact clip");
            }
            if (!value.at("actions").is_array())
                throw std::invalid_argument("Contact action coverage must be an array");
            for (const auto &id : value.at("actions"))
                if (!contact.actions.insert(text(id)).second)
                    throw std::invalid_argument("Duplicate contact action");
            if (contact.clips.empty() && contact.actions.empty())
                throw std::invalid_argument("Support contact has no active clips/actions");
            profile.support_contacts.push_back(std::move(contact));
        }
        insert(result.handling, std::move(profile));
    }
    if (!lookup(result.handling, result.empty_handling).socket.empty())
        throw std::invalid_argument("Empty handling must not declare an attachment socket");
    for (const auto &entry : json.at("visuals")) {
        anima::detail::json_fields(
            entry, {"id", "model", "primary_grip", "markers", "primary_node", "marker_nodes", "animation_tracks"});
        AttachmentVisual visual;
        visual.id = text(entry.at("id"));
        visual.model = relative_document_path(text(entry.at("model")), ".glb",
                                              "Attachment model must be a relative GLB inside the catalog directory");
        visual.primary_grip = matrix(entry.at("primary_grip"), true);
        if (!entry.at("markers").is_object())
            throw std::invalid_argument("Attachment markers must be named frames");
        for (const auto &[name, value] : entry.at("markers").items()) {
            if (name.empty() || name == "primary")
                throw std::invalid_argument("Invalid additional grip marker name");
            visual.markers.emplace(name, matrix(value, true));
        }
        if (const auto &node = entry.at("primary_node"); !node.is_null())
            visual.primary_node = text(node);
        for (const auto name : {"marker_nodes", "animation_tracks"}) {
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
        anima::detail::json_fields(entry, {"id", "visual", "handling"});
        AttachmentDefinition item{text(entry.at("id")), text(entry.at("visual")), text(entry.at("handling"))};
        if (lookup(result.handling, item.handling).socket.empty())
            throw std::invalid_argument("Attachment items require a handling profile with a socket");
        (void)lookup(result.visuals, item.visual);
        for (const auto &contact : lookup(result.handling, item.handling).support_contacts)
            (void)lookup(lookup(result.visuals, item.visual).markers, contact.marker);
        insert(result.items, std::move(item));
    }
    return result;
}
std::map<std::string, AttachmentSocket, std::less<>>
decode_sockets(std::string_view document, const anima::Manifest &manifest, const anima::Asset &body) {
    using namespace presentation_data;
    const auto adapter = presentation_data::parse(document);
    anima::detail::json_version(adapter, "version", sockets_version, "Unsupported attachment socket document version");
    anima::detail::json_fields(adapter, {"version", "skeleton", "bind_signature", "rest_joints", "sockets"});
    if (adapter.at("skeleton") != manifest.skeleton_id || adapter.at("bind_signature") != manifest.bind_signature)
        throw std::invalid_argument("Attachment sockets belong to a different body bind contract");
    const auto rest = anima::sample_pose(body);
    const auto &rest_joints = anima::detail::json_object(adapter, "rest_joints");
    for (const auto &[name, value] : rest_joints.items()) {
        const auto expected = matrix(value, false);
        const auto &actual = rest.world.at(anima::find_node(body, name));
        for (std::size_t i = 0; i < actual.size(); ++i)
            if (std::abs(actual[i] - expected[i]) > anima::mesh_limits::rest_pose_tolerance)
                throw std::invalid_argument("Body attachment rest frame changed: " + name);
    }
    std::map<std::string, AttachmentSocket, std::less<>> result;
    for (const auto &[name, value] : anima::detail::json_object(adapter, "sockets").items()) {
        anima::detail::json_fields(value, {"node", "local"});
        const auto bone = text(value.at("node"));
        if (name.empty() || !rest_joints.contains(bone))
            throw std::invalid_argument("Socket requires a named, checked rest joint");
        result.emplace(name, AttachmentSocket{anima::find_node(body, bone), matrix(value.at("local"), false)});
    }
    return result;
}
// Model-space rest matrix of node @p index, the product of its own and its ancestors' rest matrices, without
// sampling the other nodes. Throws std::runtime_error, as sample_pose does, for an invalid or cyclic hierarchy.
anima::Mat4 rest_world(const anima::Asset &asset, std::size_t index) {
    auto result = anima::identity();
    for (std::size_t visited = 0;; ++visited) {
        if (visited == asset.nodes.size())
            throw std::runtime_error("Cycle in asset node hierarchy");
        const auto &node = asset.nodes[index];
        result = anima::operator*(node.has_matrix ? node.rest_matrix : anima::matrix(node.rest), result);
        if (node.parent < 0)
            return result;
        index = static_cast<std::size_t>(node.parent);
        if (index >= asset.nodes.size())
            throw std::runtime_error("Invalid node parent");
    }
}
// Bottom-row tolerance that scene.hpp states for the matrices a scene accepts.
constexpr float scene_affine_tolerance = 1e-5F;
// Whether @p frame is finite and affine, as scene.hpp requires of the matrices a scene accepts.
bool scene_matrix(const anima::Mat4 &frame) {
    for (const auto value : frame)
        if (!std::isfinite(value))
            return false;
    return std::abs(frame[3]) < scene_affine_tolerance && std::abs(frame[7]) < scene_affine_tolerance &&
           std::abs(frame[11]) < scene_affine_tolerance && std::abs(frame[15] - 1) < scene_affine_tolerance;
}
// The entry for @p role in @p follows, an AttachmentFollower's map of roles. Throws std::out_of_range for a role it
// lacks.
template <class Map> auto &role_entry(Map &follows, std::string_view role) {
    const auto found = follows.find(role);
    if (found == follows.end())
        throw std::out_of_range("Unknown attachment role: " + std::string(role));
    return found->second;
}
} // namespace
AttachmentCatalog decode_attachment_catalog(std::string_view document, const std::filesystem::path &directory) {
    return presentation_data::decode_step([&] { return decode_catalog(document, directory); });
}
std::map<std::string, AttachmentSocket, std::less<>>
decode_attachment_sockets(std::string_view document, const anima::Manifest &manifest, const anima::Asset &body) {
    return presentation_data::decode_step([&] { return decode_sockets(document, manifest, body); });
}
struct AttachmentLibrary::State {
    AttachmentCatalog catalog;
    TexelRetention texel_retention;
    anima::detail::ModelCache<AttachmentAsset> models;
    State(AttachmentCatalog c, TexelRetention t) : catalog(std::move(c)), texel_retention(t) {}
};
AttachmentLibrary::AttachmentLibrary(AttachmentCatalog definition, TexelRetention texel_retention) {
    if (texel_retention != TexelRetention::keep && texel_retention != TexelRetention::until_upload)
        throw std::invalid_argument("Unknown texel retention");
    state_ = std::make_shared<State>(std::move(definition), texel_retention);
}
const AttachmentCatalog &AttachmentLibrary::catalog() const noexcept { return state_->catalog; }
const AttachmentDefinition &AttachmentLibrary::item(std::string_view id) const {
    return presentation_data::lookup(state_->catalog.items, id);
}
const AttachmentVisual &AttachmentLibrary::visual(std::string_view id) const {
    return presentation_data::lookup(state_->catalog.visuals, id);
}
const AttachmentHandling &AttachmentLibrary::handling(std::string_view item_id) const {
    return presentation_data::lookup(state_->catalog.handling,
                                     item_id.empty() ? state_->catalog.empty_handling : item(item_id).handling);
}
std::shared_ptr<const AttachmentAsset> AttachmentLibrary::load(std::string_view visual_id) const {
    const auto &definition = visual(visual_id);
    const auto path = std::filesystem::weakly_canonical(state_->catalog.directory / definition.model);
    const auto texel_retention = state_->texel_retention;
    auto result = state_->models.load(path, [&](std::shared_ptr<const anima::Mesh> resident) {
        auto source = anima::load_asset(path);
        auto render = anima::detail::resident_or_compile(*source, std::move(resident), texel_retention);
        source = anima::detail::without_texels(std::move(source), *render);
        return std::make_shared<const AttachmentAsset>(AttachmentAsset{std::move(source), std::move(render)});
    });
    // Visuals that share a file may name different nodes and clips, so each load checks its own after the shared
    // import.
    validate(*result->source, definition);
    return result;
}
std::vector<std::shared_ptr<const anima::Mesh>> AttachmentLibrary::resident_meshes() const {
    return state_->models.resident_meshes();
}
void AttachmentLibrary::validate(const anima::Asset &source, const AttachmentVisual &visual) {
    if (!source.animations.empty() && visual.animation_tracks.empty())
        throw std::invalid_argument("Animated attachment model needs declared tracks");
    // A model that lacks a node or clip its visual names, or has several, fails to load.
    try {
        if (!visual.primary_node.empty())
            (void)anima::find_node(source, visual.primary_node);
        for (const auto &[marker, node] : visual.marker_nodes) {
            (void)marker;
            (void)anima::find_node(source, node);
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
        throw std::invalid_argument("Invalid attachment track progress");
    if (track.empty())
        return asset.render->rest_pose();
    const auto found = visual.animation_tracks.find(track);
    if (found == visual.animation_tracks.end()) {
        if (required)
            throw std::invalid_argument("Attachment visual lacks required track: " + std::string(track));
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
    // The grip is in prop model space, so the node carries it by the node's motion from its rest placement, and at
    // the rest pose the grip stays where bind_attachment put it.
    const auto node = anima::find_node(asset, visual.primary_node);
    const auto primary = pose.world.at(node) * anima::inverse(rest_world(asset, node)) * visual.primary_grip;
    return {binding.node, binding.local * visual.primary_grip * anima::inverse(primary)};
}
anima::Mat4 attachment_marker(const AttachmentVisual &visual, const anima::Asset *asset, const anima::Pose *pose,
                              std::string_view name) {
    auto local = presentation_data::lookup(visual.markers, name);
    const auto found = visual.marker_nodes.find(name);
    if (found == visual.marker_nodes.end())
        return local;
    if (!asset || !pose)
        throw std::invalid_argument("Animated attachment marker requires the sampled prop pose");
    return anima::operator*(pose->world.at(anima::find_node(*asset, found->second)), local);
}
bool AttachmentInstance::replace(anima::Scene &scene, const AttachmentLibrary &library,
                                 const std::map<std::string, AttachmentSocket, std::less<>> &sockets,
                                 std::string_view id) {
    if (item_id == id)
        return false;
    // Check the old handle before adding anything, so that a failure leaves no untracked object in the scene.
    if (instance && instance->owner != scene.owner_)
        throw std::invalid_argument("Attachment instance belongs to another scene");
    AttachmentInstance next;
    next.item_id = id;
    if (!id.empty()) {
        const auto &item = library.item(id);
        next.binding = bind_attachment(presentation_data::lookup(sockets, library.handling(id).socket),
                                       library.visual(item.visual));
        next.asset = library.load(item.visual);
        next.instance = scene.add(next.asset->render);
    }
    // An old instance that no longer exists, removed directly or with its parent, is already gone.
    if (instance && scene.contains(*instance))
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
    AttachmentSet result;
    for (const auto &[role, id] : desired) {
        if (role.empty() || id.empty())
            throw std::invalid_argument("Empty attachment role/item");
        const auto &item = library.item(id);
        AttachmentInstance held;
        held.item_id = id;
        held.binding = bind_attachment(presentation_data::lookup(sockets, library.handling(id).socket),
                                       library.visual(item.visual));
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
AttachmentFollower::AttachmentFollower(GameObject owner, AttachmentSet attachments)
    : owner_(std::move(owner)), attachments_(std::move(attachments)) {
    // Allocate every entry before the items are added, so that nothing after the add can fail and leave them in the
    // scene without a follower to destroy them.
    for (const auto &[role, item] : attachments_.roles)
        follows_.emplace(role, Follow{{}, item.binding});
    attachments_.add(owner_);
    auto &target = owner_.scene();
    mesh_ = target.slot(owner_.id()).value.asset;
    for (auto &[role, follow] : follows_)
        follow.object = target.object(*attachments_.roles.at(role).instance);
}
AttachmentFollower::~AttachmentFollower() {
    for (auto &[role, follow] : follows_) {
        (void)role;
        if (follow.object.valid())
            follow.object.destroy();
    }
}
Scene &AttachmentFollower::scene() const {
    auto &target = owner_.scene();
    // Bindings name nodes of the original mesh, so another mesh would place the items on unrelated nodes.
    if (target.slot(owner_.id()).value.asset != mesh_)
        throw std::invalid_argument("Attachment follower requires its original owner mesh");
    return target;
}
GameObject AttachmentFollower::object(std::string_view role) const { return role_entry(follows_, role).object; }
void AttachmentFollower::set_binding(std::string_view role, const AttachmentBinding &binding) {
    auto &entry = role_entry(follows_, role);
    if (binding.node >= mesh_->rest_pose().world.size())
        throw std::out_of_range("Attachment binding node is outside the owner mesh");
    if (!scene_matrix(binding.local))
        throw std::invalid_argument("Attachment binding frame must be finite and affine");
    entry.binding = binding;
}
void AttachmentFollower::sync() {
    auto &target = scene();
    // Check every item before changing any.
    for (const auto &[role, follow] : follows_) {
        (void)role;
        if (follow.object.valid())
            (void)target.instance(follow.object.id());
    }
    // Moving and showing items changes no slot storage, so the owner's slot stays in place.
    const auto &body = target.slot(owner_.id());
    const bool visible = body.value.visible;
    const auto &pose = body.pose ? *body.pose : mesh_->rest_pose();
    for (auto &[role, follow] : follows_) {
        (void)role;
        if (!follow.object.valid())
            continue;
        if (visible)
            follow.object.set_local_matrix(attachment_placement(pose, follow.binding));
        target.set_visible(follow.object.id(), visible);
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
    // Resolve against the primary item frame once. A support chain must not
    // contain the primary node; asset loading verifies that ownership rule.
    const auto item = attachment_placement(source, primary);
    // One evaluation solves every contact on the result of the previous one. Evaluating each on its own would encode
    // the world-only pose the previous one returned, which cannot recover a joint below a collapsed one.
    MotionControls controls;
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
        controls.contacts.push_back({rule.chain, point(frame, {}), rule.pole, weight, affine_rotation(frame)});
    }
    return runtime.evaluate(source, controls);
}
void validate_attachment_ownership(const MotionRuntime &runtime, const AttachmentLibrary &library,
                                   const AttachmentSet &attachments,
                                   const std::map<std::string, AttachmentSocket, std::less<>> &sockets,
                                   bool exclusive_primary) {
    for (const auto &[clip, metadata] : runtime.clips()) {
        (void)metadata;
        std::vector<std::string_view> layers;
        for (const auto &[role, item] : attachments.roles) {
            (void)role;
            layers.push_back(library.handling(item.item_id).layer_for(clip));
        }
        runtime.validate_layers(layers);
    }
    std::set<std::size_t> nodes;
    std::set<std::string> chains;
    for (const auto &[role, item] : attachments.roles) {
        if (exclusive_primary && !nodes.insert(item.binding.node).second)
            throw std::invalid_argument("Attachment roles share an exclusive primary socket");
        for (const auto &contact : library.handling(item.item_id).support_contacts) {
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
void validate_attachment_action(const ActionRuntime &runtime, const AttachmentLibrary &library,
                                const AttachmentSet &attachments, std::string_view action) {
    std::map<std::string, std::string, std::less<>> roles;
    for (const auto &[role, item] : attachments.roles)
        roles.emplace(role, library.handling(item.item_id).id);
    runtime.validate_roles(action, roles);
    for (const auto &phase : runtime.definition(action).phases)
        for (const auto &prop : phase.props) {
            const auto found = attachments.roles.find(prop.role);
            const bool bound =
                found != attachments.roles.end() &&
                library.visual(library.item(found->second.item_id).visual).animation_tracks.contains(prop.track);
            if (prop.required && !bound)
                throw std::invalid_argument("Action requires an unavailable attachment role/track");
        }
}
} // namespace anima
