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
/// values of the wrong JSON type, throw `std::invalid_argument` unless stated. Which items a body
/// uses together is the caller's choice: there is no slot or eligibility policy.

namespace anima {
/// A fitted model bound to a body's skeleton; it shares the body's bind and never owns animation.
struct FittedAsset {
    /// Imported fitted model. With TexelRetention::until_upload, a copy whose textures are #render's,
    /// which have no texels.
    std::shared_ptr<const Asset> source;
    /// Mesh compiled from #source.
    std::shared_ptr<const Mesh> render;
    /// (fitted joint node, body joint node) pairs; see compatible_skin.
    std::vector<std::pair<std::size_t, std::size_t>> joints;
    /// Binds @p fitted to @p body and compiles #render with @p texel_retention. Throws for a null
    /// model or a model with clips, `std::runtime_error` when compatible_skin rejects the pair, and
    /// as Mesh::compile does.
    FittedAsset(const Asset &body, std::shared_ptr<const Asset> fitted,
                TexelRetention texel_retention = TexelRetention::keep);
    /// World-only pose of #render that copies each mapped joint's world matrix from @p body, a pose
    /// of the body model; other nodes keep their rest matrices. Throws `std::out_of_range` when
    /// @p body lacks a mapped joint.
    Pose pose(const Pose &body) const;
};
/// One catalog item that fits the library's body.
struct FittedDefinition {
    std::string id;
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
    /// The document has `version` 2 and at most 65,536 `items`, each with a unique nonempty `id` and
    /// a nonempty `fits` object, mapping nonempty body profiles to `model` (a relative `.glb` path
    /// without `..`) and nonempty `skeleton` and `bind_signature`. The fit for @p profile must
    /// match the manifest's skeleton and bind signature. Loads compile their meshes with
    /// @p texel_retention. Throws also for a null @p body or an unknown @p texel_retention.
    FittedLibrary(std::shared_ptr<const Asset> body, const Manifest &manifest, std::string_view profile,
                  std::string_view document, TexelRetention texel_retention = TexelRetention::keep);
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
    /// Item @p id. Throws `std::out_of_range` unless it fits this body.
    const FittedDefinition &definition(std::string_view id) const;
    /// Loads item @p id, sharing one FittedAsset per model file while it is alive. Throws
    /// `std::out_of_range` unless the item fits this body, and as load_asset and FittedAsset do.
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
/// Owns the fitted objects that follow one body object.
///
/// replace() and sync() need the body and its scene; destruction is safe after either expires.
/// Attached as a component, on_late_update syncs after every on_update hook of the frame, such as
/// an Animator's.
class FittedSet {
  public:
    /// One active item.
    struct Instance {
        /// Item id.
        std::string item;
        std::shared_ptr<const FittedAsset> asset;
        /// Child object of the body that renders the item.
        GameObject object;
    };
    /// Follows the object of @p owner, the body. Throws when @p library has a body that the body's
    /// mesh does not accept as an animation source (same node names, parents and rest pose); fails
    /// as GameObject::renderer does when the body has no mesh.
    FittedSet(ComponentOwner owner, FittedLibrary library);
    /// Destroys the item objects that still exist.
    ~FittedSet();
    FittedSet(const FittedSet &) = delete;
    FittedSet &operator=(const FittedSet &) = delete;
    /// Makes @p items the active set.
    ///
    /// Retained items keep their objects; new objects become children of the body with its
    /// current pose, placement and visibility. Every new item loads before the scene changes, and a
    /// failure leaves the active set unchanged. Throws when the body's mesh or an item object's mesh
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
    /// Active items.
    const std::vector<Instance> &instances() const { return instances_; }

  private:
    Scene &scene() const;
    GameObject owner_;
    std::shared_ptr<const Mesh> mesh_;
    FittedLibrary library_;
    std::vector<Instance> instances_;
};
} // namespace anima
