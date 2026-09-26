#pragma once
#include <anima/prefab.hpp>

/// @file
/// Additive ownership of independently loaded scenes. Part of the `anima::assets` target.
///
/// Membership changes and active selection are synchronous and happen between updates and draws.
/// They throw `std::logic_error` when nested, or when made from a loader, decoder, component hook
/// or constructor, or while a member scene is held by a driver. Unloading, replacing, clearing or
/// restoring invalidates every handle to the old scenes, their objects and their components before
/// any component cleanup runs. Old handles never rebind to a replacement.

namespace anima {
namespace detail {
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
class SceneSet {
  public:
    SceneSet() = default;
    /// Invalidates every handle to every member before any component cleanup, then releases the
    /// members.
    ~SceneSet();
    SceneSet(const SceneSet &) = delete;
    SceneSet &operator=(const SceneSet &) = delete;
    /// Adds an empty scene under namespace @p key. The first member of an empty set becomes
    /// active(). Throws `std::invalid_argument` for an invalid or duplicate namespace.
    [[nodiscard]] SceneRef create(std::string key);
    /// Adds a scene loaded from @p document, as load_scene does, under namespace @p key. On failure
    /// the set is unchanged. Throws as create() and load_scene do.
    [[nodiscard]] SceneRef load(std::string key, std::string_view document, const MeshResolver &resolve,
                                const ComponentCodecs &codecs = {});
    /// Replaces @p target with a scene loaded from @p document, keeping its namespace, position and
    /// selection, and returns the new member's handle.
    ///
    /// The replacement is fully loaded before the old scene is retired, so both exist meanwhile,
    /// with their bodies, voices and documents. On failure the set and the old scene are unchanged.
    /// Throws `std::out_of_range` for an expired @p target, `std::invalid_argument` for one from
    /// another set, and as load_scene does.
    [[nodiscard]] SceneRef replace(SceneRef target, std::string_view document, const MeshResolver &resolve,
                                   const ComponentCodecs &codecs = {});
    /// Writes every member into one `anima.scene-set` version 1 JSON document, with the rules of
    /// serialize_scene and @p codecs borrowed for this call.
    ///
    /// Components encode object links through one ObjectReferences covering the whole set, so links
    /// between members persist; a link to a stale object or one outside the set is rejected. The
    /// document has exactly `version` (`1`), `kind`, `active` (a namespace, or null exactly when the
    /// set is empty), `scenes` and `references`. Each scene has exactly `key` (its namespace),
    /// `next_key` and `objects`, as in serialize_scene. Each reference row has exactly `key` (a
    /// document-wide key, numbered from 1), `scene` and `object` (the key within that scene), and
    /// the rows map every object exactly once. Linked payloads store document-wide keys, so a
    /// member's `objects` cannot be loaded as a scene document. Limits are 16 MiB, 1,024 scenes and
    /// 65,536 objects in total, and no two meshes in the set may share a key. Throws
    /// `std::logic_error` unless the set and its members are idle.
    [[nodiscard]] std::string serialize(const MeshName &name, const ComponentCodecs &codecs = {});
    /// Replaces the whole membership and selection with those of an `anima.scene-set` document,
    /// borrowing @p codecs for this call.
    ///
    /// Every member and native object is staged before components decode through one set-wide
    /// ObjectReferences, so links may point to later members. Each mesh key resolves once. On
    /// failure the set and all handles are unchanged. On success the new members are published,
    /// then every old handle is invalidated before the old components are cleaned up; both sets
    /// exist meanwhile. Renderers keep the old, now empty scenes until given render_scenes() again.
    /// Throws `std::invalid_argument` for invalid content, as load_scene does.
    void restore(std::string_view document, const MeshResolver &resolve, const ComponentCodecs &codecs = {});
    /// Removes @p scene, then releases it. If it was active(), the first remaining member becomes
    /// active. Throws `std::out_of_range` for an expired handle and `std::invalid_argument` for one
    /// from another set.
    void unload(SceneRef scene);
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
    /// `std::logic_error` during membership changes.
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
    std::size_t index(SceneRef scene) const;
    void check_key(std::string_view key) const;
    SceneRef append(std::string key, std::shared_ptr<Scene> scene);
    void retire_all() noexcept;
    void run_components(double seconds, bool fixed, bool lifecycle_only = false);
    std::vector<std::shared_ptr<detail::SceneRecord>> scenes_;
    std::weak_ptr<detail::SceneRecord> active_;
    bool mutating_{};
};
} // namespace anima
