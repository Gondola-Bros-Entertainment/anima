#include "../detail/json.hpp"
#include "../detail/scene_driver.hpp"
#include "../detail/scene_persistence.hpp"
#include <anima/scene_set.hpp>
#include <cstdio>
#include <exception>

namespace anima {
namespace {
constexpr std::size_t maximum_namespace_bytes = 4096;
}
bool SceneRef::valid() const noexcept {
    const auto record = record_.lock();
    return record && record->attached;
}
std::shared_ptr<detail::SceneRecord> SceneRef::lock() const {
    auto record = record_.lock();
    if (!record || !record->attached)
        throw std::out_of_range("Expired scene handle");
    return record;
}
std::string SceneRef::key() const { return lock()->key; }
Scene &SceneRef::get() const { return *lock()->scene; }
std::shared_ptr<const Scene> SceneRef::render_scene() const { return lock()->scene; }

void SceneSet::require_idle() const {
    switch (activity_) {
    case Activity::idle:
        break;
    case Activity::updating:
        throw std::logic_error("Scene set is updating");
    case Activity::changing_membership:
        throw std::logic_error("Scene set is changing membership");
    case Activity::serializing:
        throw std::logic_error("Scene set is serializing");
    case Activity::driving:
        throw std::logic_error("Scene set is held by a scene driver");
    }
    for (const auto &record : scenes_)
        if (record->scene->updating_ || record->scene->constructing_)
            throw std::logic_error("A member scene is running callbacks");
}
// Holds an idle set for one update or membership change.
class SceneSet::Mutation {
  public:
    Mutation(SceneSet &owner, Activity activity) : owner_(owner) {
        owner_.require_idle();
        owner_.activity_ = activity;
    }
    ~Mutation() { owner_.activity_ = Activity::idle; }
    Mutation(const Mutation &) = delete;
    Mutation &operator=(const Mutation &) = delete;

  private:
    SceneSet &owner_;
};
SceneSet::~SceneSet() {
    // As for Scene: the update, membership change, serialization or driver on the stack resumes after its
    // callback.
    if (activity_ != Activity::idle) {
        std::fputs("SceneSet destroyed while updating, changing membership, serializing or held by a scene driver; "
                   "destroy it after the call returns\n",
                   stderr);
        std::terminate();
    }
    activity_ = Activity::changing_membership;
    retire_all();
}
void SceneSet::check_key(std::string_view key) const {
    if (key.empty() || key.size() > maximum_namespace_bytes || key.find('\0') != std::string_view::npos)
        throw std::invalid_argument("Invalid scene namespace");
    if (find(key))
        throw std::invalid_argument("Duplicate scene namespace");
}
std::size_t SceneSet::index(SceneRef scene) const {
    const auto record = scene.lock();
    const auto found = std::find(scenes_.begin(), scenes_.end(), record);
    if (found == scenes_.end())
        throw std::invalid_argument("Foreign scene handle");
    return static_cast<std::size_t>(found - scenes_.begin());
}
SceneRef SceneSet::append(std::string key, std::shared_ptr<Scene> scene) {
    auto record = std::make_shared<detail::SceneRecord>(detail::SceneRecord{std::move(key), std::move(scene), true});
    scenes_.push_back(record);
    if (scenes_.size() == 1)
        active_ = record;
    return SceneRef(record);
}
SceneRef SceneSet::create(std::string key) {
    const Mutation mutation(*this, Activity::changing_membership);
    check_key(key);
    return append(std::move(key), std::make_shared<Scene>());
}
SceneRef SceneSet::load(std::string key, std::string_view document, const MeshResolver &resolve,
                        const ComponentCodecs &codecs, const CustomMaterialResolver &materials) {
    const Mutation mutation(*this, Activity::changing_membership);
    check_key(key);
    return append(std::move(key), load_scene(document, resolve, codecs, materials));
}
// One link that a remaining member's component reported into a scene being retired.
struct SceneSet::Link {
    // Keeps the component value that holds *target alive until the change commits.
    std::shared_ptr<detail::ComponentRecord> component;
    const std::string *codec;
    GameObject *target;
};
std::vector<SceneSet::Link> SceneSet::links_into(const Scene &retired, const ComponentCodecs &codecs) const {
    struct Attached {
        std::type_index type;
        std::shared_ptr<detail::ComponentRecord> record;
    };
    // Snapshot the attachments first: link callbacks are application code.
    std::vector<Attached> attached;
    for (const auto &member : scenes_)
        if (member->scene.get() != &retired)
            for (const auto &slot : member->scene->slots_)
                if (slot.alive)
                    for (const auto &[type, record] : slot.components)
                        if (record->attached)
                            attached.push_back({type, record});
    std::vector<Link> result;
    for (const auto &[type, record] : attached) {
        const auto collected = codecs.collect_links(type, record->value->address());
        for (auto *link : collected.links)
            if (retired.contains(link->id()))
                result.push_back({record, collected.key, link});
    }
    return result;
}
SceneRef SceneSet::replace(SceneRef target, std::string_view document, const MeshResolver &resolve,
                           const ComponentCodecs &codecs, const CustomMaterialResolver &materials) {
    const Mutation mutation(*this, Activity::changing_membership);
    const auto slot = index(target);
    auto old = scenes_[slot];
    auto next = std::make_shared<detail::SceneRecord>(detail::SceneRecord{
        old->key, detail::load_scene_member(document, *this, old->key, resolve, materials, codecs), true});
    const auto links = links_into(*old->scene, codecs);
    std::vector<GameObject> rebound;
    rebound.reserve(links.size());
    for (const auto &link : links) {
        auto object = next->scene->find(link.target->key());
        if (!object.valid())
            throw std::invalid_argument("Replacement lacks a linked object key");
        rebound.push_back(std::move(object));
    }
    // No allocations after publication. Cleanup sees the committed membership and links.
    scenes_[slot] = next;
    if (active_.lock() == old)
        active_ = next;
    for (std::size_t i = 0; i < links.size(); ++i)
        *links[i].target = rebound[i];
    old->attached = false;
    old->scene->invalidate();
    old->scene->release();
    return SceneRef(next);
}
std::string SceneSet::serialize(const MeshName &name, const ComponentCodecs &codecs) {
    const detail::SceneDriver::Scope scope(*this, Activity::serializing);
    return detail::serialize_scene_set(*this, name, codecs);
}
void SceneSet::restore(std::string_view document, const MeshResolver &resolve, const ComponentCodecs &codecs,
                       const CustomMaterialResolver &materials) {
    const Mutation mutation(*this, Activity::changing_membership);
    auto staged = detail::json_step([&] { return detail::load_scene_set(document, resolve, materials, codecs); });
    // Publication cannot allocate. The staged owner now retires the old set;
    // cleanup callbacks observe the complete committed replacement.
    scenes_.swap(staged->scenes_);
    active_.swap(staged->active_);
}
std::vector<ClearedLink> SceneSet::unload(SceneRef scene, const ComponentCodecs &codecs) {
    const Mutation mutation(*this, Activity::changing_membership);
    const auto slot = index(scene);
    auto old = scenes_[slot];
    const auto links = links_into(*old->scene, codecs);
    std::vector<ClearedLink> cleared;
    cleared.reserve(links.size());
    for (const auto &link : links)
        cleared.push_back({link.component->object, *link.codec, {old->key, link.target->key()}});
    // No allocations after removal. Cleanup sees the committed membership and links.
    scenes_.erase(scenes_.begin() + static_cast<std::ptrdiff_t>(slot));
    if (active_.lock() == old)
        active_ = scenes_.empty() ? std::weak_ptr<detail::SceneRecord>{} : scenes_.front();
    for (const auto &link : links)
        *link.target = GameObject{};
    old->attached = false;
    old->scene->invalidate();
    old->scene->release();
    return cleared;
}
void SceneSet::retire_all() noexcept {
    auto retired = std::move(scenes_);
    scenes_.clear();
    active_.reset();
    // Every scene/object handle is invalid before any component cleanup runs.
    for (const auto &record : retired) {
        record->attached = false;
        record->scene->invalidate();
    }
    for (const auto &record : retired)
        record->scene->release();
}
void SceneSet::clear() {
    const Mutation mutation(*this, Activity::changing_membership);
    retire_all();
}
std::vector<SceneRef> SceneSet::scenes() const {
    std::vector<SceneRef> result;
    result.reserve(scenes_.size());
    for (const auto &record : scenes_)
        result.push_back(SceneRef(record));
    return result;
}
std::vector<std::shared_ptr<const Scene>> SceneSet::render_scenes() const {
    std::vector<std::shared_ptr<const Scene>> result;
    result.reserve(scenes_.size());
    for (const auto &record : scenes_)
        result.push_back(record->scene);
    return result;
}
void SceneSet::run_components(double seconds, bool fixed, bool lifecycle_only) {
    const Mutation mutation(*this, Activity::updating);
    std::vector<Scene *> selected;
    selected.reserve(scenes_.size());
    for (const auto &record : scenes_)
        selected.push_back(record->scene.get());
    Scene::run_components(selected, seconds, fixed, lifecycle_only);
}
void SceneSet::update(double seconds) { run_components(seconds, false); }
void SceneSet::fixed_update(double seconds) { run_components(seconds, true); }
void SceneSet::synchronize_lifecycle() { run_components(0, false, true); }
SceneRef SceneSet::find(std::string_view key) const noexcept {
    for (const auto &record : scenes_)
        if (record->key == key)
            return SceneRef(record);
    return {};
}
SceneRef SceneSet::active() const noexcept { return SceneRef(active_.lock()); }
void SceneSet::set_active(SceneRef scene) {
    require_idle(); // Selecting runs no callback, so it need not hold the set.
    active_ = scenes_[index(scene)];
}
SceneAddress SceneSet::address(GameObject object) const {
    for (const auto &record : scenes_)
        if (record->scene->contains(object.id()))
            return {record->key, object.key()};
    throw std::invalid_argument("Object is stale or outside this scene set");
}
GameObject SceneSet::find(const SceneAddress &address) const noexcept {
    for (const auto &record : scenes_)
        if (record->key == address.scene)
            return record->scene->find(address.object);
    return {};
}
} // namespace anima
