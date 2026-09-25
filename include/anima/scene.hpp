#pragma once
#include <anima/assets/scene_budget.hpp>
#include <anima/mesh.hpp>
#include <compare>
#include <map>
#include <span>
#include <typeindex>

namespace anima {
class GameObject;
class MeshRenderer;
class ObjectTransform;
class Scene;
class SceneSet;
// Scene-scoped persistent identity. Zero is null; runtime Scene::Id is not persisted.
struct ObjectKey {
    std::uint64_t value{};
    auto operator<=>(const ObjectKey &) const = default;
    [[nodiscard]] std::string string() const;
    // Canonical decimal, including "0" for null. Rejects overflow and alternate spellings.
    static ObjectKey parse(std::string_view text);
};
template <class T> class ComponentRef;
enum class ReparentMode { keep_world, keep_local };
namespace detail {
struct ComponentRecord;
struct ScenePersistence;
struct SceneDriver;
struct SceneLifetime {
    Scene *scene{};
};
} // namespace detail
// Resource-backed scene. Mutation validates only changing instance data; never
// deforms or validates a whole vertex array per frame. Single-threaded, between draws.
class Scene {
  public:
    struct Id {
        std::uint64_t owner{}, generation{};
        std::size_t slot{};
        bool operator==(const Id &) const = default;
        auto operator<=>(const Id &) const = default;
    };
    struct Instance {
        std::shared_ptr<const Mesh> asset;
        std::vector<Mat4> palette;
        std::vector<Vec3> factors;
        std::vector<bool> primitive_visible;
        std::vector<RenderBounds> primitive_bounds;
        // Union of all primitive bounds, including hidden equipment. Conservative
        // broad phase; visibility changes cannot invalidate it between poses.
        RenderBounds bounds;
        bool visible = true;
        bool active = true; // Inherited object activation; separate from authored visibility.
    };
    Scene();
    ~Scene();
    Scene(const Scene &) = delete;
    Scene &operator=(const Scene &) = delete;
    // The scene owns objects. Handles neither extend its lifetime nor own a copy.
    [[nodiscard]] GameObject create(std::string name = {}, std::shared_ptr<const Mesh> mesh = {});
    [[nodiscard]] GameObject object(Id id);
    // Missing/null keys return an invalid handle. Keys never keep objects alive.
    [[nodiscard]] GameObject find(ObjectKey key) const noexcept;
    [[nodiscard]] std::vector<GameObject> roots();
    // Snapshot each call: newly added/activated components wait until next call.
    void update(double seconds);
    void fixed_update(double seconds);
    // Reconcile optional on_enable/on_disable hooks without advancing a clock.
    // Also performed at entry to update/fixed_update. Hooks must be noexcept.
    void synchronize_lifecycle();
    template <class T> [[nodiscard]] std::vector<ComponentRef<T>> components();
    [[nodiscard]] bool contains(Id id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return object_count_; }
    [[nodiscard]] Id add(std::shared_ptr<const Mesh> asset);
    void remove(Id id);
    void set_pose(Id id, const Pose &pose, const Mat4 &world = identity());
    void set_transform(Id id, const Mat4 &world);
    void set_material_factor(Id id, std::size_t material, Vec3 factor);
    void clear_material_factor(Id id, std::size_t material);
    void set_visible(Id id, bool visible);
    void set_primitive_visible(Id id, std::size_t primitive, bool visible);
    [[nodiscard]] const Instance &instance(Id id) const;
    [[nodiscard]] std::span<const Id> instances() const { return active_; }
    [[nodiscard]] RenderBounds bounds() const;
    // Explicit, expensive tooling/reference export; never used by GPU rendering.
    // Expands all corners on the CPU under the caller's diagnostic byte budget.
    [[nodiscard]] MeshSnapshot snapshot(SceneGeometryBudget budget = {}) const;

  private:
    friend class GameObject;
    friend class MeshRenderer;
    friend class FittedSet;
    friend class Prefab;
    friend struct AttachmentSet;
    friend struct detail::ScenePersistence;
    friend class SceneSet;
    friend struct detail::SceneDriver;
    void invalidate() noexcept;
    void release() noexcept;
    struct Slot {
        Instance value;
        std::uint64_t generation = 1;
        bool alive{};
        bool active_self = true, active_hierarchy = true;
        std::string name;
        ObjectKey key;
        Mat4 world = identity();
        Mat4 local = identity();
        std::optional<Id> parent;
        std::vector<Id> children;
        std::optional<Pose> pose;
        std::map<std::type_index, std::shared_ptr<detail::ComponentRecord>> components;
    };
    Slot &slot(Id id);
    GameObject create_with_key(ObjectKey key, std::string name, std::shared_ptr<const Mesh> mesh);
    const Slot &slot(Id id) const;
    void assign_mesh(Id id, std::shared_ptr<const Mesh> mesh);
    void set_local_transform(Id id, const Mat4 &local);
    void reparent(Id id, std::optional<Id> parent, ReparentMode mode);
    std::vector<Id> subtree(Id id) const;
    void refresh_activation(std::span<const Id> objects) noexcept;
    void update_transform(Id id, const Mat4 &local, const Mat4 &world, const Pose *replacement = nullptr);
    Mat4 to_local(Id id, const Mat4 &world) const;
    std::shared_ptr<detail::ComponentRecord> component(Id id, std::type_index type) const;
    bool detach_component(Id id, std::type_index type);
    static void run_components(std::span<Scene *const> scenes, double seconds, bool fixed, bool lifecycle_only = false);
    Instance &get(Id id);
    static void pose(Instance &instance, const Pose &pose, const Mat4 &world);
    std::uint64_t owner_;
    std::shared_ptr<detail::SceneLifetime> lifetime_;
    std::size_t object_count_{};
    std::uint64_t next_key_ = 1; // Zero means exhausted; never wrap/reuse a retired key.
    std::map<ObjectKey, Id> keys_;
    std::vector<Slot> slots_;
    std::vector<Id> active_;
    bool updating_{};
    std::size_t constructing_{};
};

// Default, stale and foreign handles are checked. All access is single-threaded,
// just like Scene mutation. Dropping a handle leaves the scene-owned object alive.
class GameObject {
  public:
    GameObject() = default;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] Scene::Id id() const noexcept { return id_; }
    [[nodiscard]] ObjectKey key() const;
    [[nodiscard]] std::string name() const;
    void set_name(std::string name);
    [[nodiscard]] bool active_self() const;
    // Effective state includes every ancestor; activation never changes enabled flags.
    [[nodiscard]] bool active_in_hierarchy() const;
    void set_active(bool active);
    [[nodiscard]] ObjectTransform transform() const;
    // Every object, including an empty one, has an identity transform initially.
    [[nodiscard]] Mat4 world_matrix() const;
    [[nodiscard]] Mat4 local_matrix() const;
    [[nodiscard]] Vec3 local_position() const;
    void set_local_position(Vec3 position);
    void set_local_transform(const Transform &transform);
    void set_local_matrix(const Mat4 &local);
    [[nodiscard]] std::optional<GameObject> parent() const;
    [[nodiscard]] std::vector<GameObject> children() const;
    void set_parent(GameObject parent, ReparentMode mode = ReparentMode::keep_world);
    void clear_parent(ReparentMode mode = ReparentMode::keep_world);
    [[nodiscard]] Vec3 position() const;
    void set_position(Vec3 position);
    void set_transform(const Transform &transform);
    void set_world_matrix(const Mat4 &world);
    [[nodiscard]] bool has_renderer() const;
    [[nodiscard]] MeshRenderer add_mesh(std::shared_ptr<const Mesh> mesh);
    [[nodiscard]] MeshRenderer renderer() const;
    void remove_mesh();
    void destroy();
    template <class T, class... Args> ComponentRef<T> add_component(Args &&...args);
    template <class T> [[nodiscard]] ComponentRef<T> get_component() const;
    template <class T> [[nodiscard]] bool has_component() const;
    template <class T> bool remove_component();
    [[nodiscard]] std::vector<std::type_index> component_types() const;

  private:
    friend class Scene;
    friend class MeshRenderer;
    friend class FittedSet;
    friend class Prefab;
    friend struct AttachmentSet;
    friend struct detail::ScenePersistence;
    GameObject(std::weak_ptr<detail::SceneLifetime> lifetime, Scene::Id id) : lifetime_(std::move(lifetime)), id_(id) {}
    Scene &scene() const;
    std::weak_ptr<detail::SceneLifetime> lifetime_;
    Scene::Id id_{};
};

// A checked view of the object's current mesh component, not a second owner.
class MeshRenderer {
  public:
    [[nodiscard]] std::shared_ptr<const Mesh> mesh() const;
    void set_mesh(std::shared_ptr<const Mesh> mesh);
    void set_pose(const Pose &pose);
    void set_visible(bool visible);
    void set_material_factor(std::size_t material, Vec3 factor);
    void clear_material_factor(std::size_t material);
    void set_primitive_visible(std::size_t primitive, bool visible);
    [[nodiscard]] RenderBounds bounds() const;

  private:
    friend class Scene;
    friend class GameObject;
    explicit MeshRenderer(GameObject object) : object_(std::move(object)) {}
    GameObject object_;
};

// Every GameObject has this checked transform view. Mutation is explicit so scene
// bounds and skinning palettes stay in sync; it never exposes a dangling reference.
class ObjectTransform {
  public:
    [[nodiscard]] Vec3 position() const { return object_.position(); }
    [[nodiscard]] Mat4 matrix() const { return object_.world_matrix(); }
    [[nodiscard]] Vec3 local_position() const { return object_.local_position(); }
    [[nodiscard]] Mat4 local_matrix() const { return object_.local_matrix(); }
    void set_position(Vec3 position) { object_.set_position(position); }
    void set(const Transform &value) { object_.set_transform(value); }
    void set_matrix(const Mat4 &value) { object_.set_world_matrix(value); }
    void set_local_position(Vec3 position) { object_.set_local_position(position); }
    void set_local(const Transform &value) { object_.set_local_transform(value); }
    void set_local_matrix(const Mat4 &value) { object_.set_local_matrix(value); }

  private:
    friend class Scene;
    friend class GameObject;
    explicit ObjectTransform(GameObject object) : object_(std::move(object)) {}
    GameObject object_;
};
} // namespace anima
#include <anima/components.hpp>
