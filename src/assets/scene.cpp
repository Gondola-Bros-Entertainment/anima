#include "mesh_limits.hpp"
#include "winding.hpp"
#include <algorithm>
#include <anima/assets/scene_validation.hpp>
#include <anima/scene.hpp>
#include <atomic>
#include <bit>
#include <charconv>
#include <climits>
#include <cstdio>
#include <exception>
#include <functional>
#include <limits>
#include <set>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace anima {
namespace {
constexpr float affine_tolerance = 1e-5F;
constexpr std::size_t maximum_object_key_digits = std::numeric_limits<std::uint64_t>::digits10 + 1;
void require(bool value, const char *message) {
    if (!value)
        throw std::invalid_argument(message);
}
bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
void affine(const Mat4 &m) {
    for (auto v : m)
        require(std::isfinite(v), "Non-finite instance transform");
    require(std::abs(m[3]) < affine_tolerance && std::abs(m[7]) < affine_tolerance &&
                std::abs(m[11]) < affine_tolerance && std::abs(m[15] - 1) < affine_tolerance,
            "Instance transform must be affine");
}
void expand(RenderBounds &bounds, Vec3 v) {
    require(finite(v), "Non-finite render bounds");
    if (!bounds.valid) {
        bounds = {v, v, true};
        return;
    }
    bounds.minimum = {std::min(bounds.minimum.x, v.x), std::min(bounds.minimum.y, v.y),
                      std::min(bounds.minimum.z, v.z)};
    bounds.maximum = {std::max(bounds.maximum.x, v.x), std::max(bounds.maximum.y, v.y),
                      std::max(bounds.maximum.z, v.z)};
}
using Key = std::array<std::uint32_t, 24>;
Key key(const SourceVertex &v) {
    const auto b = [](float f) { return std::bit_cast<std::uint32_t>(f); };
    return {b(v.position.x), b(v.position.y), b(v.position.z), b(v.normal.x),   b(v.normal.y),   b(v.normal.z),
            b(v.color.x),    b(v.color.y),    b(v.color.z),    b(v.uv[0]),      b(v.uv[1]),      v.joints[0],
            v.joints[1],     v.joints[2],     v.joints[3],     b(v.weights[0]), b(v.weights[1]), b(v.weights[2]),
            b(v.weights[3]), b(v.tangent[0]), b(v.tangent[1]), b(v.tangent[2]), b(v.tangent[3]), b(v.alpha)};
}
struct Hash {
    std::size_t operator()(const Key &k) const noexcept {
        // FNV-1a mixing over each word in the bit-exact vertex key.
        constexpr std::size_t offset_basis = 14695981039346656037ULL, prime = 1099511628211ULL;
        std::size_t h = offset_basis;
        for (auto v : k) {
            h ^= v;
            h *= prime;
        }
        return h;
    }
};
} // namespace

std::shared_ptr<const Mesh> Mesh::compile(const Asset &source) {
    auto result = std::shared_ptr<Mesh>(new Mesh);
    result->rest_ = sample_pose(source);
    for (const auto &node : source.nodes)
        result->nodes_.emplace_back(node.name, node.parent);
    for (const auto &m : result->rest_.world)
        affine(m);
    auto materials = std::make_shared<MeshSnapshot>();
    materials->material_data = source.materials;
    materials->textures = source.textures;
    materials->mesh_nodes = source.mesh_nodes;
    materials->skins = source.skins.size();
    materials->materials = source.materials.size();
    materials->notices = source.notices;
    for (const auto &clip : source.animations)
        materials->clips.push_back(clip.name);
    validate_scene(*materials);
    result->skins_ = source.skins;
    std::size_t palette_size = source.nodes.size();
    std::vector<std::uint32_t> offsets;
    for (const auto &skin : source.skins) {
        require(!skin.joints.empty() && skin.joints.size() <= mesh_limits::maximum_skin_joints &&
                    skin.joints.size() == skin.inverse_bind.size(),
                "Invalid render skin palette");
        require(palette_size <= UINT32_MAX - skin.joints.size(), "Render palette index overflow");
        offsets.push_back(static_cast<std::uint32_t>(palette_size));
        palette_size += skin.joints.size();
        materials->joints += skin.joints.size();
        std::set<std::size_t> seen;
        for (std::size_t j = 0; j < skin.joints.size(); ++j) {
            require(skin.joints[j] < source.nodes.size() && seen.insert(skin.joints[j]).second,
                    "Invalid render skin joint");
            affine(skin.inverse_bind[j]);
            const auto bind = result->rest_.world[skin.joints[j]] * skin.inverse_bind[j], unit = identity();
            affine(bind);
            for (unsigned k = 0; k < 16; ++k)
                materials->bind_deviation = std::max(materials->bind_deviation, std::abs(bind[k] - unit[k]));
        }
    }
    materials->default_is_bind_pose = materials->bind_deviation < mesh_limits::bind_pose_tolerance;
    require(palette_size <= UINT32_MAX, "Render palette index overflow");
    result->palette_size_ = palette_size;
    for (const auto &primitive : source.primitives) {
        require(primitive.node < source.nodes.size(), "Invalid render primitive node");
        require(primitive.skin >= -1 && (primitive.skin < 0 || std::size_t(primitive.skin) < source.skins.size()),
                "Invalid render primitive skin");
        require(primitive.material >= -1 &&
                    (primitive.material < 0 || std::size_t(primitive.material) < source.materials.size()),
                "Invalid render primitive material");
        require(!primitive.vertices.empty() && primitive.vertices.size() % 3 == 0 &&
                    primitive.vertices.size() <= UINT32_MAX - result->indices_.size(),
                "Invalid render triangle count");
        require(primitive.vertices.size() <=
                    std::numeric_limits<std::size_t>::max() / sizeof(SourceVertex) - result->vertices_.size(),
                "Render vertex byte size overflow");
        const auto offset = primitive.skin < 0 ? static_cast<std::uint32_t>(primitive.node) : offsets[primitive.skin];
        result->draws_.push_back({static_cast<std::uint32_t>(result->indices_.size()),
                                  static_cast<std::uint32_t>(primitive.vertices.size()), offset, primitive.skin >= 0,
                                  primitive.material, static_cast<std::uint32_t>(primitive.node),
                                  source.nodes[primitive.node].name, primitive.mesh_name});
        if (primitive.skin >= 0)
            materials->skinned_vertices += primitive.vertices.size();
        const auto joint_count = primitive.skin < 0 ? 1 : source.skins[primitive.skin].joints.size();
        std::vector<RenderBounds> bounds(joint_count);
        std::unordered_map<Key, std::uint32_t, Hash> unique;
        unique.reserve(primitive.vertices.size() / 2);
        for (const auto &v : primitive.vertices) {
            require(finite(v.position) && finite(v.normal) && finite(v.color) && std::isfinite(v.uv[0]) &&
                        std::isfinite(v.uv[1]) && std::isfinite(v.alpha) && v.alpha >= 0 && v.alpha <= 1 &&
                        std::all_of(v.tangent.begin(), v.tangent.end(), [](float x) { return std::isfinite(x); }) &&
                        (v.tangent[3] == 0 || v.tangent[3] == 1 || v.tangent[3] == -1),
                    "Invalid render vertex");
            if (primitive.skin >= 0) {
                float sum = 0;
                for (unsigned j = 0; j < 4; ++j) {
                    require(std::isfinite(v.weights[j]) && v.weights[j] >= 0 && v.joints[j] < joint_count,
                            "Invalid render skin influence");
                    sum += v.weights[j];
                    if (v.weights[j] > 0)
                        expand(bounds[v.joints[j]], v.position);
                }
                require(std::abs(sum - 1) < mesh_limits::skin_weight_tolerance,
                        "Render skin weights must be normalized");
            } else
                expand(bounds[0], v.position);
            auto [found, inserted] = unique.emplace(key(v), static_cast<std::uint32_t>(result->vertices_.size()));
            if (inserted)
                result->vertices_.push_back(v);
            result->indices_.push_back(found->second);
        }
        auto &parts = result->bounds_.emplace_back();
        for (std::size_t j = 0; j < bounds.size(); ++j)
            if (bounds[j].valid)
                parts.push_back({offset + static_cast<std::uint32_t>(j), bounds[j]});
    }
    result->materials_ = std::move(materials);
    return result;
}

std::string ObjectKey::string() const { return std::to_string(value); }
ObjectKey ObjectKey::parse(std::string_view text) {
    ObjectKey key;
    if (text.empty() || text.size() > maximum_object_key_digits || (text.size() > 1 && text.front() == '0'))
        throw std::invalid_argument("Invalid object key");
    const auto result = std::from_chars(text.data(), text.data() + text.size(), key.value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::invalid_argument("Invalid object key");
    return key;
}
Scene::Scene() : lifetime_(std::make_shared<detail::SceneLifetime>()) {
    static std::atomic<std::uint64_t> next{1};
    owner_ = next.fetch_add(1);
    if (!owner_)
        std::terminate();
    lifetime_->scene = this;
}
Scene::~Scene() {
    // The hook, constructor or driver on the stack resumes using this scene after its callback
    // returns, so destroying it here would let that caller write freed memory.
    if (updating_ || constructing_) {
        std::fputs("Scene destroyed while running component hooks, constructing a component or held by a "
                   "scene driver; destroy it after the call returns\n",
                   stderr);
        std::terminate();
    }
    invalidate();
    release();
}
void Scene::invalidate() noexcept {
    lifetime_->scene = nullptr;
    keys_.clear();
    active_.clear();
    removed_instances_ = 0;
    object_count_ = 0;
    // The slots still own every record, so dropping the lists destroys no component.
    component_types_.clear();
    scheduled_ = {};
    for (auto &entry : slots_)
        for (auto &[type, record] : entry.components) {
            (void)type;
            record->attached = false;
        }
}
void Scene::release() noexcept {
    // All identities are already invalid. Release native storage before callbacks.
    std::shared_ptr<detail::ComponentRecord> retired;
    for (auto &entry : slots_) {
        for (auto &[type, record] : entry.components) {
            (void)type;
            record->retired_next = std::move(retired);
            retired = std::move(record);
        }
        entry.components.clear();
    }
    slots_.clear();
    free_slots_.clear();
    while (retired) {
        auto next = std::move(retired->retired_next);
        retired->disable();
        retired = std::move(next);
    }
}
bool Scene::contains(Id id) const noexcept {
    return lifetime_->scene && id.owner == owner_ && id.slot < slots_.size() && slots_[id.slot].alive &&
           slots_[id.slot].generation == id.generation;
}
Scene::Slot &Scene::slot(Id id) {
    if (!contains(id))
        throw std::out_of_range("Stale or foreign GameObject handle");
    return slots_[id.slot];
}
const Scene::Slot &Scene::slot(Id id) const { return const_cast<Scene *>(this)->slot(id); }
Scene::Instance &Scene::get(Id id) {
    auto &value = slot(id).value;
    if (!value.asset)
        throw std::logic_error("GameObject has no MeshRenderer");
    return value;
}
const Scene::Instance &Scene::instance(Id id) const { return const_cast<Scene *>(this)->get(id); }
void Scene::pose(Instance &value, const Pose &pose, const Mat4 &world) {
    std::vector<Mat4> palette;
    std::vector<RenderBounds> bounds;
    palette.reserve(value.asset->palette_size_);
    bounds.reserve(value.asset->draws_.size());
    const auto combined = append_pose(*value.asset, pose, world, palette, bounds);
    // All validation and allocations completed before publishing pose and bounds.
    value.palette.swap(palette);
    value.primitive_bounds.swap(bounds);
    value.bounds = combined;
}
RenderBounds Scene::append_pose(const Mesh &asset, const Pose &pose, const Mat4 &world, std::vector<Mat4> &palette,
                                std::vector<RenderBounds> &bounds) {
    require(pose.world.size() == asset.rest_.world.size(), "Pose does not match render asset");
    affine(world);
    const auto first_matrix = palette.size(), first_bound = bounds.size();
    for (const auto &node : pose.world) {
        affine(node);
        palette.push_back(world * node);
        affine(palette.back());
    }
    for (const auto &skin : asset.skins_)
        for (std::size_t j = 0; j < skin.joints.size(); ++j) {
            palette.push_back(world * pose.world[skin.joints[j]] * skin.inverse_bind[j]);
            affine(palette.back());
        }
    bounds.resize(first_bound + asset.draws_.size());
    const auto posed = std::span(bounds).subspan(first_bound);
    for (std::size_t i = 0; i < posed.size(); ++i)
        for (const auto &part : asset.bounds_[i])
            for (unsigned corner = 0; corner < 8; ++corner) {
                const auto &b = part.bound;
                expand(posed[i], point(palette[first_matrix + part.palette],
                                       {corner & 1 ? b.maximum.x : b.minimum.x, corner & 2 ? b.maximum.y : b.minimum.y,
                                        corner & 4 ? b.maximum.z : b.minimum.z}));
            }
    for (auto &bound : posed)
        if (bound.valid) {
            // Pad for the accepted weight error and an equal rounding margin in
            // the convex influence union at the GPU boundary.
            const auto pad = [](float lo, float hi) {
                return std::max({1.F, std::abs(lo), std::abs(hi)}) * (2 * mesh_limits::skin_weight_tolerance);
            };
            const Vec3 margin{pad(bound.minimum.x, bound.maximum.x), pad(bound.minimum.y, bound.maximum.y),
                              pad(bound.minimum.z, bound.maximum.z)};
            const auto lo = bound.minimum - margin, hi = bound.maximum + margin;
            expand(bound, lo);
            expand(bound, hi);
        }
    RenderBounds combined;
    for (const auto &bound : posed)
        if (bound.valid) {
            expand(combined, bound.minimum);
            expand(combined, bound.maximum);
        }
    return combined;
}
GameObject Scene::create(std::string name, std::shared_ptr<const Mesh> mesh) {
    if (!next_key_)
        throw std::overflow_error("Scene object keys exhausted");
    const ObjectKey key{next_key_++}; // Failure consumes an identity, never recycles it.
    return create_with_key(key, std::move(name), std::move(mesh));
}
GameObject Scene::create_with_key(ObjectKey key, std::string name, std::shared_ptr<const Mesh> mesh) {
    if (!lifetime_->scene)
        throw std::logic_error("Cannot create objects during scene teardown");
    require(key.value && !keys_.contains(key.value), "Duplicate or null scene object key");
    std::size_t index = slots_.size();
    if (free_slots_.empty()) {
        slots_.emplace_back();
        try {
            // Grows only when slots_ reallocates, so reserving stays amortized constant time.
            free_slots_.reserve(slots_.capacity());
        } catch (...) {
            slots_.pop_back();
            throw;
        }
    } else {
        // The lowest free slot, which a scan from the first slot would find.
        std::pop_heap(free_slots_.begin(), free_slots_.end(), std::greater<>{});
        index = free_slots_.back();
        free_slots_.pop_back();
    }
    auto &entry = slots_[index];
    entry.name = std::move(name);
    entry.key = key;
    entry.alive = true;
    ++object_count_;
    const Id id{owner_, entry.generation, index};
    try {
        keys_.emplace(key.value, id);
        auto transform = std::make_shared<detail::ComponentRecord>(object(id), typeid(ObjectTransform), false);
        transform->value =
            std::make_unique<detail::ComponentBox<ObjectTransform>>(object(id), ObjectTransform(object(id)));
        entry.components.emplace(typeid(ObjectTransform), transform);
        index_component(transform);
        if (mesh)
            assign_mesh(id, std::move(mesh));
    } catch (...) {
        remove(id);
        throw;
    }
    return object(id);
}
GameObject Scene::object(Id id) {
    (void)slot(id);
    return GameObject(lifetime_, id);
}
GameObject Scene::find(ObjectKey key) const noexcept {
    const auto found = keys_.find(key.value);
    return found != keys_.end() && contains(found->second) ? GameObject(lifetime_, found->second) : GameObject{};
}
std::vector<GameObject> Scene::roots() {
    std::vector<GameObject> result;
    for (std::size_t i = 0; i < slots_.size(); ++i)
        if (slots_[i].alive && !slots_[i].parent)
            result.push_back(object({owner_, slots_[i].generation, i}));
    return result;
}
Scene::Id Scene::add(std::shared_ptr<const Mesh> asset) {
    require(bool(asset), "Null mesh");
    return create({}, std::move(asset)).id();
}
void Scene::assign_mesh(Id id, std::shared_ptr<const Mesh> mesh, const Pose *initial_pose) {
    auto &entry = slot(id);
    require(mesh || !initial_pose, "An initial pose requires a mesh");
    if (!mesh) {
        if (entry.value.asset)
            retire_instance(entry);
        entry.value = {};
        entry.pose.reset();
        detach_component(id, typeid(MeshRenderer));
        return;
    }
    Instance next;
    next.asset = std::move(mesh);
    std::optional<Pose> next_pose;
    if (initial_pose)
        next_pose = *initial_pose;
    for (const auto &material : next.asset->materials_->material_data)
        next.factors.push_back(material.factor);
    next.custom_materials.resize(next.factors.size());
    next.primitive_visible.resize(next.asset->draws_.size(), true);
    pose(next, next_pose ? *next_pose : next.asset->rest_, entry.world);
    // Allocate before publishing. Replacement keeps the object's transform, but
    // resets overrides and uses either the authored initial pose or mesh defaults.
    if (!entry.value.asset) {
        auto renderer = std::make_shared<detail::ComponentRecord>(object(id), typeid(MeshRenderer), false);
        renderer->value = std::make_unique<detail::ComponentBox<MeshRenderer>>(object(id), MeshRenderer(object(id)));
        entry.components.emplace(typeid(MeshRenderer), renderer);
        try {
            index_component(renderer);
            active_.push_back(id);
        } catch (...) {
            detach_component(id, typeid(MeshRenderer));
            throw;
        }
        entry.instance = active_.size() - 1;
    }
    entry.value = std::move(next);
    entry.value.active = entry.active_hierarchy;
    component(id, typeid(MeshRenderer))->enabled = true;
    entry.pose = std::move(next_pose);
}
void Scene::link_child(std::size_t parent, std::size_t child) noexcept {
    auto &owner = slots_[parent];
    auto &entry = slots_[child];
    entry.previous_sibling = owner.last_child;
    entry.next_sibling = no_slot;
    if (owner.last_child == no_slot)
        owner.first_child = child;
    else
        slots_[owner.last_child].next_sibling = child;
    owner.last_child = child;
}
void Scene::unlink_child(std::size_t child) noexcept {
    auto &entry = slots_[child];
    auto &owner = slots_[entry.parent->slot];
    if (entry.previous_sibling == no_slot)
        owner.first_child = entry.next_sibling;
    else
        slots_[entry.previous_sibling].next_sibling = entry.next_sibling;
    if (entry.next_sibling == no_slot)
        owner.last_child = entry.previous_sibling;
    else
        slots_[entry.next_sibling].previous_sibling = entry.previous_sibling;
    entry.previous_sibling = entry.next_sibling = no_slot;
}
void Scene::retire_instance(Slot &entry) noexcept {
    active_[entry.instance] = Id{};
    entry.instance = no_slot;
    // Compacting once half the entries are null keeps removal amortized constant time and the
    // list at most twice the renderer count, even when instances() is never called.
    if (++removed_instances_ > active_.size() / 2)
        compact_instances();
}
void Scene::compact_instances() const noexcept {
    // Keeps the order renderers were added, as erasing each one in place did.
    std::size_t kept = 0;
    for (const auto id : active_)
        if (id != Id{}) {
            slots_[id.slot].instance = kept;
            active_[kept++] = id;
        }
    active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(kept), active_.end());
    removed_instances_ = 0;
}
std::span<const Scene::Id> Scene::instances() const {
    if (removed_instances_)
        compact_instances();
    return active_;
}
void Scene::release_slot(std::size_t index) noexcept {
    free_slots_.push_back(index); // Within the capacity reserved when the slot was created.
    std::push_heap(free_slots_.begin(), free_slots_.end(), std::greater<>{});
}
void Scene::remove(Id id) {
    auto &root = slot(id);
    if (root.parent)
        unlink_child(id.slot);
    root.parent.reset();
    std::shared_ptr<detail::ComponentRecord> retired;
    // Leaf-first traversal uses existing links, with no allocation or recursion. Each object is
    // its parent's last child when it is reached, so unlinking it takes constant time.
    auto current = id.slot;
    for (;;) {
        auto &entry = slots_[current];
        if (entry.last_child != no_slot) {
            current = entry.last_child;
            continue;
        }
        const auto parent = entry.parent;
        if (parent)
            unlink_child(current);
        if (entry.value.asset)
            retire_instance(entry);
        entry.value = {};
        entry.name.clear();
        keys_.erase(entry.key.value);
        entry.key = {};
        entry.world = entry.local = identity();
        entry.parent.reset();
        entry.pose.reset();
        // Retire values without allocating. Destructors run after the whole
        // subtree is removed, so they cannot observe partially removed objects.
        for (auto &[type, component] : entry.components) {
            (void)type;
            if (component->attached)
                unindex_component(*component);
            component->attached = false;
            component->retired_next = std::move(retired);
            retired = std::move(component);
        }
        entry.components.clear();
        entry.alive = false;
        entry.active_self = entry.active_hierarchy = true;
        --object_count_;
        if (entry.generation != UINT64_MAX)
            ++entry.generation;
        // A slot whose generation is exhausted is never reused, so no stale Id can match it.
        if (entry.generation != UINT64_MAX)
            release_slot(current);
        if (!parent)
            break;
        current = parent->slot;
    }
    const bool was_updating = std::exchange(updating_, true);
    while (retired) {
        auto next = std::move(retired->retired_next);
        retired->disable();
        retired.reset();
        retired = std::move(next);
    }
    updating_ = was_updating;
}
void Scene::set_pose(Id id, const Pose &value, const Mat4 &world) {
    auto &entry = slot(id);
    (void)get(id);
    // A stored pose with room for this one takes the copy in place. Otherwise the copy is made
    // first, so a failed allocation publishes nothing.
    const bool fits = entry.pose && entry.pose->local.capacity() >= value.local.size() &&
                      entry.pose->world.capacity() >= value.world.size();
    std::optional<Pose> copy;
    if (!fits)
        copy.emplace(value);
    update_transform(id, to_local(id, world), world, &value);
    if (copy)
        entry.pose = std::move(copy);
    else {
        // Within capacity, copying these trivially copyable elements neither allocates nor throws.
        static_assert(std::is_trivially_copyable_v<Transform> && std::is_trivially_copyable_v<Mat4>);
        entry.pose->local = value.local;
        entry.pose->world = value.world;
    }
}
void Scene::set_transform(Id id, const Mat4 &world) { update_transform(id, to_local(id, world), world); }
Mat4 Scene::to_local(Id id, const Mat4 &world) const {
    const auto &entry = slot(id);
    if (world == entry.world)
        return entry.local;
    return entry.parent ? inverse(slot(*entry.parent).world) * world : world;
}
void Scene::set_local_transform(Id id, const Mat4 &local) {
    const auto &entry = slot(id);
    update_transform(id, local, entry.parent ? slot(*entry.parent).world * local : local);
}
void Scene::update_transform(Id id, const Mat4 &local, const Mat4 &world, const Pose *replacement) {
    auto &entry = slot(id);
    affine(local);
    affine(world);
    // Most animated objects are leaves or keep their placement between samples, so only a moved
    // object with children re-poses more than itself.
    const bool moved = world != entry.world, descendants = moved && entry.first_child != no_slot;
    if (descendants || (entry.value.asset && (replacement || moved))) {
        // Validate every affected palette and bound in the working lists before publishing any.
        posed_objects_.clear();
        posed_palettes_.clear();
        posed_bounds_.clear();
        posed_objects_.push_back({id, world, {}, {}, {}});
        for (std::size_t i = 0; i < posed_objects_.size(); ++i) {
            const auto current = posed_objects_[i].id;
            const auto placed = posed_objects_[i].world;
            const auto &source = slots_[current.slot];
            affine(placed);
            if (source.value.asset) {
                posed_objects_[i].palette = posed_palettes_.size();
                posed_objects_[i].bounds = posed_bounds_.size();
                posed_objects_[i].combined = append_pose(*source.value.asset,
                                                         current == id && replacement ? *replacement
                                                         : source.pose                ? *source.pose
                                                                                      : source.value.asset->rest_,
                                                         placed, posed_palettes_, posed_bounds_);
            }
            if (descendants)
                for (auto child = source.first_child; child != no_slot; child = slots_[child].next_sibling)
                    posed_objects_.push_back({id_at(child), placed * slots_[child].local, {}, {}, {}});
        }
        // A renderer keeps its mesh, so its lists keep their sizes and copying cannot allocate.
        for (const auto &posed : posed_objects_) {
            auto &target = slots_[posed.id.slot];
            target.world = posed.world;
            auto &value = target.value;
            if (!value.asset)
                continue;
            std::copy_n(posed_palettes_.begin() + static_cast<std::ptrdiff_t>(posed.palette), value.palette.size(),
                        value.palette.begin());
            std::copy_n(posed_bounds_.begin() + static_cast<std::ptrdiff_t>(posed.bounds),
                        value.primitive_bounds.size(), value.primitive_bounds.begin());
            value.bounds = posed.combined;
        }
    }
    entry.local = local;
    entry.world = world;
}
void Scene::reparent(Id id, std::optional<Id> parent, ReparentMode mode) {
    auto &entry = slot(id);
    require(mode == ReparentMode::keep_world || mode == ReparentMode::keep_local, "Invalid reparent mode");
    constexpr auto cycle = "GameObject parenting would create a cycle";
    require(parent != id, cycle);
    if (entry.parent == parent)
        return;
    // Only a parent inside the moved subtree closes a cycle, so a leaf needs no search, and a
    // subtree is searched once instead of walking every ancestor of the new parent.
    const bool leaf = entry.first_child == no_slot;
    std::vector<Id> affected;
    if (!leaf) {
        affected = subtree(id);
        require(!parent || std::find(affected.begin(), affected.end(), *parent) == affected.end(), cycle);
    }
    const auto parent_world = parent ? slot(*parent).world : identity();
    const auto local =
        mode == ReparentMode::keep_world ? (parent ? inverse(parent_world) * entry.world : entry.world) : entry.local;
    const auto world = mode == ReparentMode::keep_world ? entry.world : parent_world * local;
    update_transform(id, local, world);
    // Relinking cannot fail, so the accepted transform and the new links are published together.
    if (entry.parent)
        unlink_child(id.slot);
    entry.parent = parent;
    if (parent)
        link_child(parent->slot, id.slot);
    refresh_activation(leaf ? std::span<const Id>(&id, 1) : std::span<const Id>(affected));
}
std::vector<Scene::Id> Scene::subtree(Id id) const {
    std::vector<Id> result{id};
    for (std::size_t i = 0; i < result.size(); ++i)
        for (auto child = slots_[result[i].slot].first_child; child != no_slot; child = slots_[child].next_sibling)
            result.push_back(id_at(child));
    return result;
}
void Scene::refresh_activation(std::span<const Id> objects) noexcept {
    for (const auto id : objects) {
        auto &entry = slots_[id.slot];
        entry.active_hierarchy = entry.active_self && (!entry.parent || slots_[entry.parent->slot].active_hierarchy);
        entry.value.active = entry.active_hierarchy;
    }
}
Scene &GameObject::scene() const {
    const auto lifetime = lifetime_.lock();
    if (!lifetime || !lifetime->scene || !lifetime->scene->contains(id_))
        throw std::out_of_range("Expired GameObject handle");
    return *lifetime->scene;
}
bool GameObject::valid() const noexcept {
    const auto lifetime = lifetime_.lock();
    return lifetime && lifetime->scene && lifetime->scene->contains(id_);
}
std::string GameObject::name() const { return scene().slot(id_).name; }
ObjectKey GameObject::key() const { return scene().slot(id_).key; }
void GameObject::set_name(std::string name) { scene().slot(id_).name = std::move(name); }
bool GameObject::active_self() const { return scene().slot(id_).active_self; }
bool GameObject::active_in_hierarchy() const { return scene().slot(id_).active_hierarchy; }
void GameObject::set_active(bool active) {
    auto &owner = scene();
    auto &entry = owner.slot(id_);
    if (entry.active_self == active)
        return;
    const auto affected = owner.subtree(id_);
    entry.active_self = active;
    owner.refresh_activation(affected);
}
ObjectTransform GameObject::transform() const {
    (void)scene();
    return ObjectTransform(*this);
}
Mat4 GameObject::world_matrix() const { return scene().slot(id_).world; }
Mat4 GameObject::local_matrix() const { return scene().slot(id_).local; }
Vec3 GameObject::local_position() const {
    const auto local = local_matrix();
    return translation_of(local);
}
void GameObject::set_local_position(Vec3 position) {
    auto local = local_matrix();
    set_translation(local, position);
    set_local_matrix(local);
}
void GameObject::set_local_transform(const Transform &transform) { set_local_matrix(matrix(transform)); }
void GameObject::set_local_matrix(const Mat4 &local) { scene().set_local_transform(id_, local); }
std::optional<GameObject> GameObject::parent() const {
    auto &owner = scene();
    const auto parent = owner.slot(id_).parent;
    return parent ? std::optional(owner.object(*parent)) : std::nullopt;
}
std::vector<GameObject> GameObject::children() const {
    auto &owner = scene();
    std::vector<GameObject> result;
    for (auto child = owner.slot(id_).first_child; child != Scene::no_slot; child = owner.slots_[child].next_sibling)
        result.push_back(GameObject(owner.lifetime_, owner.id_at(child)));
    return result;
}
void GameObject::set_parent(GameObject parent, ReparentMode mode) {
    auto &owner = scene();
    require(&parent.scene() == &owner, "GameObject parent belongs to another scene");
    owner.reparent(id_, parent.id(), mode);
}
void GameObject::clear_parent(ReparentMode mode) { scene().reparent(id_, {}, mode); }
Vec3 GameObject::position() const {
    const auto world = world_matrix();
    return translation_of(world);
}
void GameObject::set_position(Vec3 position) {
    auto world = world_matrix();
    set_translation(world, position);
    set_world_matrix(world);
}
void GameObject::set_transform(const Transform &transform) { set_world_matrix(matrix(transform)); }
void GameObject::set_world_matrix(const Mat4 &world) { scene().set_transform(id_, world); }
bool GameObject::has_renderer() const { return bool(scene().slot(id_).value.asset); }
MeshRenderer GameObject::add_mesh(std::shared_ptr<const Mesh> mesh) {
    require(bool(mesh), "Null mesh");
    if (has_renderer())
        throw std::logic_error("GameObject already has a MeshRenderer");
    scene().assign_mesh(id_, std::move(mesh));
    return MeshRenderer(*this);
}
MeshRenderer GameObject::renderer() const {
    (void)scene().instance(id_);
    return MeshRenderer(*this);
}
void GameObject::remove_mesh() { scene().assign_mesh(id_, {}); }
void GameObject::destroy() { scene().remove(id_); }
std::shared_ptr<const Mesh> MeshRenderer::mesh() const { return object_.scene().instance(object_.id_).asset; }
void MeshRenderer::set_mesh(std::shared_ptr<const Mesh> mesh) {
    require(bool(mesh), "Null mesh");
    (void)object_.scene().instance(object_.id_);
    object_.scene().assign_mesh(object_.id_, std::move(mesh));
}
void MeshRenderer::set_pose(const Pose &pose) { object_.scene().set_pose(object_.id_, pose, object_.world_matrix()); }
void MeshRenderer::set_visible(bool visible) { object_.scene().set_visible(object_.id_, visible); }
void MeshRenderer::set_material_factor(std::size_t material, Vec3 factor) {
    object_.scene().set_material_factor(object_.id_, material, factor);
}
void MeshRenderer::clear_material_factor(std::size_t material) {
    object_.scene().clear_material_factor(object_.id_, material);
}
void MeshRenderer::set_custom_material(std::size_t material, std::shared_ptr<const CustomMaterial> custom) {
    object_.scene().set_custom_material(object_.id_, material, std::move(custom));
}
void MeshRenderer::set_primitive_visible(std::size_t primitive, bool visible) {
    object_.scene().set_primitive_visible(object_.id_, primitive, visible);
}
void MeshRenderer::set_casts_shadows(bool casts) { object_.scene().set_casts_shadows(object_.id_, casts); }
RenderBounds MeshRenderer::bounds() const { return object_.scene().instance(object_.id_).bounds; }
void Scene::set_material_factor(Id id, std::size_t material, Vec3 factor) {
    auto &value = get(id);
    require(finite(factor) && factor.x >= 0 && factor.x <= 1 && factor.y >= 0 && factor.y <= 1 && factor.z >= 0 &&
                factor.z <= 1,
            "Invalid render material factor");
    value.factors.at(material) = factor;
}
void Scene::clear_material_factor(Id id, std::size_t material) {
    auto &value = get(id);
    value.factors.at(material) = value.asset->materials_->material_data.at(material).factor;
}
void Scene::set_custom_material(Id id, std::size_t material, std::shared_ptr<const CustomMaterial> custom) {
    auto &slots = get(id).custom_materials;
    if (material >= slots.size())
        throw std::out_of_range("Custom material slot is outside the mesh's materials");
    slots[material] = std::move(custom);
}
void Scene::set_visible(Id id, bool visible) {
    get(id).visible = visible;
    component(id, typeid(MeshRenderer))->enabled = visible;
}
void Scene::set_primitive_visible(Id id, std::size_t primitive, bool visible) {
    get(id).primitive_visible.at(primitive) = visible;
}
void Scene::set_casts_shadows(Id id, bool casts) { get(id).casts_shadows = casts; }
RenderBounds Scene::bounds() const {
    RenderBounds result;
    for (auto id : instances()) {
        const auto &value = instance(id);
        if (!value.visible || !value.active)
            continue;
        for (std::size_t i = 0; i < value.primitive_bounds.size(); ++i)
            if (value.primitive_visible[i] && value.primitive_bounds[i].valid) {
                expand(result, value.primitive_bounds[i].minimum);
                expand(result, value.primitive_bounds[i].maximum);
            }
    }
    return result;
}
MeshSnapshot Scene::snapshot(SceneGeometryBudget budget) const {
    std::size_t corners = 0;
    const auto objects = instances();
    for (auto id : objects) {
        const auto size = instance(id).asset->indices().size();
        require(size <= std::numeric_limits<std::size_t>::max() - corners, "Snapshot vertex overflow");
        corners += size;
    }
    (void)validate_scene_geometry(corners, budget);
    MeshSnapshot result;
    result.vertices.reserve(corners);
    RenderBounds bounds;
    for (auto id : objects) {
        const auto &value = instance(id);
        const auto &asset = *value.asset;
        const auto &description = *asset.materials();
        const auto material_offset = result.material_data.size(), texture_offset = result.textures.size();
        require(description.material_data.size() <= INT_MAX && description.textures.size() <= INT_MAX &&
                    material_offset <= INT_MAX - description.material_data.size() &&
                    texture_offset <= INT_MAX - description.textures.size(),
                "Snapshot material index overflow");
        result.textures.insert(result.textures.end(), description.textures.begin(), description.textures.end());
        for (std::size_t i = 0; i < description.material_data.size(); ++i) {
            auto material = description.material_data[i];
            material.factor = value.factors[i];
            for (auto *index : {&material.texture, &material.normal_texture, &material.metallic_roughness_texture,
                                &material.emissive_texture, &material.occlusion_texture})
                if (*index >= 0)
                    *index += static_cast<int>(texture_offset);
            result.material_data.push_back(std::move(material));
        }
        result.mesh_nodes += description.mesh_nodes;
        result.skins += description.skins;
        result.joints += description.joints;
        result.bind_deviation = std::max(result.bind_deviation, description.bind_deviation);
        result.default_is_bind_pose &= description.default_is_bind_pose;
        result.clips.insert(result.clips.end(), description.clips.begin(), description.clips.end());
        result.notices.insert(result.notices.end(), description.notices.begin(), description.notices.end());
        for (std::size_t i = 0; i < asset.draws().size(); ++i) {
            const auto &draw = asset.draws()[i];
            const bool visible = value.visible && value.active && value.primitive_visible[i];
            const auto factor = draw.material < 0 ? Vec3{1, 1, 1} : value.factors[draw.material];
            result.primitives.push_back(
                {draw.node_name, draw.mesh_name,
                 draw.material < 0 ? "default" : description.material_data[draw.material].name,
                 value.palette[draw.node], static_cast<std::uint32_t>(result.vertices.size()), draw.index_count,
                 draw.material < 0 ? -1 : static_cast<int>(material_offset) + draw.material, visible});
            if (draw.skinned)
                result.skinned_vertices += draw.index_count;
            bool reversed = false;
            for (std::size_t j = draw.first_index; j < std::size_t(draw.first_index) + draw.index_count; ++j) {
                const auto corner = j - draw.first_index;
                const auto &source = asset.vertices()[asset.indices()[j]];
                auto transform = value.palette[draw.palette_offset];
                if (draw.skinned) {
                    transform = {};
                    for (unsigned influence = 0; influence < 4; ++influence)
                        if (source.weights[influence] != 0)
                            for (unsigned k = 0; k < 16; ++k)
                                transform[k] += value.palette[draw.palette_offset + source.joints[influence]][k] *
                                                source.weights[influence];
                }
                // The first corner's matrix decides the triangle's winding, as it does on the GPU.
                if (corner % 3 == 0)
                    reversed = detail::reverses_winding(transform);
                const auto position = point(transform, source.position);
                result.vertices.push_back(
                    {position,
                     normal(transform, source.normal),
                     {source.color.x * factor.x, source.color.y * factor.y, source.color.z * factor.z},
                     source.uv,
                     tangent(transform, source.tangent),
                     source.alpha});
                if (visible)
                    expand(bounds, position);
                // A reversed triangle swaps its last two corners, keeping its source winding against its normals.
                if (reversed && corner % 3 == 2)
                    std::swap(result.vertices[result.vertices.size() - 2], result.vertices.back());
            }
        }
    }
    result.materials = result.material_data.size();
    if (bounds.valid) {
        result.minimum = bounds.minimum;
        result.maximum = bounds.maximum;
    }
    return result;
}
} // namespace anima
