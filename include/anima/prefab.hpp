#pragma once
#include <anima/scene.hpp>
#include <functional>

/// @file
/// Prefabs and the strict JSON documents for prefabs and scenes. Part of the `anima::assets` target.
///
/// Documents are JSON of at most 16 MiB, nested at most 16 deep, with exactly the fields listed for
/// their kind: missing, unknown or repeated fields (compared after unescaping) and other versions or
/// kinds are rejected. ObjectKey values are written as canonical decimal strings. A mesh is stored
/// as an application-owned key of 1 to 4,096 bytes: MeshName names each distinct mesh once per call
/// and two meshes cannot share a key, and MeshResolver runs once per distinct key. File access
/// belongs to the caller.
///
/// Readers throw `std::invalid_argument` for invalid content, including malformed JSON and values
/// of the wrong JSON type, and writers throw it for a string that is not valid UTF-8.
/// Instantiation and loading create every native object before any component decoder runs, and a
/// failure destroys every object they created; side effects of application callbacks are not
/// undone, and keys allocated by a failed instantiation are not reused. Instantiated and loaded
/// components receive their first `on_enable()` at the next lifecycle reconciliation.

namespace anima {
/// Returns the application's stable key for a mesh when writing a document.
using MeshName = std::function<std::string(const std::shared_ptr<const Mesh> &)>;
/// Returns the mesh for a key when reading a document; returning null rejects the document.
using MeshResolver = std::function<std::shared_ptr<const Mesh>(std::string_view)>;
class Prefab;
/// Returns the immutable prefab for an application-owned resource key; returning null rejects the
/// operation.
using PrefabResolver = std::function<std::shared_ptr<const Prefab>(std::string_view)>;

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
        /// Optional shared mesh; without one, #pose, #material_factors and #primitive_visible must be
        /// empty and #visible true.
        std::shared_ptr<const Mesh> mesh;
        /// Initial Pose::world matrices, one per mesh node; empty uses the mesh's rest pose.
        std::optional<Pose> pose;
        /// Renderer visibility.
        bool visible = true;
        /// One factor per mesh material, each channel in [0, 1]; empty keeps the authored factors.
        std::vector<Vec3> material_factors;
        /// One flag per mesh primitive; empty shows every primitive.
        std::vector<bool> primitive_visible;
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
    /// Instantiates the prefab as a new root of @p scene, with @p placement multiplied by the
    /// authored root matrix, restores its components with the retained codecs and returns the new
    /// root. Every component type is checked before any object is created.
    [[nodiscard]] GameObject instantiate(Scene &scene, const Mat4 &placement = identity()) const;
    /// Instantiates the prefab under @p parent, as instantiate(Scene &, const Mat4 &) does.
    [[nodiscard]] GameObject instantiate(GameObject parent, const Mat4 &placement = identity()) const;
    /// Instantiates the prefab with @p codecs, borrowed for this call, in place of the retained
    /// codecs, which stay unchanged. Use it to bind an instance to another session's services.
    [[nodiscard]] GameObject instantiate(Scene &scene, const Mat4 &placement, const ComponentCodecs &codecs) const;
    /// Instantiates the prefab under @p parent with @p codecs, borrowed for this call.
    [[nodiscard]] GameObject instantiate(GameObject parent, const Mat4 &placement, const ComponentCodecs &codecs) const;
    /// Writes an `anima.prefab` version 3 document: exactly `version`, `kind` and `objects`, with
    /// objects as in serialize_scene and the root first. No codec runs.
    [[nodiscard]] std::string serialize(const MeshName &name) const;
    /// Reads an `anima.prefab` version 3 document, in which every object key is explicit, and
    /// constructs a prefab that keeps @p codecs.
    static Prefab deserialize(std::string_view document, const MeshResolver &resolve, ComponentCodecs codecs = {});

  private:
    std::vector<Node> nodes_;
    ComponentCodecs codecs_;
    GameObject create(Scene &scene, const GameObject *parent, const Mat4 &placement,
                      const ComponentCodecs &codecs) const;
};

/// Writes every object of @p scene as an `anima.scene` version 3 document, borrowing @p codecs for
/// this call.
///
/// Components encode object links through one ObjectReferences covering the whole scene, so links
/// between roots persist and links outside the scene are rejected. Lifecycle notification state,
/// runtime ids and GPU residency are not stored. The document has exactly `version` (`3`), `kind`
/// (`"anima.scene"`), `next_key` (the key the scene allocates next, `"0"` once exhausted) and
/// `objects`: at most 65,536, each after its parent, with exactly these fields:
/// - `key`: nonzero and unique;
/// - `name`;
/// - `parent`: null, or the index of an earlier object;
/// - `local`: 16 finite numbers, column-major;
/// - `mesh`: null or a mesh key;
/// - `pose`: null or one 16-number Pose::world matrix per mesh node;
/// - `visible` and `active`: booleans;
/// - `material_factors`: empty or one `[r, g, b]` per mesh material, each in [0, 1];
/// - `primitive_visible`: empty or one boolean per mesh primitive;
/// - `components`: at most 1,024 objects with exactly `type`, `state` and `enabled`
///   (ComponentData).
///
/// An object without a mesh has a null `pose`, empty arrays and `visible` true. Throws
/// `std::invalid_argument` when a component has no codec, a link leaves the scene, mesh naming
/// fails or a limit is exceeded.
[[nodiscard]] std::string serialize_scene(Scene &scene, const MeshName &name, const ComponentCodecs &codecs = {});
/// Builds a new scene from an `anima.scene` version 3 document, keeping its object keys and
/// `next_key`, which must be `"0"` or greater than every object key. @p codecs is borrowed for this
/// call and must register every component type. The caller decides when to use the new scene.
[[nodiscard]] std::shared_ptr<Scene> load_scene(std::string_view document, const MeshResolver &resolve,
                                                const ComponentCodecs &codecs = {});
} // namespace anima
