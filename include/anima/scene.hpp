#pragma once
#include <anima/assets/mesh_snapshot.hpp>
#include <anima/assets/scene_budget.hpp>
#include <anima/custom_material.hpp>
#include <anima/mesh.hpp>
#include <anima/mesh_placements.hpp>
#include <compare>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <typeindex>
#include <unordered_map>

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
/// Distances from the eye at which an object draws, in metres, measured to the center of its mesh's rest bounds
/// (Mesh::rest_bounds()) as its world matrix places it or, for an object with placements, as each copy is placed, as
/// Godot's visibility ranges measure to the center of an instance's bounds.
///
/// Outside [#begin, #end] the object, or the copy, does not draw in any pass. Within #begin_margin of #begin and
/// #end_margin of #end it dissolves with a 4x4 ordered dither instead of popping: it is whole from `begin +
/// begin_margin` to `end - end_margin` and gone at #begin and #end, and in between a share of its pixels proportional
/// to the distance into the margin is discarded, so it costs no blending or sorting. Fading out it keeps the pixels of
/// the dither's lowest thresholds and fading in those of its highest, so where one object's end margin spans the same
/// distances as another's begin margin, as when two models of one thing hand over, the two keep complementary pixels
/// from every viewpoint when their rest bounds' centers coincide as placed. Each measures to its own center, so centers
/// apart by `d` differ in distance by up to `d`, which leaves about `d / margin` of their pixels drawn twice or by
/// neither. It casts shadows while more than half of it draws. Ranges apply to perspective views; an orthographic view
/// draws every object whole.
///
/// The renderer culls an object, or a cluster of its placed copies (MeshPlacements::clusters()), only where it lies
/// wholly outside the range; the standard material's shaders hide the other copies outside it and dissolve the margins.
/// A custom material's shaders do so only through `animaVisibility()` and `animaDissolved()`, as custom_material.hpp
/// describes; without them an object draws whole inside its range, and so does every copy in a cluster that lies partly
/// inside, even one outside the range.
struct VisibilityRange {
    /// Nearest distance at which the object draws; 0, the default, draws it however near.
    float begin = 0;
    /// Farthest distance at which the object draws; infinity, the default, draws it however far.
    float end = std::numeric_limits<float>::infinity();
    /// Width over which it dissolves in beyond #begin; 0 makes it appear at once.
    float begin_margin = 0;
    /// Width over which it dissolves out before #end; 0 makes it vanish at once, and an endless range has none.
    float end_margin = 0;
    bool operator==(const VisibilityRange &) const = default;
};
/// Throws `std::invalid_argument` unless VisibilityRange::begin and the margins of @p range are finite and
/// nonnegative, its end is greater than its begin, and the margins fit between them ("A visibility range requires
/// 0 <= begin < end and margins that fit between them"), and for an end margin on an infinite end ("An endless
/// visibility range has no end margin").
void validate_visibility_range(const VisibilityRange &range);
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
/// stack. Creating an object takes amortized time independent of the scene's other objects, and
/// destroying objects takes amortized time proportional to them and their components, apart from a
/// term logarithmic in the number of free slots each time one is reused or freed. Prefabs and
/// documents add the terms that prefab.hpp states.
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
    /// #active and its #primitive_visible entry are all true, and into the key light's shadow maps
    /// only when #casts_shadows is also true and its material is lit and not AlphaMode::blend, or, for
    /// a primitive whose material slot has a custom material, when that material casts shadows.
    struct Instance {
        /// Shared immutable mesh.
        std::shared_ptr<const Mesh> asset;
        /// World matrix of each mesh node, followed by one skinning matrix per joint of each skin. With
        /// #placements, the copy that one identity placement would draw.
        std::vector<Mat4> palette;
        /// Linear RGB factor per mesh material: the authored value or this object's override.
        std::vector<Vec3> factors;
        /// Custom material per mesh material, which draws that material's primitives in place of it;
        /// null keeps the mesh's Material. Primitives without a material (IndexedDraw::material -1)
        /// always draw with the default Material.
        std::vector<std::shared_ptr<const CustomMaterial>> custom_materials;
        /// Visibility per mesh primitive.
        std::vector<bool> primitive_visible;
        /// Conservative world bounds per mesh primitive, covering every copy of it.
        std::vector<RenderBounds> primitive_bounds;
        /// Union of all primitive bounds, including hidden primitives, so visibility changes cannot
        /// invalidate it between poses.
        RenderBounds bounds;
        /// Authored renderer visibility; not inherited from parents.
        bool visible = true;
        /// The object's GameObject::active_in_hierarchy state, separate from #visible.
        bool active = true;
        /// Whether the renderer's primitives cast shadows. They receive shadows either way.
        bool casts_shadows = true;
        /// Copies drawn in place of the one at the object, relative to it, or null to draw that one; see
        /// set_placements(). Visibility, factors, custom materials and shadow casting apply to every copy.
        std::shared_ptr<const MeshPlacements> placements;
        /// The object's world matrix, which places #placements.
        Mat4 world = identity();
        /// Distances at which the object, or each of its copies, draws; see set_visibility_range().
        VisibilityRange visibility_range;
    };
    Scene();
    /// Invalidates every handle to the scene, then sends `on_disable()` to each component that
    /// received `on_enable()` and destroys the components. Objects cannot be created meanwhile.
    ///
    /// Destroying the scene while it runs component hooks or cleanup, constructs a component or is
    /// held by a scene driver writes a diagnostic to `stderr` and terminates the program: the call in
    /// progress would keep using the freed scene. Destroy it after that call returns.
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
    /// Participants are the components attached and active at entry whose types declare any of the
    /// hooks above; components added or activated during the call wait for a later call, and one
    /// removed, disabled or deactivated during the call skips its remaining hooks. The call pins each
    /// participant's value, so one removed during the call is destroyed when the call ends; a
    /// component without hooks is not pinned. Order within a phase is unspecified. The call visits
    /// only components whose types declare hooks, so other objects and components add no work;
    /// adding or removing such components can make the next call first sort them, in time
    /// O(h log h) for h of them. @p seconds is passed through unchanged: the application owns
    /// accumulation, pausing and time scale.
    ///
    /// Throws `std::invalid_argument` unless @p seconds is finite and nonnegative, and
    /// `std::logic_error` while the scene is running component hooks or cleanup, constructing a
    /// component, held by a scene driver or being destroyed; nested updates are therefore
    /// rejected. A hook's exception propagates after the scene unlocks, without undoing earlier
    /// hooks. Destroying the scene during the call terminates the program, as ~Scene() states.
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
    /// objects. It visits only attachments of `T`, so objects and components of other types add
    /// no work; adding or removing attachments of `T` can make the next query first sort them, in
    /// time O(k log k) for k of them.
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
    /// The renderer keeps a copy of @p pose in storage it reuses: once it has held a pose with at
    /// least as many Pose::local and Pose::world entries since its mesh was set, the call allocates
    /// only as a transform change does (see GameObject). Throws `std::logic_error` when the object
    /// has no renderer or its renderer draws placements, whose copies keep the rest pose.
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
    /// Draws the primitives of mesh material @p material of this object with @p custom, or with the
    /// mesh's Material again when @p custom is null (Instance::custom_materials). The object's factor
    /// for that material still reaches the custom shaders. Throws `std::out_of_range` for an index
    /// outside the mesh's materials, `std::logic_error` when the object has no renderer, and
    /// `std::invalid_argument` when the renderer draws placements that @p custom does not read
    /// (CustomMaterial::reads_placements(): "A custom material that draws placements must read them").
    void set_custom_material(Id id, std::size_t material, std::shared_ptr<const CustomMaterial> custom);
    /// Shows or hides @p id's renderer and sets its MeshRenderer component's enabled flag to match.
    /// Per-primitive choices are kept. Throws `std::logic_error` when the object has no renderer.
    void set_visible(Id id, bool visible);
    /// Shows or hides one primitive of @p id's mesh. Throws `std::out_of_range` for an index
    /// outside the mesh's primitives and `std::logic_error` when the object has no renderer.
    void set_primitive_visible(Id id, std::size_t primitive, bool visible);
    /// Sets whether @p id's renderer casts shadows (Instance::casts_shadows). Throws
    /// `std::logic_error` when the object has no renderer.
    void set_casts_shadows(Id id, bool casts);
    /// Draws one copy of @p id's mesh per placement of @p placements, as MeshPlacements describes, instead of one
    /// copy at the object, or that one copy again when @p placements is null (Instance::placements). The copies
    /// draw the mesh's rest pose, and the renderer's bounds cover all of them. Moving the object then takes time
    /// proportional to the mesh's primitives, as before, not to the placements. Throws `std::logic_error` when the
    /// object has no renderer or its renderer has a pose (set_pose(), or one that a document or prefab supplied),
    /// and `std::invalid_argument` when @p placements copy another mesh ("Placements copy another mesh") or a custom
    /// material of the renderer does not read them (CustomMaterial::reads_placements(): "A custom material that
    /// draws placements must read them").
    void set_placements(Id id, std::shared_ptr<const MeshPlacements> placements);
    /// Draws @p id's renderer only at the distances @p range allows, as VisibilityRange describes. Throws
    /// `std::logic_error` when the object has no renderer and what validate_visibility_range() throws.
    void set_visibility_range(Id id, const VisibilityRange &range);
    /// Render state of @p id's renderer, borrowed until the scene next changes. Throws
    /// `std::logic_error` when the object has no renderer.
    [[nodiscard]] const Instance &instance(Id id) const;
    /// Objects that have renderers, including hidden ones and those on inactive objects. Borrowed
    /// until the scene next changes.
    [[nodiscard]] std::span<const Id> instances() const;
    /// Union of the bounds of visible primitives of visible renderers on active objects; invalid
    /// when there are none.
    [[nodiscard]] RenderBounds bounds() const;
    /// Expands every renderer's current geometry into an owning MeshSnapshot on the CPU, for tools
    /// and reference checks; rendering never uses it. Primitives keep their mesh materials, since a
    /// snapshot does not describe custom materials, and textures their meshes' images, which have no
    /// texels for a Mesh compiled with TexelRetention::until_upload.
    ///
    /// Each renderer, in instances() order, adds its mesh's textures and materials, with the renderer's material
    /// factors, and the mesh nodes, skins, joints, clips and notices of the mesh's MeshDescription.
    /// MeshSnapshot::bind_deviation is the largest of the descriptions' and MeshSnapshot::default_is_bind_pose holds
    /// when each of theirs does, while MeshSnapshot::skinned_vertices counts the snapshot's own skinned vertices,
    /// copies included.
    ///
    /// Each draw contributes its own triangles, never its levels of detail (IndexedDraw::levels), once per
    /// copy: a renderer with placements gives one primitive per placement and draw, in
    /// MeshPlacements::transforms() order, placed as set_placements() describes, and visibility ranges
    /// leave every copy in. Primitives that are hidden, or whose renderer is hidden or on an inactive object, are
    /// included but marked invisible and left out of the snapshot bounds. Normals follow normal(), and
    /// corners follow MeshSnapshot::vertices. Throws SceneCapacityError when the expanded vertices
    /// exceed @p budget, and `std::invalid_argument` when their count or the combined material or
    /// texture indices overflow.
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
    static constexpr std::size_t no_slot = SIZE_MAX;
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
        // Children in attachment order, linked through slot indices; no_slot ends a list.
        std::size_t first_child = no_slot, last_child = no_slot;
        std::size_t previous_sibling = no_slot, next_sibling = no_slot;
        // Position in active_ while the object has a renderer; compact_instances() moves it.
        mutable std::size_t instance = no_slot;
        std::optional<Pose> pose;
        std::map<std::type_index, std::shared_ptr<detail::ComponentRecord>> components;
    };
    // Attached components of one type, or every attached component whose type has hooks. Each
    // record stores its positions; removal moves the last record into the gap, so a list is
    // sorted again by slot, then type, only when it is read.
    struct ComponentList {
        std::vector<std::shared_ptr<detail::ComponentRecord>> records;
        bool ordered = true;
    };
    // An object that update_transform moves: its new world matrix and, with a renderer, where its
    // palette and primitive bounds start in the working lists, and their union.
    struct PosedObject {
        Id id;
        Mat4 world;
        std::size_t palette{}, bounds{};
        RenderBounds combined;
    };
    Id id_at(std::size_t index) const noexcept { return {owner_, slots_[index].generation, index}; }
    void link_child(std::size_t parent, std::size_t child) noexcept;
    void unlink_child(std::size_t child) noexcept;
    void release_slot(std::size_t index) noexcept;
    void retire_instance(Slot &entry) noexcept;
    void compact_instances() const noexcept;
    void index_component(const std::shared_ptr<detail::ComponentRecord> &record);
    void unindex_component(detail::ComponentRecord &record) noexcept;
    std::span<const std::shared_ptr<detail::ComponentRecord>> attached_components(std::type_index type);
    static std::span<const std::shared_ptr<detail::ComponentRecord>> ordered(ComponentList &list,
                                                                             bool scheduled) noexcept;
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
    static RenderBounds append_pose(const Mesh &asset, const Pose &pose, const Mat4 &world, std::vector<Mat4> &palette,
                                    std::vector<RenderBounds> &bounds);
    // Writes the world bounds of @p placements' copies of each primitive under @p world into @p bounds and
    // returns their union.
    static RenderBounds place(const MeshPlacements &placements, const Mat4 &world, std::span<RenderBounds> bounds);
    std::uint64_t owner_;
    std::shared_ptr<detail::SceneLifetime> lifetime_;
    std::size_t object_count_{};
    std::uint64_t next_key_ = 1; // Zero means exhausted; never wrap/reuse a retired key.
    // Live objects by ObjectKey::value.
    std::unordered_map<std::uint64_t, Id> keys_;
    std::vector<Slot> slots_;
    // Min-heap of reusable slots. Its capacity covers every slot, so removal never allocates.
    std::vector<std::size_t> free_slots_;
    // Objects with renderers in the order they were added. A removed renderer leaves a null Id
    // until compact_instances(), which instances() runs first and removal runs once half are null.
    mutable std::vector<Id> active_;
    mutable std::size_t removed_instances_{};
    std::unordered_map<std::type_index, ComponentList> component_types_;
    ComponentList scheduled_;
    // Working lists of update_transform, kept between calls, so moving objects allocates only when
    // a move affects more objects or palette matrices than any before it.
    std::vector<PosedObject> posed_objects_;
    std::vector<Mat4> posed_palettes_;
    std::vector<RenderBounds> posed_bounds_;
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
/// moves descendants and renderers at once, keeping their animation poses. Its work is proportional
/// to that subtree, and the scene keeps its working storage, so a change allocates only when it
/// affects more objects, palette matrices or mesh primitives than any earlier one.
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
    ///
    /// The work is proportional to this object's subtree, whatever the depth or child count of
    /// either parent.
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
    /// Adds a renderer for @p mesh with its rest pose, authored material factors, the renderer and
    /// every primitive visible, and shadow casting on. Throws `std::invalid_argument` for a null mesh and
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
    /// factors to authored values, custom materials and placements to none, the visibility range to
    /// every distance, the renderer and every primitive to visible, and shadow casting on. Throws
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
    /// Draws one mesh material's primitives with a custom material, or null for the mesh's own; see
    /// Scene::set_custom_material.
    void set_custom_material(std::size_t material, std::shared_ptr<const CustomMaterial> custom);
    /// Shows or hides one primitive; see Scene::set_primitive_visible.
    void set_primitive_visible(std::size_t primitive, bool visible);
    /// Sets whether the renderer casts shadows; see Scene::set_casts_shadows.
    void set_casts_shadows(bool casts);
    /// Copies drawn in place of the one at the object, or null; see Scene::set_placements.
    [[nodiscard]] std::shared_ptr<const MeshPlacements> placements() const;
    /// Draws a copy at each placement, or one copy at the object for null; see Scene::set_placements.
    void set_placements(std::shared_ptr<const MeshPlacements> placements);
    /// Distances at which the renderer draws; see Scene::set_visibility_range.
    [[nodiscard]] VisibilityRange visibility_range() const;
    /// Draws only at the distances @p range allows; see Scene::set_visibility_range.
    void set_visibility_range(const VisibilityRange &range);
    /// Union of all primitive bounds in world space, including hidden primitives and every copy.
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
