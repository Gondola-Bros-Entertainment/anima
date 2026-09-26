#pragma once
#include <anima/assets/scene_budget.hpp>
#include <anima/mesh.hpp>
#include <compare>
#include <map>
#include <span>
#include <typeindex>

/// @file
/// Scenes of GameObjects with transforms, optional mesh renderers and native C++ components.
///
/// Part of the `anima::assets` target; no display or GPU is required. A Scene owns its objects and
/// their components. GameObject, MeshRenderer, ObjectTransform and ComponentRef are checked,
/// non-owning handles: any use of a default or stale handle, or of a Scene::Id from another scene,
/// throws `std::out_of_range`, except validity checks and GameObject::id(). Invalid arguments throw
/// `std::invalid_argument`, or MathError (derived from it) for a matrix or rotation that cannot be
/// inverted or normalized; calls made in the wrong state throw `std::logic_error`. A failed call
/// leaves the scene unchanged unless its comment says otherwise.
///
/// Matrices must be finite and affine, with a bottom row within `1e-5` of (0, 0, 0, 1). They are
/// stored exactly as given, so shear, reflection and nonuniform scale are preserved.
///
/// Use a scene, its handles and any SceneSet that owns it from one thread, and never while a
/// renderer draws it; holding a `const` shared pointer does not synchronize access.

namespace anima {
class GameObject;
class MeshRenderer;
class ObjectTransform;
class Scene;
class SceneSet;
/// Persistent identity of an object within one scene; zero is null.
///
/// Keys are unique within their scene and do not change when an object is renamed, reparented,
/// activated or given other components; two scenes may use the same key. A scene allocates keys in
/// increasing order and never reuses one, even after deletion or a failed creation. Scene documents
/// store keys; runtime Scene::Id values are never persisted.
struct ObjectKey {
    std::uint64_t value{};
    auto operator<=>(const ObjectKey &) const = default;
    /// Canonical decimal text, `"0"` for null.
    [[nodiscard]] std::string string() const;
    /// Parses canonical decimal text as string() writes it. Throws `std::invalid_argument` for empty
    /// text, a sign, whitespace, a leading zero, any other character or a value above 2^64 - 1.
    static ObjectKey parse(std::string_view text);
};
template <class T> class ComponentRef;
/// Which matrix GameObject::set_parent and GameObject::clear_parent preserve.
enum class ReparentMode {
    keep_world, ///< Keeps the world matrix; a new parent's world matrix must be invertible.
    keep_local  ///< Keeps the local matrix, so the world matrix follows the new parent.
};
namespace detail {
struct ComponentRecord;
struct ScenePersistence;
struct SceneDriver;
struct SceneLifetime {
    Scene *scene{};
};
} // namespace detail
/// Owner of a hierarchy of GameObjects and their components.
///
/// Scenes cannot be copied or moved. Mutations validate only the instance data they change, never
/// whole vertex arrays, and hierarchy traversal is iterative, so depth does not consume the call
/// stack.
///
/// Components are ordinary C++ types attached with GameObject::add_component. The scene calls
/// these optional public hooks only while a component is active (enabled, on an object active in
/// the hierarchy):
/// - `void on_update(double)` and `void on_late_update(double)`, from update();
/// - `void on_fixed_update(double)`, from fixed_update();
/// - `void on_enable() noexcept` and `void on_disable() noexcept`, when a lifecycle reconciliation
///   sees the active state change; removal calls `on_disable()` at once if `on_enable()` was
///   delivered. Their `noexcept` is checked at compile time.
///
/// Members of these names that return anything but `void` are not called. There are no implicit
/// creation or destruction hooks: constructors and destructors own resources.
class Scene {
  public:
    /// Runtime handle of one object: its scene's process-unique owner number, a storage slot and
    /// that slot's generation. Removing the object invalidates the Id for good, even when the slot
    /// is reused. A default Id is never valid.
    struct Id {
        std::uint64_t owner{}, generation{};
        std::size_t slot{};
        bool operator==(const Id &) const = default;
        auto operator<=>(const Id &) const = default;
    };
    /// Render state of one object's MeshRenderer. A renderer draws a primitive only when #visible,
    /// #active and its #primitive_visible entry are all true.
    struct Instance {
        /// Shared immutable mesh.
        std::shared_ptr<const Mesh> asset;
        /// World matrix of each mesh node, followed by one skinning matrix per joint of each skin.
        std::vector<Mat4> palette;
        /// Linear RGB factor per mesh material: the authored value or this object's override.
        std::vector<Vec3> factors;
        /// Visibility per mesh primitive.
        std::vector<bool> primitive_visible;
        /// Conservative world bounds per mesh primitive.
        std::vector<RenderBounds> primitive_bounds;
        /// Union of all primitive bounds, including hidden primitives, so visibility changes cannot
        /// invalidate it between poses.
        RenderBounds bounds;
        /// Authored renderer visibility; not inherited from parents.
        bool visible = true;
        /// The object's GameObject::active_in_hierarchy state, separate from #visible.
        bool active = true;
    };
    Scene();
    /// Invalidates every handle to the scene, then sends `on_disable()` to each component that
    /// received `on_enable()` and destroys the components. Objects cannot be created meanwhile.
    ~Scene();
    Scene(const Scene &) = delete;
    Scene &operator=(const Scene &) = delete;
    /// Creates a root object named @p name, active, with an identity transform and, when @p mesh is
    /// not null, a renderer as GameObject::add_mesh creates. Names need not be unique.
    ///
    /// The object receives the scene's next ObjectKey, which a failed creation still consumes.
    /// Throws `std::overflow_error` once keys are exhausted and `std::logic_error` during teardown.
    [[nodiscard]] GameObject create(std::string name = {}, std::shared_ptr<const Mesh> mesh = {});
    /// Handle to the live object @p id.
    [[nodiscard]] GameObject object(Id id);
    /// Handle to the object with @p key, or an invalid handle for a missing or null key.
    [[nodiscard]] GameObject find(ObjectKey key) const noexcept;
    /// Handles to every object without a parent.
    [[nodiscard]] std::vector<GameObject> roots();
    /// Runs one frame: reconciles lifecycle as synchronize_lifecycle() does, then calls every
    /// participant's `on_update(seconds)`, then every participant's `on_late_update(seconds)`.
    ///
    /// Participants are the components attached and active at entry; components added or activated
    /// during the call wait for a later call, and one removed, disabled or deactivated during the
    /// call skips its remaining hooks. Order within a phase is unspecified. @p seconds is passed
    /// through unchanged: the application owns accumulation, pausing and time scale.
    ///
    /// Throws `std::invalid_argument` unless @p seconds is finite and nonnegative, and
    /// `std::logic_error` while the scene is running component hooks or cleanup, constructing a
    /// component, held by a scene driver or being destroyed; nested updates are therefore
    /// rejected. A hook's exception propagates after the scene unlocks, without undoing earlier
    /// hooks. The scene must outlive the call.
    void update(double seconds);
    /// Runs one fixed step: reconciles lifecycle, then calls every participant's
    /// `on_fixed_update(seconds)`, under the same rules as update(). It neither advances a clock nor
    /// steps physics.
    void fixed_update(double seconds);
    /// Delivers pending `on_enable()` and `on_disable()` notifications without other hooks or time.
    ///
    /// Each component attached at entry whose active state differs from its last notification is
    /// reconciled, disables before enables; order is otherwise unspecified. A new component's first
    /// `on_enable()` waits for a reconciliation, so construction and loading never expose partly
    /// built objects, and a flag turned off and on again in between produces no notification.
    /// Components that the hooks add or activate wait for the next call. Hooks may change objects
    /// and components but not run scene updates. Throws `std::logic_error` in the same states as
    /// update().
    void synchronize_lifecycle();
    /// Every attachment of component type `T`, including disabled ones and those on inactive
    /// objects.
    template <class T> [[nodiscard]] std::vector<ComponentRef<T>> components();
    /// Whether @p id names a live object of this scene.
    [[nodiscard]] bool contains(Id id) const noexcept;
    /// Number of live objects, including those without renderers.
    [[nodiscard]] std::size_t size() const noexcept { return object_count_; }
    /// Creates an unnamed root object rendering @p asset, as create() does, and returns its Id.
    /// Throws `std::invalid_argument` for a null mesh.
    [[nodiscard]] Id add(std::shared_ptr<const Mesh> asset);
    /// Destroys object @p id and its descendants, as GameObject::destroy does.
    void remove(Id id);
    /// Sets the animation pose of @p id's renderer together with its world matrix, as
    /// GameObject::set_world_matrix does; @p world defaults to identity, not the current placement.
    ///
    /// Only Pose::world is used: one affine matrix per mesh node, matching the mesh's node count.
    /// Compute it with sample_pose or pose_from_local; changing Pose::local alone has no effect.
    /// Throws `std::logic_error` when the object has no renderer.
    void set_pose(Id id, const Pose &pose, const Mat4 &world = identity());
    /// Sets @p id's world matrix, as GameObject::set_world_matrix does.
    void set_transform(Id id, const Mat4 &world);
    /// Replaces the authored linear RGB factor of mesh material @p material for this object only.
    /// Each channel must be finite and in [0, 1]. Throws `std::out_of_range` for an index outside
    /// the mesh's materials and `std::logic_error` when the object has no renderer.
    void set_material_factor(Id id, std::size_t material, Vec3 factor);
    /// Restores the mesh's authored factor for material @p material. Throws as
    /// set_material_factor() does.
    void clear_material_factor(Id id, std::size_t material);
    /// Shows or hides @p id's renderer and sets its MeshRenderer component's enabled flag to match.
    /// Per-primitive choices are kept. Throws `std::logic_error` when the object has no renderer.
    void set_visible(Id id, bool visible);
    /// Shows or hides one primitive of @p id's mesh. Throws `std::out_of_range` for an index
    /// outside the mesh's primitives and `std::logic_error` when the object has no renderer.
    void set_primitive_visible(Id id, std::size_t primitive, bool visible);
    /// Render state of @p id's renderer, borrowed until the scene next changes. Throws
    /// `std::logic_error` when the object has no renderer.
    [[nodiscard]] const Instance &instance(Id id) const;
    /// Objects that have renderers, including hidden ones and those on inactive objects. Borrowed
    /// until the scene next changes.
    [[nodiscard]] std::span<const Id> instances() const { return active_; }
    /// Union of the bounds of visible primitives of visible renderers on active objects; invalid
    /// when there are none.
    [[nodiscard]] RenderBounds bounds() const;
    /// Expands every renderer's current geometry into an owning MeshSnapshot on the CPU, for tools
    /// and reference checks; rendering never uses it.
    ///
    /// Primitives that are hidden, or whose renderer is hidden or on an inactive object, are
    /// included but marked invisible and left out of the snapshot bounds. Throws SceneCapacityError
    /// when the expanded vertices exceed @p budget, and `std::runtime_error` when a vertex transform
    /// is singular.
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
    void assign_mesh(Id id, std::shared_ptr<const Mesh> mesh, const Pose *initial_pose = nullptr);
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
    std::size_t next_free_slot_{}; // Lower bound for the first reusable slot.
    std::vector<Id> active_;
    bool updating_{};
    std::size_t constructing_{};
};

/// Checked, copyable handle to one object of a Scene.
///
/// Copying or dropping a handle never creates or destroys the object. Every object has an
/// ObjectTransform and at most one component of each type, including an optional MeshRenderer.
/// Transform setters without `local` in their name work in world space and derive the local matrix
/// from the inverse of the parent's world matrix, so they throw MathError under a singular parent;
/// the local setters do not invert. A transform change validates the whole affected subtree, then
/// moves descendants and renderers at once, keeping their animation poses.
class GameObject {
  public:
    /// Invalid handle.
    GameObject() = default;
    /// Whether the handle refers to a live object.
    [[nodiscard]] bool valid() const noexcept;
    /// Runtime identity, available even when the handle is invalid.
    [[nodiscard]] Scene::Id id() const noexcept { return id_; }
    /// Persistent identity within the scene.
    [[nodiscard]] ObjectKey key() const;
    [[nodiscard]] std::string name() const;
    void set_name(std::string name);
    /// Authored activation flag, independent of ancestors.
    [[nodiscard]] bool active_self() const;
    /// Whether this object and every ancestor are active.
    [[nodiscard]] bool active_in_hierarchy() const;
    /// Sets the authored activation flag; the effective state of the whole subtree updates at once.
    ///
    /// Objects start active, and descendants keep their own flags. An inactive object keeps its
    /// transform, components and persistence, but is neither drawn nor counted in Scene::bounds.
    /// Its components skip hooks at once and receive `on_disable()` at the next lifecycle
    /// reconciliation; their enabled flags do not change.
    void set_active(bool active);
    /// Transform view of this object.
    [[nodiscard]] ObjectTransform transform() const;
    /// World matrix; identity for a new object, including one without a renderer.
    [[nodiscard]] Mat4 world_matrix() const;
    /// Matrix relative to the parent, or to the world for a root.
    [[nodiscard]] Mat4 local_matrix() const;
    /// Translation of local_matrix().
    [[nodiscard]] Vec3 local_position() const;
    /// Replaces the translation of the local matrix, keeping its other columns.
    void set_local_position(Vec3 position);
    /// Sets the local matrix to anima::matrix(@p transform).
    void set_local_transform(const Transform &transform);
    /// Sets the matrix relative to the parent.
    void set_local_matrix(const Mat4 &local);
    /// Parent handle, or empty for a root.
    [[nodiscard]] std::optional<GameObject> parent() const;
    /// Direct children, in the order they were attached.
    [[nodiscard]] std::vector<GameObject> children() const;
    /// Makes this object a child of @p parent, keeping the matrix that @p mode selects. Throws
    /// `std::invalid_argument` when @p parent belongs to another scene or is this object or one of
    /// its descendants.
    void set_parent(GameObject parent, ReparentMode mode = ReparentMode::keep_world);
    /// Makes this object a root, keeping the matrix that @p mode selects; no inverse is needed.
    void clear_parent(ReparentMode mode = ReparentMode::keep_world);
    /// World translation.
    [[nodiscard]] Vec3 position() const;
    /// Replaces the translation of the world matrix, keeping its other columns.
    void set_position(Vec3 position);
    /// Sets the world matrix to anima::matrix(@p transform).
    void set_transform(const Transform &transform);
    /// Sets the world matrix; the local matrix becomes the inverse parent world matrix times
    /// @p world.
    void set_world_matrix(const Mat4 &world);
    /// Whether the object has a MeshRenderer.
    [[nodiscard]] bool has_renderer() const;
    /// Adds a renderer for @p mesh with its rest pose, authored material factors, and the renderer
    /// and every primitive visible. Throws `std::invalid_argument` for a null mesh and
    /// `std::logic_error` when the object already has a renderer.
    [[nodiscard]] MeshRenderer add_mesh(std::shared_ptr<const Mesh> mesh);
    /// View of the current renderer. Throws `std::logic_error` when there is none.
    [[nodiscard]] MeshRenderer renderer() const;
    /// Removes the renderer, if any, keeping the object and its transform.
    void remove_mesh();
    /// Destroys this object and all its descendants.
    ///
    /// Every handle to them becomes invalid first, and their keys are never reused. Then each
    /// component that received `on_enable()` gets `on_disable()` and every component is destroyed;
    /// a value still pinned by a running scene update or ComponentRef::operator-> is destroyed when
    /// the pin is released.
    void destroy();
    /// Constructs a component of type `T`, an unqualified value type, attaches it and returns its
    /// handle.
    ///
    /// This object is passed first when that compiles: an aggregate is initialized as
    /// `T{object, args...}` when valid and `T{args...}` otherwise; another type is constructed as
    /// `T(object, args...)` when possible and `T(args...)` otherwise. The component starts enabled,
    /// and its first `on_enable()` waits for a lifecycle reconciliation.
    ///
    /// Throws `std::logic_error` when the object already has a `T`, including a recursive add from
    /// the constructor, or when construction removed the object or the attachment. A failed
    /// construction releases the type; its side effects on other objects are not undone. For
    /// ObjectTransform it throws `std::logic_error`; for MeshRenderer it forwards @p args to
    /// add_mesh().
    template <class T, class... Args> ComponentRef<T> add_component(Args &&...args);
    /// Handle to the attached `T`, or an invalid handle when there is none.
    template <class T> [[nodiscard]] ComponentRef<T> get_component() const;
    /// Whether a `T` is attached.
    template <class T> [[nodiscard]] bool has_component() const;
    /// Detaches and destroys the `T` component, returning whether one was attached.
    ///
    /// Its handles become invalid first, then it gets `on_disable()` if it received `on_enable()`. A
    /// value still pinned by a running scene update or ComponentRef::operator-> is destroyed when
    /// the pin is released. For ObjectTransform it throws `std::logic_error`; for MeshRenderer it
    /// calls remove_mesh().
    template <class T> bool remove_component();
    /// Types of all attached components, including ObjectTransform and any MeshRenderer, in
    /// unspecified order.
    [[nodiscard]] std::vector<std::type_index> component_types() const;

  private:
    friend class Scene;
    friend class MeshRenderer;
    friend class FittedSet;
    friend class Prefab;
    friend class PrefabComposition;
    friend struct AttachmentSet;
    friend struct detail::ScenePersistence;
    GameObject(std::weak_ptr<detail::SceneLifetime> lifetime, Scene::Id id) : lifetime_(std::move(lifetime)), id_(id) {}
    Scene &scene() const;
    std::weak_ptr<detail::SceneLifetime> lifetime_;
    Scene::Id id_{};
};

/// Checked view of an object's current MeshRenderer component; not a second owner.
///
/// The view follows whichever renderer the object has: after GameObject::remove_mesh its calls
/// throw `std::logic_error` until a renderer is added again. Material and primitive indices refer
/// to the mesh's materials and primitives.
class MeshRenderer {
  public:
    /// Shared mesh being drawn.
    [[nodiscard]] std::shared_ptr<const Mesh> mesh() const;
    /// Replaces the mesh, keeping the object's transform and resetting the pose to rest, material
    /// factors to authored values, and the renderer and every primitive to visible. Throws
    /// `std::invalid_argument` for a null mesh.
    void set_mesh(std::shared_ptr<const Mesh> mesh);
    /// Sets the animation pose, keeping the object's placement; see Scene::set_pose.
    void set_pose(const Pose &pose);
    /// Shows or hides the renderer; see Scene::set_visible.
    void set_visible(bool visible);
    /// Overrides one material factor for this object; see Scene::set_material_factor.
    void set_material_factor(std::size_t material, Vec3 factor);
    /// Restores one authored material factor; see Scene::clear_material_factor.
    void clear_material_factor(std::size_t material);
    /// Shows or hides one primitive; see Scene::set_primitive_visible.
    void set_primitive_visible(std::size_t primitive, bool visible);
    /// Union of all primitive bounds in world space, including hidden primitives.
    [[nodiscard]] RenderBounds bounds() const;

  private:
    friend class Scene;
    friend class GameObject;
    explicit MeshRenderer(GameObject object) : object_(std::move(object)) {}
    GameObject object_;
};

/// Checked view of an object's transform, which is also its ObjectTransform component.
///
/// Members forward to the GameObject methods of the same meaning; names without `local` use world
/// space. Every object has this component, and it cannot be added, removed or disabled.
class ObjectTransform {
  public:
    /// World translation.
    [[nodiscard]] Vec3 position() const { return object_.position(); }
    /// World matrix.
    [[nodiscard]] Mat4 matrix() const { return object_.world_matrix(); }
    /// Translation relative to the parent.
    [[nodiscard]] Vec3 local_position() const { return object_.local_position(); }
    /// Matrix relative to the parent.
    [[nodiscard]] Mat4 local_matrix() const { return object_.local_matrix(); }
    /// Replaces the world translation; see GameObject::set_position.
    void set_position(Vec3 position) { object_.set_position(position); }
    /// Sets the world transform; see GameObject::set_transform.
    void set(const Transform &value) { object_.set_transform(value); }
    /// Sets the world matrix; see GameObject::set_world_matrix.
    void set_matrix(const Mat4 &value) { object_.set_world_matrix(value); }
    /// Replaces the local translation; see GameObject::set_local_position.
    void set_local_position(Vec3 position) { object_.set_local_position(position); }
    /// Sets the local transform; see GameObject::set_local_transform.
    void set_local(const Transform &value) { object_.set_local_transform(value); }
    /// Sets the local matrix; see GameObject::set_local_matrix.
    void set_local_matrix(const Mat4 &value) { object_.set_local_matrix(value); }

  private:
    friend class Scene;
    friend class GameObject;
    explicit ObjectTransform(GameObject object) : object_(std::move(object)) {}
    GameObject object_;
};
} // namespace anima
#include <anima/components.hpp>
