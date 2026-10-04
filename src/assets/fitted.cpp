#include "model_cache.hpp"
#include "presentation_data.hpp"
#include "texel_hold.hpp"
#include <algorithm>
#include <anima/assets/fitted.hpp>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
namespace anima {
namespace {
// compatible_skin's documented bound on inverse-bind and rest world matrix differences.
constexpr float skin_match_tolerance = 1e-4F;
float difference(const Mat4 &a, const Mat4 &b) {
    float d = 0;
    for (unsigned i = 0; i < 16; ++i)
        d = std::max(d, std::abs(a[i] - b[i]));
    return d;
}
std::string ancestors(const Asset &asset, std::size_t node) {
    std::string result;
    for (auto parent = asset.nodes.at(node).parent; parent >= 0; parent = asset.nodes.at(parent).parent)
        result += "/" + asset.nodes.at(parent).name;
    return result;
}
} // namespace
std::vector<FittedJoint> compatible_skin(const Asset &body, const Asset &fitted) {
    if (body.skins.size() != 1 || fitted.skins.size() != 1)
        throw std::runtime_error("Fitted skin check requires one skin in the body and in the fitted model");
    const auto &body_skin = body.skins[0];
    const auto &fitted_skin = fitted.skins[0];
    if (body_skin.joints.size() != fitted_skin.joints.size())
        throw std::runtime_error("Fitted joint count differs from the body; export the fitted model against its rig");
    std::map<std::string, std::size_t> body_names;
    std::set<std::string> fitted_names;
    for (std::size_t i = 0; i < body_skin.joints.size(); ++i)
        if (!body_names.emplace(body.nodes.at(body_skin.joints[i]).name, i).second)
            throw std::runtime_error("Duplicate body joint name");
    const auto body_pose = sample_pose(body), fitted_pose = sample_pose(fitted);
    std::vector<FittedJoint> mapping;
    for (std::size_t i = 0; i < fitted_skin.joints.size(); ++i) {
        const auto node = fitted_skin.joints[i];
        const auto &name = fitted.nodes.at(node).name;
        if (!fitted_names.insert(name).second)
            throw std::runtime_error("Duplicate fitted joint: " + name);
        const auto found = body_names.find(name);
        if (found == body_names.end())
            throw std::runtime_error("Fitted joint missing from the body: " + name);
        const auto body_node = body_skin.joints[found->second];
        if (ancestors(body, body_node) != ancestors(fitted, node))
            throw std::runtime_error("Fitted hierarchy mismatch at " + name + "; export against the body's rig");
        const auto bind_error = difference(body_skin.inverse_bind.at(found->second), fitted_skin.inverse_bind.at(i));
        if (bind_error > skin_match_tolerance)
            throw std::runtime_error("Fitted inverse-bind mismatch at " + name + " (max error " +
                                     std::to_string(bind_error) + "); export the fitted model for this body");
        if (difference(body_pose.world.at(body_node), fitted_pose.world.at(node)) > skin_match_tolerance)
            throw std::runtime_error("Fitted rest-pose mismatch at " + name + "; use the body's rest transforms");
        mapping.push_back({.fitted_node = node, .body_node = body_node});
    }
    for (const auto &primitive : fitted.primitives)
        if (primitive.skin != 0)
            throw std::runtime_error("Fitted model contains an unskinned mesh; bind it to the body's rig");
    return mapping;
}
struct FittedLibrary::State {
    std::shared_ptr<const anima::Asset> body;
    std::filesystem::path directory;
    TexelRetention texel_retention;
    anima::detail::ModelCache<FittedAsset> models;
    State(std::shared_ptr<const anima::Asset> b, std::filesystem::path d, TexelRetention t)
        : body(std::move(b)), directory(std::move(d)), texel_retention(t) {}
};
FittedAsset::FittedAsset(const anima::Asset &body, std::shared_ptr<const anima::Asset> fitted,
                         TexelRetention texel_retention)
    : FittedAsset(body, std::move(fitted), texel_retention, nullptr) {}
FittedAsset::FittedAsset(const anima::Asset &body, std::shared_ptr<const anima::Asset> fitted,
                         TexelRetention texel_retention, std::shared_ptr<const anima::Mesh> resident)
    : source(std::move(fitted)), joints(source ? anima::compatible_skin(body, *source)
                                               : throw std::invalid_argument("Fitted asset requires a source")) {
    if (!source->animations.empty())
        throw std::invalid_argument("Fitted models follow the body pose and cannot own motion");
    render = anima::detail::resident_or_compile(*source, std::move(resident), texel_retention);
    source = anima::detail::without_texels(std::move(source), *render);
}
anima::Pose FittedAsset::pose(const anima::Pose &body) const {
    auto result = render->rest_pose();
    for (const auto &joint : joints)
        result.world.at(joint.fitted_node) = body.world.at(joint.body_node);
    // Only world matrices are consumed by the renderer. Do not expose stale
    // local transforms as if they described the copied body pose.
    result.local.clear();
    return result;
}
FittedLibrary::FittedLibrary(std::shared_ptr<const anima::Asset> body, const anima::Manifest &manifest,
                             std::string_view profile, std::string_view document, TexelRetention texel_retention)
    : state_(std::make_shared<State>(std::move(body), manifest.directory, texel_retention)) {
    using namespace presentation_data;
    if (!state_->body)
        throw std::invalid_argument("Fitted library requires a body");
    if (texel_retention != TexelRetention::keep && texel_retention != TexelRetention::until_upload)
        throw std::invalid_argument("Unknown texel retention");
    const auto catalog = parse(document);
    constexpr std::size_t maximum_catalog_items = 65'536;
    // The one fitted catalog version this reader accepts.
    constexpr int catalog_version = 2;
    anima::detail::json_step([&] {
        anima::detail::json_version(catalog, "version", catalog_version, "Unsupported fitted catalog version");
        anima::detail::json_fields(catalog, {"version", "items"});
        if (!catalog.at("items").is_array() || catalog.at("items").size() > maximum_catalog_items)
            throw std::invalid_argument("Invalid fitted catalog");
        std::set<std::string> ids;
        for (const auto &item : catalog.at("items")) {
            anima::detail::json_fields(item, {"id", "fits"});
            const auto id = text(item.at("id"));
            if (!ids.insert(id).second || !item.at("fits").is_object() || item.at("fits").empty())
                throw std::invalid_argument("Invalid/duplicate fitted item");
            known_ids_.insert(id);
            for (const auto &[body_id, fit] : item.at("fits").items()) {
                if (body_id.empty())
                    throw std::invalid_argument("Empty fitted body profile");
                anima::detail::json_fields(fit, {"model", "skeleton", "bind_signature"});
                const auto path = relative_document_path(text(fit.at("model")), ".glb",
                                                         "Fitted model must be a relative .glb path without '..'");
                if (text(fit.at("skeleton")).empty() || text(fit.at("bind_signature")).empty())
                    throw std::invalid_argument("Missing fitted skeleton or bind signature");
                if (body_id != profile)
                    continue;
                if (fit.at("skeleton") != manifest.skeleton_id || fit.at("bind_signature") != manifest.bind_signature)
                    throw std::invalid_argument("Fitted model belongs to a different body bind");
                definitions_.emplace(id, FittedDefinition{id, path});
            }
        }
    });
}
const FittedDefinition &FittedLibrary::definition(std::string_view id) const {
    return presentation_data::lookup(definitions_, id);
}
std::shared_ptr<const FittedAsset> FittedLibrary::load(std::string_view id, const StagingOptions &options) const {
    const auto &definition = presentation_data::lookup(definitions_, id);
    const auto path = std::filesystem::weakly_canonical(state_->directory / definition.model);
    return state_->models.load(path, options.stop, [&](std::shared_ptr<const anima::Mesh> resident) {
        return std::make_shared<const FittedAsset>(
            FittedAsset(*state_->body, anima::load_asset(path, options), state_->texel_retention, std::move(resident)));
    });
}
std::vector<std::shared_ptr<const anima::Mesh>> FittedLibrary::resident_meshes() const {
    if (!state_)
        return {};
    return state_->models.resident_meshes();
}

FittedSet::FittedSet(ComponentOwner owner, FittedLibrary library)
    : owner_(std::move(owner.object)), mesh_(owner_.renderer().mesh()), library_(std::move(library)) {
    if (library_.state_ && !mesh_->accepts_animation_source(*library_.state_->body))
        throw std::invalid_argument("Fitted library does not match the body mesh");
}
FittedSet::~FittedSet() {
    for (auto &instance : instances_)
        if (instance.object.valid())
            instance.object.destroy();
}
Scene &FittedSet::scene() const {
    auto &scene = owner_.scene();
    if (owner_.renderer().mesh() != mesh_)
        throw std::invalid_argument("Fitted set requires its original body mesh");
    for (const auto &instance : instances_)
        if (instance.object.renderer().mesh() != instance.asset->render)
            throw std::invalid_argument("Fitted set object mesh was replaced");
    return scene;
}
void FittedSet::replace(const std::set<std::string, std::less<>> &items) {
    auto &target = scene();
    const auto &body = target.slot(owner_.id());
    // Creating objects can grow scene storage. Copy the accepted body state first.
    const auto world = body.world;
    const auto pose = body.pose ? *body.pose : mesh_->rest_pose();
    const bool visible = body.value.visible;
    std::vector<Instance> next;
    next.reserve(items.size());
    // Load and validate every fit before changing scene membership.
    for (const auto &item : items) {
        const auto found =
            std::find_if(instances_.begin(), instances_.end(), [&](const auto &entry) { return entry.item == item; });
        if (found != instances_.end())
            next.push_back(*found);
        else
            next.push_back({item, library_.load(item), {}});
    }
    std::vector<GameObject> added;
    added.reserve(next.size());
    try {
        for (auto &entry : next) {
            if (entry.object.valid())
                continue;
            entry.object = target.create(entry.item, entry.asset->render);
            added.push_back(entry.object);
            entry.object.set_parent(owner_, ReparentMode::keep_local);
            target.set_pose(entry.object.id(), entry.asset->pose(pose), world);
            entry.object.renderer().set_visible(visible);
        }
    } catch (...) {
        for (auto &object : added)
            object.destroy();
        throw;
    }
    for (auto &entry : instances_)
        if (!items.contains(entry.item))
            entry.object.destroy();
    instances_.swap(next);
}
void FittedSet::sync() {
    auto &target = scene();
    const auto &body = target.slot(owner_.id());
    const auto &pose = body.pose ? *body.pose : mesh_->rest_pose();
    for (auto &entry : instances_) {
        if (body.value.visible)
            target.set_pose(entry.object.id(), entry.asset->pose(pose), body.world);
        entry.object.renderer().set_visible(body.value.visible);
    }
}
} // namespace anima
