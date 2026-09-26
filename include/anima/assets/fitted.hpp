#pragma once
#include <anima/assets/preview.hpp>
#include <anima/scene.hpp>
#include <map>
#include <set>

/// @file
/// Fitted meshes: models skinned to a body's rig that copy the body's final pose instead of owning
/// animation.
///
/// Part of the `anima::assets` target. Catalogs are UTF-8 JSON of at most 4 MiB and 64 nesting
/// levels; duplicate and unknown fields are rejected. Failures, including JSON syntax errors and
/// values of the wrong JSON type, throw `std::invalid_argument` unless stated. Slots are caller
/// data: there is no slot or eligibility policy.

namespace anima {
/// A fitted model bound to a body's skeleton; it shares the body's bind and never owns animation.
struct FittedAsset {
    /// Caller-defined slot; nonempty.
    std::string slot;
    /// Imported fitted model.
    std::shared_ptr<const Asset> source;
    /// Mesh compiled from #source.
    std::shared_ptr<const Mesh> render;
    /// (fitted joint node, body joint node) pairs; see compatible_skin.
    std::vector<std::pair<std::size_t, std::size_t>> joints;
    /// Binds @p fitted to @p body. Throws for a null model, an empty slot or a model with clips,
    /// and `std::runtime_error` when compatible_skin rejects the pair.
    FittedAsset(std::string_view slot, const Asset &body, std::shared_ptr<const Asset> fitted);
    /// World-only pose of #render that copies each mapped joint's world matrix from @p character, a
    /// pose of the body; other nodes keep their rest matrices. Throws `std::out_of_range` when
    /// @p character lacks a mapped joint.
    Pose pose(const Pose &character) const;
};
/// One catalog item that fits the library's body.
struct FittedDefinition {
    std::string id;
    std::string slot;
    /// Relative `.glb` path without `..`, resolved against the body manifest's directory.
    std::filesystem::path model;
};
/// The items of a fitted catalog that fit one body profile, with shared loading.
///
/// Copies share the body and the loaded-model cache. load() and resident_assets() lock a mutex
/// that all copies share, so they may run on several threads.
class FittedLibrary {
  public:
    /// An empty library without a body.
    FittedLibrary() = default;
    /// Decodes @p document for @p body, the model of @p manifest, keeping the items that fit body
    /// profile @p profile.
    ///
    /// The document has `version` 1 and at most 65,536 `items`, each with a unique nonempty `id`, a
    /// nonempty `slot` and a nonempty `fits` object, mapping nonempty body profiles to `model` (a
    /// relative `.glb` path without `..`) and nonempty `skeleton` and `bind_signature`. The fit
    /// for @p profile must match the manifest's skeleton and bind signature. Throws also for a
    /// null @p body.
    FittedLibrary(std::shared_ptr<const Asset> body, const Manifest &manifest, std::string_view profile,
                  std::string_view document);
    /// Whether no item fits this body.
    bool empty() const { return definitions_.empty(); }
    /// Number of items that fit this body.
    std::size_t size() const { return definitions_.size(); }
    /// Whether item @p id fits this body.
    bool contains(std::string_view id) const { return definitions_.contains(id); }
    /// Whether the catalog lists item @p id for any body.
    bool known(std::string_view id) const { return known_ids_.contains(id); }
    /// Forgets every item, as for an empty catalog; loaded models are unaffected.
    void clear() {
        definitions_.clear();
        known_ids_.clear();
    }
    /// Items that fit this body, by id.
    const auto &definitions() const { return definitions_; }
    /// Item @p id. Throws unless it fits this body.
    const FittedDefinition &definition(std::string_view id) const;
    /// Loads item @p id, sharing one FittedAsset per model file and slot while it is alive. Throws
    /// unless the item fits this body, and as load_asset and FittedAsset do.
    std::shared_ptr<const FittedAsset> load(std::string_view id) const;
    /// Meshes of loaded items that are still alive.
    std::vector<std::shared_ptr<const Mesh>> resident_assets() const;

  private:
    friend class FittedSet;
    struct State;
    std::shared_ptr<State> state_;
    std::map<std::string, FittedDefinition, std::less<>> definitions_;
    std::set<std::string, std::less<>> known_ids_;
};
/// Owns the fitted objects worn by one body object.
///
/// replace() and sync() need the body and its scene; destruction is safe after either expires.
/// Attached as a component, on_late_update syncs after every on_update hook of the frame, such as
/// an Animator's.
class FittedSet {
  public:
    /// One worn item.
    struct Instance {
        /// Item id.
        std::string item;
        std::shared_ptr<const FittedAsset> asset;
        /// Child object of the body that renders the item.
        GameObject object;
    };
    /// Throws when @p library has a body that @p owner's mesh does not accept as an animation
    /// source (same node names, parents and rest pose); fails as GameObject::renderer does when
    /// @p owner has no mesh.
    FittedSet(GameObject owner, FittedLibrary library);
    /// Destroys the item objects that still exist.
    ~FittedSet();
    FittedSet(const FittedSet &) = delete;
    FittedSet &operator=(const FittedSet &) = delete;
    /// Makes @p items the worn set.
    ///
    /// Retained items keep their objects; new objects become children of the body with its
    /// current pose, placement and visibility. Every new item loads before the scene changes, and a
    /// failure leaves the worn set unchanged. Throws when the body's mesh or an item object's mesh
    /// was replaced, `std::out_of_range` when the body or an item object no longer exists, and as
    /// FittedLibrary::load does.
    void replace(const std::set<std::string, std::less<>> &items);
    /// Copies the body's current pose and placement to every item while the body is visible, and
    /// its visibility always; hidden sets skip the pose work. Call after publishing the body's
    /// final pose, transform and visibility; this does not drive body animation. Throws as
    /// replace() does for replaced meshes and missing objects.
    void sync();
    /// Component hook: runs sync().
    void on_late_update(double) { sync(); }
    /// Worn items.
    const std::vector<Instance> &instances() const { return instances_; }

  private:
    Scene &scene() const;
    GameObject owner_;
    std::shared_ptr<const Mesh> mesh_;
    FittedLibrary library_;
    std::vector<Instance> instances_;
};
} // namespace anima
