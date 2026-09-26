#include "presentation_data.hpp"
#include <anima/assets/fitted.hpp>
#include <mutex>
namespace anima {
static std::filesystem::path relative_model(const std::string &name) {
    const std::filesystem::path result = name;
    if (result.is_absolute() || result.extension() != ".glb" ||
        std::any_of(result.begin(), result.end(), [](const auto &component) { return component == ".."; }))
        throw std::invalid_argument("Garment model must be a relative GLB inside its catalog directory");
    return result;
}
struct FittedLibrary::State {
    std::shared_ptr<const anima::Asset> body;
    std::filesystem::path directory;
    std::mutex mutex;
    std::map<std::pair<std::filesystem::path, std::string>, std::weak_ptr<const FittedAsset>> models;
    State(std::shared_ptr<const anima::Asset> b, std::filesystem::path d)
        : body(std::move(b)), directory(std::move(d)) {}
};
FittedAsset::FittedAsset(std::string_view slot_, const anima::Asset &body, std::shared_ptr<const anima::Asset> fitted)
    : slot(slot_), source(std::move(fitted)),
      joints(source ? anima::compatible_skin(body, *source)
                    : throw std::invalid_argument("Fitted asset requires a source")) {
    if (slot.empty() || !source->animations.empty())
        throw std::invalid_argument("Fitted garments reuse the body pose and cannot own motion");
    render = anima::Mesh::compile(*source);
}
anima::Pose FittedAsset::pose(const anima::Pose &character) const {
    auto result = render->rest_pose();
    for (const auto &[gear, body] : joints)
        result.world.at(gear) = character.world.at(body);
    // Only world matrices are consumed by the renderer. Do not expose stale
    // local transforms as if they described the copied body pose.
    result.local.clear();
    return result;
}
FittedLibrary::FittedLibrary(std::shared_ptr<const anima::Asset> body, const anima::Manifest &manifest,
                             std::string_view profile, std::string_view document)
    : state_(std::make_shared<State>(std::move(body), manifest.directory)) {
    using namespace presentation_data;
    if (!state_->body)
        throw std::invalid_argument("Fitted library requires a body");
    const auto catalog = parse(document);
    anima::detail::json_step([&] {
        anima::detail::json_fields(catalog, {"version", "items"});
        if (catalog.at("version") != 1 || !catalog.at("items").is_array() || catalog.at("items").size() > 65536)
            throw std::invalid_argument("Invalid garment catalog");
        std::set<std::string> ids;
        for (const auto &item : catalog.at("items")) {
            anima::detail::json_fields(item, {"id", "slot", "fits"});
            const auto id = text(item.at("id"));
            const auto slot = text(item.at("slot"));
            if (!ids.insert(id).second || !item.at("fits").is_object() || item.at("fits").empty())
                throw std::invalid_argument("Invalid/duplicate garment definition");
            known_ids_.insert(id);
            for (const auto &[body_id, fit] : item.at("fits").items()) {
                if (body_id.empty())
                    throw std::invalid_argument("Empty garment body identity");
                anima::detail::json_fields(fit, {"model", "skeleton", "bind_signature"});
                const auto path = relative_model(text(fit.at("model")));
                if (text(fit.at("skeleton")).empty() || text(fit.at("bind_signature")).empty())
                    throw std::invalid_argument("Missing garment fit identity");
                if (body_id != profile)
                    continue;
                if (fit.at("skeleton") != manifest.skeleton_id || fit.at("bind_signature") != manifest.bind_signature)
                    throw std::invalid_argument("Garment fit belongs to a different body bind");
                definitions_.emplace(id, FittedDefinition{id, std::string(slot), path});
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
    // Slot semantics belong to the item record as well as the source mesh.
    const auto key = std::make_pair(path, definition.slot);
    if (const auto found = state_->models.find(key); found != state_->models.end())
        if (auto result = found->second.lock())
            return result;
    auto result = std::make_shared<FittedAsset>(definition.slot, *state_->body, anima::load_asset(path));
    state_->models[key] = result;
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
