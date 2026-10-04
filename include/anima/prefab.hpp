#pragma once
#include <anima/assets/staging.hpp>
#include <anima/scene.hpp>
#include <functional>

/// @file
/// Prefabs and the strict JSON documents for prefabs and scenes. Part of the `anima::assets` target.
///
/// Documents are JSON of at most 16 MiB, nested at most 16 deep, with exactly the fields listed for
/// their kind, every one required; writers write every field. Repeated fields (compared after
/// unescaping) are rejected while parsing; then other versions or kinds, before the other fields;
/// then missing and unknown fields.
/// ObjectKey values are written as canonical decimal strings. A mesh is stored
/// as an application-owned key of 1 to 4,096 bytes: MeshName names each distinct mesh once per call
/// and two meshes cannot share a key, and MeshResolver runs once per distinct key. A custom material
/// is stored as its CustomMaterial::name, never as shader code: two different materials cannot share
/// a name in one document, and CustomMaterialResolver runs once per distinct name. File access
/// belongs to the caller.
///
/// Readers throw `std::invalid_argument` for invalid content, including malformed JSON and values
/// of the wrong JSON type, and writers throw it for a string that is not valid UTF-8.
/// Instantiation and loading create every native object before any component decoder runs, and a
/// failure destroys every object they created; side effects of application callbacks are not
/// undone, and keys allocated by a failed instantiation are not reused. Instantiated and loaded
/// components receive their first `on_enable()` at the next lifecycle reconciliation.
///
/// Object keys are checked, and object links mapped, through ordered containers, so constructing,
/// reading, capturing and instantiating prefabs, and writing and loading scenes, take time
/// O(n log n) in their n objects, apart from mesh, codec and parsing work and the costs that Scene
/// states.
///
/// Loading a scene document has two parts. stage_scene parses and validates it and resolves its meshes and
/// custom materials, and may run on any thread; load_scene(const StagedScene &, const ComponentCodecs &) and
/// SceneSet create its objects and decode its components on the thread that owns the scene. The overloads that
/// take a document run both parts on the calling thread. Readers and writers get the C locale's number
/// separators from `localeconv()`, under one lock that they all share, and readers convert numbers with
/// `strtod`, so the locale must not change while any of them runs. glibc's `localeconv()` rewrites one static
/// buffer on every call, so on glibc no code outside Anima may call it while documents are read or written on
/// another thread.

namespace anima {
namespace detail {
struct SceneStage;
} // namespace detail
/// Returns the application's stable key for a mesh when writing a document.
using MeshName = std::function<std::string(const std::shared_ptr<const Mesh> &)>;
/// Returns the mesh for a key when reading a document; returning null rejects the document.
using MeshResolver = std::function<std::shared_ptr<const Mesh>(std::string_view)>;
/// Returns the custom material that the application registers under a name when reading a document.
/// Returning null, or a material with another name, rejects the document, as does an empty resolver
/// for a document that names any custom material.
using CustomMaterialResolver = std::function<std::shared_ptr<const CustomMaterial>(std::string_view)>;
class Prefab;
/// Returns the immutable prefab for an application-owned resource key; returning null rejects the
/// operation.
using PrefabResolver = std::function<std::shared_ptr<const Prefab>(std::string_view)>;

/// Renderer of an authored object (Prefab::Node::renderer), or the one that a prefab variant puts in
/// its place (PrefabVariant::Override::renderer). Copies share the mesh, custom materials and
/// placements, which are immutable.
///
/// A null #mesh means no renderer, and then every other field keeps its default: #pose empty,
/// #placements null, every array empty, #visible and #casts_shadows true and #visibility_range the
/// default. The constructors of Prefab and PrefabVariant check these rules and the counts that the
/// mesh sets, and throw `std::invalid_argument`, each with its own messages.
struct RendererState {
    /// Shared mesh, or null for no renderer.
    std::shared_ptr<const Mesh> mesh;
    /// Initial Pose::world matrices, one per mesh node; empty uses the mesh's rest pose.
    std::optional<Pose> pose;
    /// Renderer visibility.
    bool visible = true;
    /// One factor per mesh material, each channel in [0, 1]; empty keeps the authored factors.
    std::vector<Vec3> material_factors;
    /// One custom material per mesh material, each null to keep the mesh's Material
    /// (Scene::set_custom_material); empty keeps every Material. Prefab::capture leaves it empty
    /// when no slot has a custom material.
    std::vector<std::shared_ptr<const CustomMaterial>> custom_materials;
    /// One flag per mesh primitive; empty shows every primitive.
    std::vector<bool> primitive_visible;
    /// Whether the renderer casts shadows (Scene::Instance::casts_shadows).
    bool casts_shadows = true;
    /// Copies drawn in place of the one at the object (Scene::set_placements), or null; they must
    /// copy #mesh, and #pose must be empty.
    std::shared_ptr<const MeshPlacements> placements;
    /// Distances at which the renderer draws (Scene::set_visibility_range).
    VisibilityRange visibility_range;
    /// Compares every field: #mesh, each custom material and #placements by address, the others by
    /// value with `float` `==`, so `-0` equals `0` and a NaN equals nothing.
    bool operator==(const RendererState &) const = default;
};

/// Immutable description of an object hierarchy, instantiated as independent copies.
///
/// A prefab owns its nodes and a copy of the ComponentCodecs it was given; ordinary instantiation
/// uses that copy's bindings, such as a physics world, mixer or UI host, even in another scene, so
/// whatever its callbacks capture must remain usable. Instances share meshes but own their objects
/// and components. Each instance allocates fresh keys in its scene and maps the authored keys to
/// its new objects, so object links, including forward, cyclic and self links, stay within that
/// instance. Instantiation may run from component hooks.
class Prefab {
  public:
    /// One authored object.
    struct Node {
        std::string name;
        /// Index of an earlier node; empty only for the root, which is node 0.
        std::optional<std::size_t> parent;
        /// Matrix relative to the parent; the root's is multiplied by the instantiation placement.
        Mat4 local = identity();
        /// Renderer created with the object; the default, without a mesh, creates none.
        RendererState renderer;
        /// Encoded components, at most 1,024, each type at most once.
        std::vector<ComponentData> components;
        /// Authored activation (GameObject::active_self), independent of the parent.
        bool active = true;
        /// Identity within the prefab; a zero key receives the lowest unused key at construction.
        ObjectKey key;
    };
    /// Validates @p nodes and keeps them with a copy of @p codecs.
    ///
    /// Requires 1 to 65,536 nodes, parents that precede their children, a single root at index 0,
    /// unique explicit keys, component types registered in @p codecs, and matrices and renderer data
    /// that a trial instantiation without components accepts; no component constructor or codec
    /// callback runs. Throws `std::invalid_argument` for invalid nodes.
    explicit Prefab(std::vector<Node> nodes, ComponentCodecs codecs = {});
    /// Captures @p root and its descendants, keeping @p root's local matrix and every object's key,
    /// activation, renderer state and set pose, and encodes their components with @p codecs, which
    /// the prefab keeps. Throws `std::invalid_argument` when a component has no codec or links
    /// outside the captured subtree.
    static Prefab capture(GameObject root, const ComponentCodecs &codecs = {});
    /// Nodes in hierarchy order, with keys assigned.
    [[nodiscard]] std::span<const Node> nodes() const { return nodes_; }
    /// Instantiates the prefab as a new root of @p scene, whose local matrix, and so its world
    /// matrix, is @p placement times the authored root matrix, restores its components with the
    /// retained codecs and returns the new root. Every component type is checked before any object
    /// is created.
    [[nodiscard]] GameObject instantiate(Scene &scene, const Mat4 &placement = identity()) const;
    /// Instantiates the prefab as instantiate(Scene &, const Mat4 &) does, but with the new root as
    /// the last child of @p parent. @p placement is relative to @p parent, not a world pose: the new
    /// root's local matrix is @p placement times the authored root matrix, so its world matrix is
    /// @p parent's world matrix times that.
    [[nodiscard]] GameObject instantiate(GameObject parent, const Mat4 &placement = identity()) const;
    /// Instantiates the prefab with @p codecs, borrowed for this call, in place of the retained
    /// codecs, which stay unchanged. Use it to bind an instance to another session's services.
    [[nodiscard]] GameObject instantiate(Scene &scene, const Mat4 &placement, const ComponentCodecs &codecs) const;
    /// Instantiates the prefab under @p parent, placed relative to it as
    /// instantiate(GameObject, const Mat4 &) places it, with @p codecs, borrowed for this call.
    [[nodiscard]] GameObject instantiate(GameObject parent, const Mat4 &placement, const ComponentCodecs &codecs) const;
    /// Writes an `anima.prefab` version 4 document: exactly `version`, `kind` and `objects`, with
    /// objects as in serialize_scene and the root first. No codec runs.
    [[nodiscard]] std::string serialize(const MeshName &name) const;
    /// Reads an `anima.prefab` version 4 document, in which every object key is explicit, resolving
    /// custom material names through @p materials, and constructs a prefab that keeps @p codecs.
    ///
    /// Calls may run concurrently on any thread. Each reads @p document and the C locale, neither of which
    /// may change during the call, takes the locale lock that the file comment describes, and runs
    /// @p resolve and @p materials on the calling thread, once per distinct mesh key and custom material
    /// name. No callback of @p codecs runs; several threads may copy one registry into @p codecs at once
    /// while it does not change. The trial instantiation uses a private scene, whose construction shares only
    /// an atomic counter with other scenes.
    static Prefab deserialize(std::string_view document, const MeshResolver &resolve, ComponentCodecs codecs = {},
                              const CustomMaterialResolver &materials = {});

  private:
    std::vector<Node> nodes_;
    ComponentCodecs codecs_;
    GameObject create(Scene &scene, const GameObject *parent, const Mat4 &placement,
                      const ComponentCodecs &codecs) const;
};

/// Writes every object of @p scene as an `anima.scene` version 4 document, borrowing @p codecs for
/// this call.
///
/// Components encode object links through one ObjectReferences covering the whole scene, so links
/// between roots persist and links outside the scene are rejected. Lifecycle notification state,
/// runtime ids and GPU residency are not stored. The document has exactly `version` (`4`), `kind`
/// (`"anima.scene"`), `next_key` (the key the scene allocates next, `"0"` once exhausted) and
/// `objects`: at most 65,536, each after its parent, with exactly these fields, the fields of
/// Prefab::Node and its RendererState:
/// - `key`: nonzero and unique;
/// - `name`;
/// - `parent`: null, or the index of an earlier object;
/// - `local`: 16 finite numbers, column-major;
/// - `mesh`: null or a mesh key;
/// - `pose`: null for the rest pose, or one 16-number Pose::world matrix per mesh node;
/// - `visible`, `active` and `casts_shadows`: booleans;
/// - `material_factors`: empty, to keep the authored factors, or one `[r, g, b]` per mesh material, each in [0, 1];
/// - `custom_materials`: empty, to keep every Material, or one entry per mesh material, each null or a custom
///   material name of 1 to 4,096 bytes;
/// - `primitive_visible`: empty, to show every primitive, or one boolean per mesh primitive;
/// - `placements`: null, or 1 to 1,048,576 placements (MeshPlacements), each 16 numbers of an
///   affine matrix, column-major, relative to the object, for an object with a mesh and a null `pose`. Written one
///   number to a line, as serialization indents the document, a placement takes about 380 bytes when it rotates
///   and scales and about 260 when it only translates by whole numbers, so the document byte limit holds about
///   44,000 of the first or 63,000 of the second. Writing keeps MeshPlacements::transforms() order, which reading
///   keeps. Objects that share one MeshPlacements are each written in full and read back with separate sets;
/// - `visibility_range`: null, to draw at every distance, or an object with exactly `begin`, `end` (null for
///   no end), `begin_margin` and `end_margin` that validate_visibility_range() accepts, so an object with a null
///   `end` has an `end_margin` of 0 (VisibilityRange);
/// - `components`: at most 1,024 objects with exactly `type`, `state` and `enabled` (ComponentData).
///
/// An object without a mesh has null `pose`, `placements` and `visibility_range`, empty arrays, and `visible` and
/// `casts_shadows` true. Throws `std::invalid_argument` when a component has no codec, a link leaves the scene, mesh
/// naming fails, two different custom materials share a name or a limit is exceeded.
[[nodiscard]] std::string serialize_scene(Scene &scene, const MeshName &name, const ComponentCodecs &codecs = {});
/// An `anima.scene` document that stage_scene parsed, validated and resolved, for
/// load_scene(const StagedScene &, const ComponentCodecs &) and SceneSet to commit on the thread that owns
/// the scene.
///
/// It holds the document's objects and `next_key`, with component payloads still encoded, and shares the
/// meshes and custom materials that the resolvers returned; it references no Scene, SceneSet,
/// ComponentCodecs or service. What it holds never changes, so any thread may copy, read or destroy it, and
/// it can be committed any number of times; as with `std::shared_ptr`, only assigning to one object while
/// another thread uses that same object needs synchronization. Copies share its data, and moving it copies
/// it, so no object is ever empty.
class StagedScene {
  public:
    StagedScene(const StagedScene &) = default;
    StagedScene &operator=(const StagedScene &) = default;
    /// Bytes of decoded data held, excluding container overhead and the meshes and custom materials, which
    /// are shared: for each object, `sizeof(Prefab::Node)`, which includes its RendererState, the bytes of its
    /// name, `sizeof(Mat4)` per pose matrix, `sizeof(Vec3)` per material factor,
    /// `sizeof(std::shared_ptr<const CustomMaterial>)` per custom material slot, null or not, one byte per eight
    /// primitive visibility flags, rounded up, for each component `sizeof(ComponentData)` and the bytes of its
    /// type and payload, and for the MeshPlacements that staging built from its placements,
    /// `sizeof(MeshPlacements)`, `sizeof(Mat4)` per placement, `sizeof(MeshPlacements::Cluster)` per cluster and
    /// `sizeof(RenderBounds)` per mesh draw. Copies share these bytes.
    [[nodiscard]] std::size_t retained_bytes() const noexcept;

  private:
    friend struct detail::ScenePersistence;
    explicit StagedScene(std::shared_ptr<const detail::SceneStage> data) : data_(std::move(data)) {}
    std::shared_ptr<const detail::SceneStage> data_;
};
/// Parses and validates an `anima.scene` version 4 document, as load_scene does, and resolves each distinct
/// mesh key and custom material name once, without creating a scene or decoding any component.
///
/// Calls may run concurrently on any thread. Each reads @p document and the C locale, neither of which may
/// change during the call, takes the locale lock that the file comment describes, shares no other mutable
/// state than the StagingProgress in @p options, and runs no ComponentCodecs callback. @p resolve and
/// @p materials run on the calling thread, once per distinct mesh key and custom material name, so they
/// must allow calls from every thread that stages with them. @p options is checked before the parse, before
/// each object and each @p resolve or @p materials call, and before the call returns; its steps are the
/// parse, each object, each distinct mesh key and each distinct custom material name.
///
/// Throws `std::invalid_argument` for invalid content, with the messages that load_scene throws for it, what
/// @p resolve or @p materials throws, and StagingCancelled when @p options reports a stop. The checks that need
/// a scene or codecs run when the document is committed: component types, against the codecs given then;
/// local and pose matrices that a scene rejects; and material factors, custom materials and primitive
/// visibility flags that do not match their mesh.
[[nodiscard]] StagedScene stage_scene(std::string_view document, const MeshResolver &resolve,
                                      const CustomMaterialResolver &materials = {}, const StagingOptions &options = {});
/// Builds a new scene from @p staged, keeping its object keys and `next_key`. @p codecs is borrowed for this
/// call and must register every component type, which is checked before any object is created. The caller
/// decides when to use the new scene. On failure no scene remains; side effects of decoders are not undone.
[[nodiscard]] std::shared_ptr<Scene> load_scene(const StagedScene &staged, const ComponentCodecs &codecs = {});
/// Builds a new scene from an `anima.scene` version 4 document, keeping its object keys and
/// `next_key`, which must be `"0"` or greater than every object key. @p codecs is borrowed for this
/// call and must register every component type, and @p materials resolves custom material names.
/// The caller decides when to use the new scene.
///
/// Equivalent to load_scene(stage_scene(document, resolve, materials), codecs) on the calling thread.
[[nodiscard]] std::shared_ptr<Scene> load_scene(std::string_view document, const MeshResolver &resolve,
                                                const ComponentCodecs &codecs = {},
                                                const CustomMaterialResolver &materials = {});
} // namespace anima
