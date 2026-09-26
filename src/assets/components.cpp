#include "../detail/json.hpp"
#include <anima/components.hpp>
#include <set>

namespace anima {
std::shared_ptr<detail::ComponentRecord> Scene::component(Id id, std::type_index type) const {
    const auto &components = slot(id).components;
    const auto found = components.find(type);
    return found == components.end() ? nullptr : found->second;
}
bool Scene::detach_component(Id id, std::type_index type) {
    auto &components = slot(id).components;
    const auto found = components.find(type);
    if (found == components.end())
        return false;
    auto record = std::move(found->second);
    record->attached = false;
    components.erase(found);
    const bool was_updating = std::exchange(updating_, true);
    record->disable();
    // Release outside container references but inside the callback boundary.
    record.reset();
    updating_ = was_updating;
    return true;
}
std::vector<std::type_index> GameObject::component_types() const {
    std::vector<std::type_index> result;
    for (const auto &[type, record] : scene().slot(id_).components)
        if (record->attached)
            result.push_back(type);
    return result;
}
void Scene::run_components(std::span<Scene *const> scenes, double seconds, bool fixed, bool lifecycle_only) {
    if (!std::isfinite(seconds) || seconds < 0)
        throw std::invalid_argument("Invalid component time step");
    for (const auto *scene : scenes)
        if (scene->updating_ || scene->constructing_ || !scene->lifetime_->scene)
            throw std::logic_error("Component updates cannot be nested");
    struct Participant {
        std::shared_ptr<detail::ComponentRecord> record;
        bool eligible;
    };
    std::vector<Participant> snapshot;
    for (const auto *scene : scenes)
        for (const auto &entry : scene->slots_)
            if (entry.alive)
                for (const auto &[type, record] : entry.components) {
                    (void)type;
                    if (record->attached)
                        snapshot.push_back({record, record->enabled && entry.active_hierarchy});
                }
    for (auto *scene : scenes)
        scene->updating_ = true;
    try {
        const auto active = [](const auto &record) {
            return record->attached && record->enabled && record->object.valid() &&
                   record->object.active_in_hierarchy();
        };
        // Disable before enabling replacements. Mutation from hooks is bounded
        // by this one snapshot; no unbounded drain or recursive scheduler entry.
        for (const auto &[record, eligible] : snapshot)
            if (!eligible || !active(record))
                record->disable();
        for (const auto &[record, eligible] : snapshot)
            if (eligible && active(record) && !record->lifecycle_active)
                record->enable();
        const auto run = [&](detail::ComponentPhase phase) {
            for (const auto &[record, eligible] : snapshot)
                if (eligible && active(record) && record->lifecycle_active)
                    record->value->update(phase, seconds);
        };
        if (!lifecycle_only) {
            run(fixed ? detail::ComponentPhase::fixed : detail::ComponentPhase::frame);
            if (!fixed)
                run(detail::ComponentPhase::late);
        }
    } catch (...) {
        snapshot.clear(); // Retire pinned values while every scene remains locked.
        for (auto *scene : scenes)
            scene->updating_ = false;
        throw;
    }
    snapshot.clear();
    for (auto *scene : scenes)
        scene->updating_ = false;
}
void Scene::update(double seconds) {
    Scene *self = this;
    run_components({&self, 1}, seconds, false);
}
void Scene::fixed_update(double seconds) {
    Scene *self = this;
    run_components({&self, 1}, seconds, true);
}
void Scene::synchronize_lifecycle() {
    Scene *self = this;
    run_components({&self, 1}, 0, false, true);
}

ObjectReferences::ObjectReferences(std::span<const GameObject> objects) {
    for (const auto &object : objects)
        add(object.key(), object);
}
ObjectReferences::ObjectReferences(std::span<const Entry> entries) {
    for (const auto &entry : entries)
        add(entry.key, entry.object);
}
void ObjectReferences::add(ObjectKey key, GameObject object) {
    if (!key.value || !object.valid() || objects_.contains(key) || keys_.contains(object.id()))
        throw std::invalid_argument("Invalid or duplicate object reference mapping");
    objects_.emplace(key, object);
    keys_.emplace(object.id(), key);
}
ObjectKey ObjectReferences::key(GameObject object) const {
    if (object.id() == Scene::Id{})
        return {};
    const auto found = keys_.find(object.id());
    if (!object.valid() || found == keys_.end())
        throw std::invalid_argument("Object reference is stale or outside the captured graph");
    return found->second;
}
GameObject ObjectReferences::resolve(ObjectKey key) const {
    if (!key.value)
        return {};
    const auto found = objects_.find(key);
    if (found == objects_.end() || !found->second.valid())
        throw std::invalid_argument("Object reference target is missing or expired");
    return found->second;
}

std::vector<ComponentData> ComponentCodecs::capture(GameObject object, const ObjectReferences &references) const {
    std::vector<ComponentData> result;
    for (const auto type : object.component_types()) {
        if (type == typeid(ObjectTransform) || type == typeid(MeshRenderer))
            continue;
        const auto found = codecs_.find(type);
        if (found == codecs_.end())
            throw std::invalid_argument("Component has no persistence codec");
        auto data = detail::json_step([&] { return found->second.encode(object, references); });
        data.type = found->second.key;
        result.push_back(std::move(data));
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.type < b.type; });
    return result;
}
void ComponentCodecs::validate(std::span<const ComponentData> data) const {
    std::set<std::string_view> seen;
    for (const auto &component : data) {
        if (!seen.insert(component.type).second)
            throw std::invalid_argument("Duplicate serialized component");
        const auto codec = std::find_if(codecs_.begin(), codecs_.end(),
                                        [&](const auto &entry) { return entry.second.key == component.type; });
        if (codec == codecs_.end())
            throw std::invalid_argument("Unknown serialized component type");
    }
}
void ComponentCodecs::restore(GameObject object, std::span<const ComponentData> data,
                              const ObjectReferences &references) const {
    validate(data);
    for (const auto &component : data) {
        const auto codec = std::find_if(codecs_.begin(), codecs_.end(),
                                        [&](const auto &entry) { return entry.second.key == component.type; });
        detail::json_step([&] { codec->second.decode(object, component, references); });
    }
}
} // namespace anima
