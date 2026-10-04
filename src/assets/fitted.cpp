#include "presentation_data.hpp"
#include "texel_hold.hpp"
#include <anima/assets/fitted.hpp>
#include <mutex>
namespace anima {
struct FittedLibrary::State {
    std::shared_ptr<const anima::Asset> body;
    std::filesystem::path directory;
    TexelRetention texel_retention;
    std::mutex mutex;
    std::map<std::filesystem::path, std::weak_ptr<const FittedAsset>> models;
    State(std::shared_ptr<const anima::Asset> b, std::filesystem::path d, TexelRetention t)
        : body(std::move(b)), directory(std::move(d)), texel_retention(t) {}
};
FittedAsset::FittedAsset(const anima::Asset &body, std::shared_ptr<const anima::Asset> fitted,
                         TexelRetention texel_retention)
    : source(std::move(fitted)), joints(source ? anima::compatible_skin(body, *source)
                                               : throw std::invalid_argument("Fitted asset requires a source")) {
    if (!source->animations.empty())
        throw std::invalid_argument("Fitted models follow the body pose and cannot own motion");
    render = anima::Mesh::compile(*source, texel_retention);
    source = anima::detail::without_texels(std::move(source), *render);
}
anima::Pose FittedAsset::pose(const anima::Pose &body) const {
    auto result = render->rest_pose();
    for (const auto &[fitted_node, body_node] : joints)
        result.world.at(fitted_node) = body.world.at(body_node);
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
std::shared_ptr<const FittedAsset> FittedLibrary::load(std::string_view id) const {
    const auto &definition = presentation_data::lookup(definitions_, id);
    const auto path = std::filesystem::weakly_canonical(state_->directory / definition.model);
    const std::scoped_lock lock(state_->mutex);
    if (const auto found = state_->models.find(path); found != state_->models.end())
        if (auto result = found->second.lock())
            return result;
    auto result = std::make_shared<FittedAsset>(*state_->body, anima::load_asset(path), state_->texel_retention);
    state_->models[path] = result;
    return result;
}
std::vector<std::shared_ptr<const anima::Mesh>> FittedLibrary::resident_assets() const {
    std::vector<std::shared_ptr<const anima::Mesh>> result;
    if (!state_)
        return result;
    const std::scoped_lock lock(state_->mutex);
    for (const auto &[key, value] : state_->models) {
        (void)key;
        if (auto asset = value.lock())
            result.push_back(asset->render);
    }
    return result;
}

FittedSet::FittedSet(GameObject owner, FittedLibrary library)
    : owner_(std::move(owner)), mesh_(owner_.renderer().mesh()), library_(std::move(library)) {
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
