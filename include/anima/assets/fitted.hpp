#pragma once
#include <anima/assets/manifest.hpp>
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
/// One joint that a fitted model copies from its body: node indices into each model's
/// Asset::nodes.
struct FittedJoint {
    /// Joint node of the fitted model.
    std::size_t fitted_node{};
    /// Body joint node of the same name, whose world matrix the fitted node copies.
    std::size_t body_node{};
    bool operator==(const FittedJoint &) const = default;
};
/// Pairs each joint node of @p fitted's skin with the @p body joint node of the same name, in
/// @p fitted's joint order, so that @p fitted can copy @p body's pose.
///
/// Both assets need exactly one skin, with equal joint counts and unique joint names. Matching
/// joints need the same ancestor names and inverse bind and rest world matrices within `1e-4`, and
/// every @p fitted primitive must use the skin. Throws `std::runtime_error` otherwise.
[[nodiscard]] std::vector<FittedJoint> compatible_skin(const Asset &body, const Asset &fitted);
/// A fitted model bound to a body's skeleton; it shares the body's bind and never owns animation.
struct FittedAsset {
    /// Imported fitted model. With TexelRetention::until_upload, a copy whose textures are #render's,
    /// which have no texels.
    std::shared_ptr<const Asset> source;
    /// Mesh compiled from #source.
    std::shared_ptr<const Mesh> render;
    /// Joints copied from the body, as compatible_skin returns them.
    std::vector<FittedJoint> joints;
    /// Binds @p fitted to @p body and compiles #render with @p texel_retention. Throws for a null
    /// model or a model with clips, `std::runtime_error` when compatible_skin rejects the pair, and
    /// as Mesh::compile does.
    FittedAsset(const Asset &body, std::shared_ptr<const Asset> fitted,
                TexelRetention texel_retention = TexelRetention::keep);
    /// World-only pose of #render that copies each mapped joint's world matrix from @p body, a pose
    /// of the body model; other nodes keep their rest matrices. Throws `std::out_of_range` when
    /// @p body lacks a mapped joint.
    Pose pose(const Pose &body) const;

  private:
    friend class FittedLibrary;
    // As the public constructor, except that #render is @p resident when that Mesh accepts @p fitted as an animation
    // source and has as many textures.
    FittedAsset(const Asset &body, std::shared_ptr<const Asset> fitted, TexelRetention texel_retention,
                std::shared_ptr<const Mesh> resident);
};
/// One catalog item that fits the library's body.
struct FittedDefinition {
    std::string id;
    /// Relative `.glb` path, decoded from the catalog's UTF-8, without a root, colon, backslash or
    /// `..` component; resolved against the body manifest's directory.
    std::filesystem::path model;
};
/// The items of a fitted catalog that fit one body profile, with shared loading.
///
/// Copies share the body and the loaded-model cache. load() and resident_meshes() may run on
/// several threads at once, on one library and its copies: loads of different files run
/// concurrently, and concurrent loads of one file share one import.
class FittedLibrary {
  public:
    /// An empty library without a body.
    FittedLibrary() = default;
    /// Decodes @p document for @p body, the model of @p manifest, keeping the items that fit body
    /// profile @p profile.
    ///
    /// The document has `version` 2 and at most 65,536 `items`, each with a unique nonempty `id` and
    /// a nonempty `fits` object, mapping nonempty body profiles to `model` (a path as
    /// FittedDefinition::model describes) and nonempty `skeleton` and `bind_signature`. The fit for
    /// @p profile must match the manifest's skeleton and bind signature. Loads compile their meshes with
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
    /// Loads item @p id, importing its model with load_asset and @p options.
    ///
    /// Loads of one model file share a FittedAsset while it is alive, and its Mesh while that is
    /// alive: with only the Mesh alive, a load imports the file again and keeps the Mesh when the
    /// Mesh accepts the new model as an animation source (see Mesh::accepts_animation_source) and
    /// has as many textures, and otherwise compiles a new one. Concurrent loads of one file share
    /// one import, and its failure, which caches nothing, so a later load imports again.
    ///
    /// @p options cancel and count the import only, not the binding and compile after it. A load
    /// that returns a live FittedAsset, or waits for another load's import, adds no steps to its
    /// StagingProgress, and its StopToken does not end the wait. When the import it waits for
    /// throws StagingCancelled, the load throws it too if its own token reports a stop, and
    /// otherwise looks again, so that it imports the file itself unless another load has begun.
    ///
    /// Throws `std::out_of_range` unless the item fits this body, StagingCancelled when its import
    /// is cancelled, and as load_asset and FittedAsset do.
    std::shared_ptr<const FittedAsset> load(std::string_view id, const StagingOptions &options = {}) const;
    /// Every Mesh that loads compiled and that is still alive, once each, in no particular order;
    /// none for a library without a body.
    std::vector<std::shared_ptr<const Mesh>> resident_meshes() const;

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
    /// Throws when @p library has a body that @p owner's mesh does not accept as an animation
    /// source (same node names, parents and rest pose); fails as GameObject::renderer does when
    /// @p owner has no mesh.
    FittedSet(GameObject owner, FittedLibrary library);
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
