#pragma once
#include <anima/prefab.hpp>

/// @file
/// Additive ownership of independently loaded scenes. Part of the `anima::assets` target.
///
/// Membership changes and active selection are synchronous and happen between updates and draws, on the
/// thread that owns the set. A document can be staged beforehand on any thread with stage_scene or
/// stage_scene_set, and the overloads that take the staged result then only commit it. Membership changes
/// throw `std::logic_error` while the set is busy, as SceneSet describes. Unloading,
/// replacing, clearing or restoring invalidates every handle to the old scenes, their objects and
/// their components before any component cleanup runs. Old handles never rebind to a replacement;
/// instead, SceneSet::replace and SceneSet::unload overwrite the links that surviving members'
/// components report through their codecs (see ObjectLinks).

namespace anima {
namespace detail {
struct SceneSetStage;
struct SceneRecord {
    std::string key;
    std::shared_ptr<Scene> scene;
    bool attached{};
};
} // namespace detail

/// Address of an object across a SceneSet: a scene namespace plus a key local to that scene.
///
/// Addresses do not keep targets alive and are resolved on each SceneSet::find call, so after a
/// namespace is loaded again an address may select an object of the new scene.
struct SceneAddress {
    /// Namespace of the scene.
    std::string scene;
    /// Key of the object within that scene.
    ObjectKey object;
    bool operator==(const SceneAddress &) const = default;
};

/// A component link that SceneSet::unload set to null because it named an object of the unloaded
/// scene.
struct ClearedLink {
    /// Object of a remaining member whose component held the link.
    GameObject owner;
    /// Codec key of that component's type, as ComponentCodecs::add registered it.
    std::string component;
    /// Former target, whose namespace is the unloaded scene's.
    SceneAddress target;
};

/// An `anima.scene-set` document that stage_scene_set parsed, validated and resolved, for SceneSet::restore
/// and SceneSet::replace to commit on the thread that owns the set.
///
/// It holds the document's members, active namespace and reference table, with component payloads still
/// encoded, and shares the meshes and custom materials that the resolvers returned; it references no Scene,
/// SceneSet, ComponentCodecs or service. What it holds never changes, so any thread may copy, read or
/// destroy it, and it can be committed any number of times; as with `std::shared_ptr`, only assigning to one
/// object while another thread uses that same object needs synchronization. Copies share its data, and
/// moving it copies it, so no object is ever empty.
class StagedSceneSet {
  public:
    StagedSceneSet(const StagedSceneSet &) = default;
    StagedSceneSet &operator=(const StagedSceneSet &) = default;
    /// Bytes of decoded data held, excluding container overhead and the shared meshes and custom materials:
    /// the bytes of each member's namespace and of its objects, counted as StagedScene::retained_bytes counts
    /// them, and `2 * sizeof(ObjectKey) + sizeof(std::size_t)` per reference row. Copies share these bytes.
    [[nodiscard]] std::size_t retained_bytes() const noexcept;

  private:
    friend struct detail::ScenePersistence;
    explicit StagedSceneSet(std::shared_ptr<const detail::SceneSetStage> data) : data_(std::move(data)) {}
    std::shared_ptr<const detail::SceneSetStage> data_;
};
/// Parses and validates an `anima.scene-set` version 2 document, as SceneSet::restore does, and resolves each
/// distinct mesh key and custom material name of the whole document once, without creating a scene or decoding
/// any component.
///
/// Calls may run concurrently on any thread, with the sharing, callbacks, cancellation and steps that
/// stage_scene documents. Throws `std::invalid_argument` for invalid content, with the messages that
/// SceneSet::restore throws for it, what @p resolve or @p materials throws, and StagingCancelled when
/// @p options reports a stop. The checks that need a scene run when the document is committed, as for
/// stage_scene.
[[nodiscard]] StagedSceneSet stage_scene_set(std::string_view document, const MeshResolver &resolve,
                                             const CustomMaterialResolver &materials = {},
                                             const StagingOptions &options = {});

/// Checked, weak identity of one member scene of a SceneSet.
///
/// Unloading or replacing the scene, or clearing, restoring or destroying the set, invalidates every
/// copy; any access except valid() and `operator bool` then throws `std::out_of_range`.
class SceneRef {
  public:
    SceneRef() = default;
    /// Whether the scene is still a member of its set.
    [[nodiscard]] bool valid() const noexcept;
    explicit operator bool() const noexcept { return valid(); }
    /// Namespace of the scene.
    [[nodiscard]] std::string key() const;
    /// Borrowed scene for explicit drivers; do not keep the reference across membership changes.
    [[nodiscard]] Scene &get() const;
    /// Keeps the scene's storage alive until the end of the full expression.
    class Access {
      public:
        explicit Access(std::shared_ptr<detail::SceneRecord> record) : record_(std::move(record)) {}
        Scene *operator->() const { return record_->scene.get(); }

      private:
        std::shared_ptr<detail::SceneRecord> record_;
    };
    /// Member access that pins the scene's storage through the full expression.
    Access operator->() const { return Access(lock()); }
    /// Read-only shared scene for a renderer. Holding it does not delay unloading: once the scene
    /// leaves the set, the scene it points to is permanently empty, and it never follows a
    /// replacement.
    [[nodiscard]] std::shared_ptr<const Scene> render_scene() const;

  private:
    friend class SceneSet;
    explicit SceneRef(const std::shared_ptr<detail::SceneRecord> &record) : record_(record) {}
    std::shared_ptr<detail::SceneRecord> lock() const;
    std::weak_ptr<detail::SceneRecord> record_;
};

/// Owner of several independently loaded scenes, each under a unique namespace.
///
/// A namespace is a nonempty string of at most 4,096 bytes without NUL; it is application data, not
/// a path. Members stay in insertion order, and objects in different members cannot be parented to
/// each other. There is no global scene, file access, worker thread or graphics dependency. The
/// set must outlive calls into its scenes. active() is only a caller default: no scene driver or
/// view_matrix consults it.
///
/// The set is busy while it updates its members, changes membership (in create, load, replace,
/// restore, unload, clear or its destructor), serializes, or is held by a scene driver that runs
/// callbacks, and while a member scene runs component hooks or cleanup, constructs a component or
/// is held by a scene driver of its own. Only application callbacks run meanwhile, so only they
/// can observe it. While the set is busy, its membership changes, set_active(), update(),
/// fixed_update(), synchronize_lifecycle(), serialize() and every scene driver given the set throw
/// `std::logic_error`, whose message names what the set is doing, or that a member scene is
/// running callbacks.
class SceneSet {
  public:
    SceneSet() = default;
    /// Invalidates every handle to every member before any component cleanup, then releases the
    /// members.
    ///
    /// Destroying the set while it updates its members, changes membership, serializes or is held
    /// by a scene driver writes a diagnostic to `stderr` and terminates the program, as
    /// Scene::~Scene() does.
    ~SceneSet();
    SceneSet(const SceneSet &) = delete;
    SceneSet &operator=(const SceneSet &) = delete;
    /// Adds an empty scene under namespace @p key. The first member of an empty set becomes
    /// active(). Throws `std::invalid_argument` for an invalid or duplicate namespace.
    [[nodiscard]] SceneRef create(std::string key);
    /// Adds a scene built from @p staged, as load_scene(const StagedScene &, const ComponentCodecs &) builds
    /// it, under namespace @p key. On failure the set is unchanged. Throws as create() and load_scene do.
    [[nodiscard]] SceneRef load(std::string key, const StagedScene &staged, const ComponentCodecs &codecs = {});
    /// Adds a scene loaded from @p document, as load_scene does, under namespace @p key. On failure
    /// the set is unchanged. Throws as create() and load_scene do. After checking @p key, it stages
    /// @p document with stage_scene on the calling thread and commits it as
    /// load(std::string, const StagedScene &, const ComponentCodecs &) does.
    [[nodiscard]] SceneRef load(std::string key, std::string_view document, const MeshResolver &resolve,
                                const ComponentCodecs &codecs = {}, const CustomMaterialResolver &materials = {});
    /// Replaces @p target with a scene loaded from @p document, keeping its namespace, position and
    /// selection and the links between it and the other members, and returns the new member's
    /// handle. @p codecs is borrowed for this call.
    ///
    /// @p document is either an `anima.scene` document, loaded as load_scene loads it, or an
    /// `anima.scene-set` document such as serialize() writes. A set document is validated as
    /// restore() validates it, and each of its mesh keys and custom material names resolves once,
    /// through @p resolve and @p materials, but only its member with
    /// @p target's namespace is loaded; its `active` field is ignored. That member's components
    /// decode through the document's reference table: rows in its own namespace map to the
    /// replacement's objects, and rows in other namespaces map to the objects that SceneSet::find
    /// returns for their addresses among the other members. A row whose address finds no object is
    /// left out, so a link to it fails to decode.
    ///
    /// Then every link that the other members' components report through link callbacks in
    /// @p codecs (see ComponentCodecs::add) and that names a live object of @p target is rebound to
    /// the replacement's object with the same ObjectKey. Links of components whose codec has no
    /// link callback, or that @p codecs lacks, expire with the old scene.
    ///
    /// The replacement is fully loaded and every link is checked before the old scene is retired,
    /// so both exist meanwhile, with their bodies, voices and documents. On failure the set, the old
    /// scene, every handle and every link are unchanged. On success the links are rebound after the
    /// replacement is published and before the old scene's handles are invalidated and its
    /// components are cleaned up.
    ///
    /// Throws `std::out_of_range` for an expired @p target. Throws `std::invalid_argument` for a
    /// @p target from another set, for invalid content as load_scene or restore() does, for a set
    /// document without @p target's namespace, and when the replacement lacks the key of a linked
    /// object. A link callback's exception propagates.
    ///
    /// After checking @p target, it stages @p document on the calling thread, as stage_scene or
    /// stage_scene_set does for its kind, and commits it as the overloads that take a staged document do.
    [[nodiscard]] SceneRef replace(SceneRef target, std::string_view document, const MeshResolver &resolve,
                                   const ComponentCodecs &codecs = {}, const CustomMaterialResolver &materials = {});
    /// Replaces @p target with a scene built from @p staged, as replace(SceneRef, std::string_view, const
    /// MeshResolver &, const ComponentCodecs &, const CustomMaterialResolver &) does for an `anima.scene`
    /// document, with the same guarantees and exceptions.
    [[nodiscard]] SceneRef replace(SceneRef target, const StagedScene &staged, const ComponentCodecs &codecs = {});
    /// Replaces @p target with the member of @p staged that has its namespace, as replace(SceneRef,
    /// std::string_view, const MeshResolver &, const ComponentCodecs &, const CustomMaterialResolver &) does
    /// for an `anima.scene-set` document, with the same guarantees and exceptions. Every member's component
    /// types are checked against @p codecs before any object is created.
    [[nodiscard]] SceneRef replace(SceneRef target, const StagedSceneSet &staged, const ComponentCodecs &codecs = {});
    /// Writes every member into one `anima.scene-set` version 2 JSON document, with the rules of
    /// serialize_scene and @p codecs borrowed for this call.
    ///
    /// Components encode object links through one ObjectReferences covering the whole set, so links
    /// between members persist; a link to a stale object or one outside the set is rejected. The
    /// document has exactly `version` (`2`), `kind`, `active` (a namespace, or null exactly when the
    /// set is empty), `scenes` and `references`. Each scene has exactly `key` (its namespace),
    /// `next_key` and `objects`, as in serialize_scene. Each reference row has exactly `key` (a
    /// document-wide key, numbered from 1), `scene` and `object` (the key within that scene), and
    /// the rows map every object exactly once. Linked payloads store document-wide keys, so a
    /// member's `objects` cannot be loaded as a scene document. Limits are 16 MiB, 1,024 scenes and
    /// 65,536 objects in total, and no two meshes in the set may share a key, nor two custom
    /// materials a name. Throws `std::logic_error` while the set is busy.
    [[nodiscard]] std::string serialize(const MeshName &name, const ComponentCodecs &codecs = {});
    /// Replaces the whole membership and selection with those of an `anima.scene-set` document,
    /// borrowing @p codecs for this call.
    ///
    /// Every member and native object is staged before components decode through one set-wide
    /// ObjectReferences, so links may point to later members. Each mesh key resolves once through
    /// @p resolve, and each custom material name once through @p materials. On
    /// failure the set and all handles are unchanged. On success the new members are published,
    /// then every old handle is invalidated before the old components are cleaned up; both sets
    /// exist meanwhile. Renderers keep the old, now empty scenes until given render_scenes() again.
    /// Throws `std::invalid_argument` for invalid content, as load_scene does.
    ///
    /// Stages @p document with stage_scene_set on the calling thread, then commits it as
    /// restore(const StagedSceneSet &, const ComponentCodecs &) does.
    void restore(std::string_view document, const MeshResolver &resolve, const ComponentCodecs &codecs = {},
                 const CustomMaterialResolver &materials = {});
    /// Replaces the whole membership and selection with those of @p staged, as restore(std::string_view,
    /// const MeshResolver &, const ComponentCodecs &, const CustomMaterialResolver &) does, with the same
    /// guarantees. Every component type is checked against @p codecs, borrowed for this call, before any object
    /// is created. Throws `std::invalid_argument` for an unregistered component type, and what a decoder throws.
    void restore(const StagedSceneSet &staged, const ComponentCodecs &codecs = {});
    /// Removes @p scene, then releases it, and returns the links it cleared. If it was active(), the
    /// first remaining member becomes active. @p codecs is borrowed for this call.
    ///
    /// Every link that the remaining members' components report through link callbacks in
    /// @p codecs (see ComponentCodecs::add) and that names a live object of @p scene is set to a
    /// default GameObject, so it can be persisted as null, and is returned in member order; the
    /// order within a member is unspecified. Links of components whose codec has no link callback,
    /// or that @p codecs lacks, expire with the scene. The links are cleared after @p scene leaves
    /// the set and before its handles are invalidated and its components are cleaned up.
    ///
    /// Throws `std::out_of_range` for an expired handle and `std::invalid_argument` for one from
    /// another set, and a link callback's exception propagates; on failure the set, every handle and
    /// every link are unchanged.
    std::vector<ClearedLink> unload(SceneRef scene, const ComponentCodecs &codecs = {});
    /// Removes every member, invalidating all of them before any component cleanup.
    void clear();
    /// Number of members.
    [[nodiscard]] std::size_t size() const noexcept { return scenes_.size(); }
    /// Handles to the members in insertion order; a replacement keeps its predecessor's position.
    [[nodiscard]] std::vector<SceneRef> scenes() const;
    /// Runs Scene::update across every member with one participant snapshot.
    ///
    /// Lifecycle is reconciled across the set, disables first; then every member's `on_update`
    /// hooks run before any `on_late_update` hook. Components added or activated in any member
    /// during the call wait for the next call. Throws as Scene::update does, and
    /// `std::logic_error` while the set is busy.
    void update(double seconds);
    /// Runs Scene::fixed_update across every member with one participant snapshot.
    void fixed_update(double seconds);
    /// Runs Scene::synchronize_lifecycle across every member with one participant snapshot.
    void synchronize_lifecycle();
    /// Every attachment of component type `T` in every member, in member order, including disabled
    /// ones and those on inactive objects.
    template <class T> [[nodiscard]] std::vector<ComponentRef<T>> components() const {
        std::vector<ComponentRef<T>> result;
        for (const auto &record : scenes_) {
            auto values = record->scene->components<T>();
            result.insert(result.end(), values.begin(), values.end());
        }
        return result;
    }
    /// Read-only pointers to the current members, for a renderer's scene selection. The snapshot
    /// never follows later changes: an unloaded or replaced member stays in it, empty.
    [[nodiscard]] std::vector<std::shared_ptr<const Scene>> render_scenes() const;
    /// Member with namespace @p key, or an invalid handle.
    [[nodiscard]] SceneRef find(std::string_view key) const noexcept;
    /// Selected member, or an invalid handle when the set is empty.
    [[nodiscard]] SceneRef active() const noexcept;
    /// Selects @p scene as active(), a caller default that does not enable, simulate or render
    /// anything. Throws `std::out_of_range` for an expired handle and `std::invalid_argument` for
    /// one from another set.
    void set_active(SceneRef scene);
    /// Address of @p object. Throws `std::invalid_argument` for a stale object or one outside the
    /// set.
    [[nodiscard]] SceneAddress address(GameObject object) const;
    /// Object at @p address, or an invalid handle when its namespace is not a member or its key is
    /// missing or null.
    [[nodiscard]] GameObject find(const SceneAddress &address) const noexcept;

  private:
    friend struct detail::SceneDriver;
    class Mutation;
    struct Link;
    // What the set itself is doing while it is busy.
    enum class Activity : unsigned char { idle, updating, changing_membership, serializing, driving };
    // Throws std::logic_error while the set is busy.
    void require_idle() const;
    std::vector<Link> links_into(const Scene &retired, const ComponentCodecs &codecs) const;
    SceneRef replace_member(std::size_t slot, std::shared_ptr<Scene> scene, const ComponentCodecs &codecs);
    void restore_members(const StagedSceneSet &staged, const ComponentCodecs &codecs);
    std::size_t index(SceneRef scene) const;
    void check_key(std::string_view key) const;
    SceneRef append(std::string key, std::shared_ptr<Scene> scene);
    void retire_all() noexcept;
    void run_components(double seconds, bool fixed, bool lifecycle_only = false);
    std::vector<std::shared_ptr<detail::SceneRecord>> scenes_;
    std::weak_ptr<detail::SceneRecord> active_;
    Activity activity_{};
};
} // namespace anima
