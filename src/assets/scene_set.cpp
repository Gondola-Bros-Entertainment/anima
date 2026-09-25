#include <anima/scene_set.hpp>

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

class SceneSet::Mutation {
  public:
    explicit Mutation(SceneSet &owner) : owner_(owner) {
        if (owner_.mutating_)
            throw std::logic_error("Scene membership changes cannot be nested");
        for (const auto &record : owner_.scenes_)
            if (record->scene->updating_ || record->scene->constructing_)
                throw std::logic_error("Scene membership cannot change during scene callbacks");
        owner_.mutating_ = true;
    }
    ~Mutation() { owner_.mutating_ = false; }

  private:
    SceneSet &owner_;
};
SceneSet::~SceneSet() {
    mutating_ = true;
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
    const Mutation mutation(*this);
    check_key(key);
    return append(std::move(key), std::make_shared<Scene>());
}
SceneRef SceneSet::load(std::string key, std::string_view document, const MeshResolver &resolve,
                        const ComponentCodecs &codecs) {
    const Mutation mutation(*this);
    check_key(key);
    return append(std::move(key), load_scene(document, resolve, codecs));
}
SceneRef SceneSet::replace(SceneRef target, std::string_view document, const MeshResolver &resolve,
                           const ComponentCodecs &codecs) {
    const Mutation mutation(*this);
    const auto slot = index(target);
    auto old = scenes_[slot];
    auto next = std::make_shared<detail::SceneRecord>(
        detail::SceneRecord{old->key, load_scene(document, resolve, codecs), true});
    // No allocations after publication. Cleanup sees the committed membership.
    scenes_[slot] = next;
    if (active_.lock() == old)
        active_ = next;
    old->attached = false;
    old->scene->invalidate();
    old->scene->release();
    return SceneRef(next);
}
void SceneSet::unload(SceneRef scene) {
    const Mutation mutation(*this);
    const auto slot = index(scene);
    auto old = scenes_[slot];
    scenes_.erase(scenes_.begin() + static_cast<std::ptrdiff_t>(slot));
    if (active_.lock() == old)
        active_ = scenes_.empty() ? std::weak_ptr<detail::SceneRecord>{} : scenes_.front();
    old->attached = false;
    old->scene->invalidate();
    old->scene->release();
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
    const Mutation mutation(*this);
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
    const Mutation mutation(*this);
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
    const Mutation mutation(*this);
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
