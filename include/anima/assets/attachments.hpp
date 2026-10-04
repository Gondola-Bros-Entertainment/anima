#pragma once
#include <anima/assets/action_runtime.hpp>
#include <anima/assets/motion_runtime.hpp>
#include <anima/scene.hpp>
#include <map>
#include <optional>
#include <set>

/// @file
/// Attachments: props held at body sockets, with their catalogs, shared loading, placement, a
/// follower that keeps them on their sockets, support contacts and ownership checks.
///
/// Part of the `anima::assets` target. Animation ownership (handling), item identity and geometry
/// (visuals) are independent records. Frames are column-major matrices; distances are in meters.
/// Documents are UTF-8 JSON of at most 4 MiB and 64 nesting levels; duplicate and unknown fields
/// are rejected. Invalid documents and arguments, including JSON syntax errors and values of the
/// wrong JSON type, throw `std::invalid_argument` unless stated, as do documents that name a node,
/// clip, item, visual, handling, socket or marker that does not exist. An argument naming one that
/// does not exist throws `std::out_of_range`, and model load failures throw as load_asset and
/// Mesh::compile do (see AttachmentLibrary::load).
/// Which items are held and by which roles, which handling profile performs an action, item
/// metadata such as categories, and gameplay rules belong to the caller.

namespace anima {
/// A support contact: while active, a motion chain moves a secondary body socket onto a prop
/// marker.
struct AttachmentContact {
    /// Motion contact chain; it must end at #socket's node.
    std::string chain;
    /// Secondary body socket, distinct from the handling's primary socket.
    std::string socket;
    /// Prop marker that #socket is placed on.
    std::string marker;
    /// Body model-space pole for the chain, supplied by the handling profile.
    anima::Vec3 pole{};
    /// Base clips during which the contact is active.
    std::set<std::string, std::less<>> clips;
    /// Actions during which the contact is active.
    std::set<std::string, std::less<>> actions;
};
/// A handling profile: the socket, motion layer clips and support contacts with which an item is
/// held.
struct AttachmentHandling {
    /// Unique, nonempty id.
    std::string id;
    /// Primary body socket; the empty handling has none, and items need one.
    std::string socket;
    /// Layer clip (see MotionRuntime::compose) applied over every base clip that #layer_overrides
    /// does not name, or empty for none.
    std::string layer{};
    /// Layer clip per base clip name, replacing #layer for that clip.
    std::map<std::string, std::string, std::less<>> layer_overrides{};
    /// At most 4 support contacts, with distinct chains; empty for none.
    std::vector<AttachmentContact> support_contacts{};
    /// Layer clip for base clip @p clip: its override, else #layer.
    [[nodiscard]] std::string_view layer_for(std::string_view clip) const {
        const auto found = layer_overrides.find(clip);
        return found == layer_overrides.end() ? std::string_view(layer) : std::string_view(found->second);
    }
};
/// A prop model with its grip and markers.
struct AttachmentVisual {
    std::string id;
    /// Relative `.glb` path, decoded from the catalog's UTF-8, without a root, colon, backslash or
    /// `..` component; resolved against AttachmentCatalog::directory.
    std::filesystem::path model;
    /// Rigid, right-handed frame in prop model space that the primary socket holds.
    anima::Mat4 primary_grip = anima::identity();
    /// Named rigid, right-handed frames in prop model space, or relative to their node when listed
    /// in #marker_nodes. The name `primary` is reserved.
    std::map<std::string, anima::Mat4, std::less<>> markers;
    /// Prop node whose animation moves the grip from its rest placement, or empty for none;
    /// #primary_grip stays in prop model space. See animated_attachment_binding.
    std::string primary_node;
    /// Marker name to the prop node it follows.
    std::map<std::string, std::string, std::less<>> marker_nodes;
    /// Semantic track name to prop clip name.
    std::map<std::string, std::string, std::less<>> animation_tracks;
};
/// A catalog item: a visual held with a handling profile.
struct AttachmentDefinition {
    std::string id;
    /// AttachmentVisual id.
    std::string visual;
    /// AttachmentHandling id; the profile must have a primary socket.
    std::string handling;
};
/// A decoded catalog; see decode_attachment_catalog.
struct AttachmentCatalog {
    /// Directory that AttachmentVisual::model paths resolve against.
    std::filesystem::path directory;
    /// Handling profile of a role with no item, which AttachmentLibrary::handling returns for an
    /// empty item id; it has no socket.
    std::string empty_handling;
    /// Handling profiles by id.
    std::map<std::string, AttachmentHandling, std::less<>> handling;
    /// Visuals by id.
    std::map<std::string, AttachmentVisual, std::less<>> visuals;
    /// Items by id.
    std::map<std::string, AttachmentDefinition, std::less<>> items;
};
/// A body socket: a frame relative to a model node. Pass it to bind_attachment to place a prop's
/// grip on it.
struct AttachmentSocket {
    /// Body model node.
    std::size_t node{};
    /// Frame relative to #node.
    anima::Mat4 local = anima::identity();
};
/// Body sockets by name.
using AttachmentSockets = std::map<std::string, AttachmentSocket, std::less<>>;

/// Decodes a catalog document (`version` 4, `units` `"meters"`) whose models resolve against
/// @p directory.
///
/// The integer `version` is checked before any other field; another version throws "Unsupported
/// attachment catalog version". The document has `empty_handling` and nonempty `handling`,
/// `visuals` and `items` arrays. A handling entry has `id`, `socket`, `layer` (a layer clip name,
/// or empty), `layer_overrides` (base clip name to nonempty layer clip name, `{}` for none) and
/// `support_contacts`: an array of at most 4 entries, `[]` for none, of `chain`, `socket`,
/// `marker`, `pole` (three numbers in body model space), `clips` and `actions`, each active for at
/// least one clip or action. A visual has `id`, `model` (a path as AttachmentVisual::model
/// describes), `primary_grip`, `markers`, `primary_node` (a nonempty node name, or null for none),
/// `marker_nodes` (marker name to node name) and `animation_tracks` (track name to clip name). An
/// item has `id`, `visual` and `handling`; its visual must have every marker that its handling's
/// contacts use. Every field is required. Frames are 16 column-major numbers. Layer clips and
/// chains are checked against a MotionRuntime by validate_attachment_ownership, not here.
[[nodiscard]] AttachmentCatalog decode_attachment_catalog(std::string_view document,
                                                          const std::filesystem::path &directory);
/// Decodes the body sockets of @p body, the model of @p manifest.
///
/// The document has the integer `version` 1, checked before any other field; `skeleton` and
/// `bind_signature` equal to the manifest's; `rest_joints`, mapping node names to their expected
/// model-space rest matrices, each within `1e-5` of @p body's; and `sockets`, mapping nonempty
/// names to `node`, one of the rest joints, and `local`, an affine frame relative to it. Frames are
/// 16 column-major numbers. Another version throws "Unsupported attachment socket document
/// version".
[[nodiscard]] AttachmentSockets decode_attachment_sockets(std::string_view document, const Manifest &manifest,
                                                          const Asset &body);
/// A loaded prop model.
struct AttachmentAsset {
    /// Imported model. When its library compiles #render with TexelRetention::until_upload, a copy whose
    /// textures are #render's, which have no texels.
    std::shared_ptr<const Asset> source;
    /// Mesh compiled from #source.
    std::shared_ptr<const Mesh> render;
};
/// Catalog lookups and shared prop loading.
///
/// Copies share the catalog and the loaded-model cache. Moving a library copies it, so a library
/// always has both. References that members return stay valid until the library is destroyed or
/// assigned to. Const members may run on several threads at once, on one library and its copies:
/// loads of different files run concurrently, and concurrent loads of one file share one import.
class AttachmentLibrary {
  public:
    /// A library of @p definition whose loads compile their meshes with @p texel_retention. Throws
    /// `std::invalid_argument` for an unknown @p texel_retention.
    explicit AttachmentLibrary(AttachmentCatalog definition, TexelRetention texel_retention = TexelRetention::keep);
    AttachmentLibrary(const AttachmentLibrary &) = default;
    AttachmentLibrary &operator=(const AttachmentLibrary &) = default;
    /// The catalog as given; the library does not validate it again.
    [[nodiscard]] const AttachmentCatalog &catalog() const noexcept;
    /// Item @p id. Throws `std::out_of_range` for an unknown id.
    [[nodiscard]] const AttachmentDefinition &item(std::string_view id) const;
    /// Visual @p id. Throws `std::out_of_range` for an unknown id.
    [[nodiscard]] const AttachmentVisual &visual(std::string_view id) const;
    /// Handling profile of item @p item_id, or the empty handling for an empty id. Throws
    /// `std::out_of_range` for an unknown id.
    [[nodiscard]] const AttachmentHandling &handling(std::string_view item_id) const;
    /// Loads the model of visual @p visual_id, importing it with load_asset and @p options.
    ///
    /// Loads of one file share an AttachmentAsset while it is alive, and its Mesh while that is
    /// alive: with only the Mesh alive, a load imports the file again and keeps the Mesh when the
    /// Mesh accepts the new model as an animation source (see Mesh::accepts_animation_source) and
    /// has as many textures, and otherwise compiles a new one. Concurrent loads of one file share
    /// one import and compile, and their failure, which caches nothing, so a later load imports
    /// again. Each load then checks the model against its own visual.
    ///
    /// @p options cancel and count the import only, not the compile after it. A load that returns
    /// a live AttachmentAsset, or waits for another load's import, adds no steps to its
    /// StagingProgress, and its StopToken does not end the wait. When the import it waits for
    /// throws StagingCancelled, the load throws it too if its own token reports a stop, and
    /// otherwise looks again, so that it imports the file itself unless another load has begun.
    ///
    /// Throws `std::out_of_range` for an unknown visual, `std::invalid_argument` for an animated
    /// model without AttachmentVisual::animation_tracks, `std::runtime_error` when the model does
    /// not have exactly one node or clip of each name that the visual uses, StagingCancelled when
    /// its import is cancelled, and as load_asset and Mesh::compile do.
    [[nodiscard]] std::shared_ptr<const AttachmentAsset> load(std::string_view visual_id,
                                                              const StagingOptions &options = {}) const;
    /// Every Mesh that loads compiled and that is still alive, once each, in no particular order.
    [[nodiscard]] std::vector<std::shared_ptr<const Mesh>> resident_meshes() const;

  private:
    static void validate(const Asset &source, const AttachmentVisual &visual);
    struct State;
    std::shared_ptr<State> state_;
};
/// A socket combined with a visual's grip: where the prop's model origin goes, relative to a body
/// node. bind_attachment and animated_attachment_binding make one from a socket; it is a type of
/// its own so that an AttachmentSocket, whose frame is where the grip goes, is not taken for one.
struct AttachmentBinding {
    /// Body model node.
    std::size_t node{};
    /// Prop model-to-node matrix.
    anima::Mat4 local = anima::identity();
};
/// Binding that puts @p visual's primary grip on @p socket. Throws anima::MathError when the grip
/// cannot be inverted.
[[nodiscard]] AttachmentBinding bind_attachment(const AttachmentSocket &socket, const AttachmentVisual &visual);
/// Prop model-to-world matrix, `actor * pose.world[binding.node] * binding.local`; with the
/// default @p actor it is in the body's model space. Throws `std::out_of_range` for a node outside
/// @p pose.
[[nodiscard]] Mat4 attachment_placement(const Pose &pose, const AttachmentBinding &binding,
                                        const Mat4 &actor = identity());
/// Whether sample_attachment_pose requires the visual to declare the track it names.
enum class TrackRequirement {
    required, ///< A track the visual lacks throws `std::invalid_argument`.
    optional  ///< A track the visual lacks gives the rest pose.
};
/// Prop pose for semantic @p track at normalized @p progress in [0, 1]: its clip sampled at
/// `progress * duration`. An empty track, or a track the visual lacks when @p requirement is
/// TrackRequirement::optional, gives the rest pose. Throws `std::invalid_argument` for @p progress
/// that is not finite or lies outside [0, 1] and for a missing required track, and as
/// find_animation and sample_pose do.
[[nodiscard]] Pose sample_attachment_pose(const AttachmentAsset &asset, const AttachmentVisual &visual,
                                          std::string_view track = {}, double progress = 0,
                                          TrackRequirement requirement = TrackRequirement::required);
/// @p binding, made by bind_attachment for @p visual, adjusted so that the grip as
/// AttachmentVisual::primary_node moves it in @p pose meets the socket.
///
/// The node carries AttachmentVisual::primary_grip, a frame in prop model space, by its motion from
/// its rest placement: the moved grip is `pose.world[node] * inverse(rest) * primary_grip`, where
/// `rest` is the node's model-space matrix in the rest pose of @p asset. At that rest pose the result
/// equals @p binding, up to rounding. Returns @p binding unchanged when the visual has no primary
/// node. Throws `std::out_of_range` when @p asset has no node of that name or @p pose has no world
/// matrix for it, `std::invalid_argument` when several nodes have the name, `std::runtime_error`
/// for an invalid or cyclic node hierarchy, and anima::MathError when a frame cannot be inverted or
/// a rest rotation normalized.
[[nodiscard]] AttachmentBinding animated_attachment_binding(const AttachmentBinding &binding,
                                                            const AttachmentVisual &visual, const Asset &asset,
                                                            const Pose &pose);
/// A prop model with a pose of it, such as sample_attachment_pose returns. It refers to both, so
/// they must outlive it.
struct PropPose {
    /// Prop model, such as AttachmentAsset::source.
    const Asset &asset;
    /// Pose of #asset.
    const Pose &pose;
};
/// Marker @p name in prop model space. A marker listed in AttachmentVisual::marker_nodes follows
/// its node in the pose of @p prop, so it needs @p prop. Throws `std::out_of_range` for an unknown
/// marker, or a marker node that the asset of @p prop lacks or its pose has no world matrix for;
/// `std::invalid_argument` when several nodes have that name; and `std::invalid_argument`
/// ("Animated attachment marker requires the sampled prop pose") for an animated marker without
/// @p prop.
[[nodiscard]] Mat4 attachment_marker(const AttachmentVisual &visual, std::string_view name,
                                     std::optional<PropPose> prop = {});
/// One attached item in a scene.
struct AttachmentInstance {
    /// Scene instance, once added.
    std::optional<Scene::Id> instance;
    /// Item id; empty when nothing is attached.
    std::string item_id;
    std::shared_ptr<const AttachmentAsset> asset;
    /// Binding from the item's handling socket and visual.
    AttachmentBinding binding;
    /// Replaces the attached item with item @p id, or removes it for an empty id.
    ///
    /// The new item is added to @p scene as a root instance with an identity transform; place it
    /// with attachment_placement. The old instance is then removed from @p scene; one that no
    /// longer exists there, because it was removed directly or with its parent, counts as already
    /// removed. Returns false and changes nothing when @p id is already attached. Throws
    /// `std::invalid_argument` ("Attachment instance belongs to another scene") when the old
    /// instance belongs to a scene other than @p scene, `std::out_of_range` for an unknown item or
    /// socket, and as AttachmentLibrary::load and Scene::create do; on failure the old item stays
    /// attached and @p scene is unchanged.
    bool replace(Scene &scene, const AttachmentLibrary &library, const AttachmentSockets &sockets, std::string_view id);
};
/// Items attached for named roles.
struct AttachmentSet {
    /// Instances by role.
    std::map<std::string, AttachmentInstance, std::less<>> roles;
    /// Whether the set holds exactly @p desired, a map from role to item id.
    [[nodiscard]] bool matches(const std::map<std::string, std::string, std::less<>> &desired) const;
    /// Loads the items of @p desired, a map from role to item id, and binds them to @p sockets,
    /// without touching any scene. There is no fixed limit on the number of roles. Each item loads
    /// with AttachmentLibrary::load and @p options, one role after another in role order, so the
    /// imports add to one StagingProgress and a stop ends the import in progress. Throws for an
    /// empty role or item id, or an unknown item or socket, and as AttachmentLibrary::load does,
    /// including StagingCancelled.
    [[nodiscard]] static AttachmentSet prepare(const AttachmentLibrary &library, const AttachmentSockets &sockets,
                                               const std::map<std::string, std::string, std::less<>> &desired,
                                               const StagingOptions &options = {});
    /// Adds every prepared item to @p scene as a root instance with an identity transform; place
    /// them with attachment_placement. On failure the instances already added are removed. Throws
    /// unless every role is prepared and not yet added.
    void add(Scene &scene);
    /// Adds every prepared item as a child of @p owner, starting at its socket in the owner's
    /// current pose and with the owner's visibility. The children inherit the owner's transform and
    /// lifetime, but not later poses or visibility; AttachmentFollower keeps them on their sockets.
    /// Throws as add(Scene &) does, when @p owner has no mesh, and `std::out_of_range` for a
    /// binding node outside the owner's pose, before adding anything.
    void add(GameObject owner);
    /// Removes the set's instances that still exist from @p scene and clears every handle. Throws,
    /// before removing anything, when an instance belongs to another scene.
    void remove(Scene &scene);
    /// Shows or hides every added instance, after checking every handle. Throws
    /// `std::out_of_range` for a stale handle.
    void set_visible(Scene &scene, bool visible) const;

  private:
    void add_to(Scene &scene, const GameObject *owner);
};
/// Owns the items of an AttachmentSet held by one body object, and keeps them on their sockets as
/// the body's pose changes.
///
/// The items are children of the body. Construction and sync() need the body and its scene;
/// destruction is safe after either expires. Attached as a component, on_late_update syncs after
/// every on_update hook of the frame, such as an Animator's. Use it from the thread that uses its
/// scene.
class AttachmentFollower {
  public:
    /// Adds @p attachments as children of the object of @p owner, the body, as
    /// AttachmentSet::add(GameObject) does, and keeps the body's mesh, which sync() requires. Each
    /// role starts with its AttachmentInstance::binding. Throws as AttachmentSet::add(GameObject)
    /// does, including `std::out_of_range` ("Expired GameObject handle") for an expired body; on
    /// failure no item stays in the scene.
    AttachmentFollower(ComponentOwner owner, AttachmentSet attachments);
    /// Destroys the item objects that still exist, with their descendants, wherever they have been
    /// moved in the hierarchy.
    ~AttachmentFollower();
    AttachmentFollower(const AttachmentFollower &) = delete;
    AttachmentFollower &operator=(const AttachmentFollower &) = delete;
    /// The set as added. Its bindings are the ones each role started with, whatever set_binding()
    /// has chosen since.
    const AttachmentSet &attachments() const noexcept { return attachments_; }
    /// Object that renders the item of @p role; the handle is invalid once that object is
    /// destroyed. Throws `std::out_of_range` ("Unknown attachment role: " followed by @p role) for a
    /// role the set does not hold.
    GameObject object(std::string_view role) const;
    /// Places the item of @p role with @p binding from the next sync() on, for example with the
    /// result of animated_attachment_binding for the item's current prop pose, which the caller
    /// sets on object(). Pass the role's binding from attachments() to return to it. Throws
    /// `std::out_of_range` for an unknown role, as object() does, or for a node outside the owner's
    /// mesh ("Attachment binding node is outside the owner mesh"), and `std::invalid_argument`
    /// ("Attachment binding frame must be finite and affine") for a frame that is not a valid scene
    /// matrix (see scene.hpp); on failure nothing changes.
    void set_binding(std::string_view role, const AttachmentBinding &binding);
    /// While the owner's renderer is visible, sets each item's local matrix to
    /// attachment_placement of the owner's current pose, its mesh's rest pose when none was set,
    /// and the role's binding. Each item's renderer takes the owner's visibility either way, and
    /// items whose objects no longer exist are skipped. Call it after publishing the owner's final
    /// pose and visibility; it does not drive animation.
    ///
    /// Throws `std::out_of_range` ("Expired GameObject handle") when the owner no longer exists,
    /// `std::invalid_argument` ("Attachment follower requires its original owner mesh") when the
    /// owner's mesh was replaced or removed, and `std::logic_error` ("GameObject has no
    /// MeshRenderer") when an item's renderer was removed, each before changing anything; and as
    /// GameObject::set_local_matrix does for a placement that overflows the float range.
    void sync();
    /// Component hook: runs sync().
    void on_late_update(double) { sync(); }

  private:
    // An item's object and the binding that sync() places it with.
    struct Follow {
        GameObject object;
        AttachmentBinding binding;
    };
    Scene &scene() const;
    GameObject owner_;
    std::shared_ptr<const Mesh> mesh_;
    AttachmentSet attachments_;
    std::map<std::string, Follow, std::less<>> follows_;
};
/// Optional inputs of apply_attachment_contacts. It refers to what its members name, so build it in
/// the call, for example `{.action = "reach", .prop = PropPose{*asset.source, pose}}`.
struct AttachmentContactOptions {
    /// Action whose contacts are active alongside the base clip's, or empty for none.
    std::string_view action{};
    /// Prop model and pose that animated markers follow (see attachment_marker), or none.
    std::optional<PropPose> prop{};
    /// Contact weights by chain name, each in [0, 1], or null for all 1; a chain the map does not
    /// name has weight 1.
    const std::map<std::string, float, std::less<>> *weights = nullptr;
};
/// Solves the support contacts of @p handling that are active for base clip @p clip or the action
/// of @p options, in order, with one MotionRuntime::evaluate of @p source.
///
/// Each contact moves its chain so that its body socket frame meets its prop marker, with the prop
/// placed through @p primary in @p source, and is solved on the result of the contacts before it.
/// A zero weight skips the contact. Animated markers need the prop of @p options. With no active
/// contact, returns @p source unchanged. Throws `std::invalid_argument` ("Invalid item contact
/// weight") for a weight that is not finite or lies outside [0, 1], and for a collapsed contact
/// frame (see affine_rotation), `std::out_of_range` for an unknown socket, and as
/// MotionRuntime::evaluate and attachment_marker do.
[[nodiscard]] MotionEvaluation apply_attachment_contacts(const MotionRuntime &runtime, const Pose &source,
                                                         std::string_view clip, const AttachmentHandling &handling,
                                                         const AttachmentVisual &visual,
                                                         const AttachmentBinding &primary,
                                                         const AttachmentSockets &sockets,
                                                         const AttachmentContactOptions &options = {});
/// Whether validate_attachment_ownership lets two roles hold their props on one primary socket node.
enum class PrimarySocketSharing {
    /// Each role's primary socket node is its own; a shared one throws `std::invalid_argument`.
    exclusive,
    /// Roles may share a primary socket node.
    shared
};
/// Checks that the items of @p attachments can be held together; call it before adding a prepared
/// set to a scene.
///
/// For every base clip, the layer clips that the roles' handling profiles apply to it (see
/// AttachmentHandling::layer_for) must not overlap (see MotionRuntime::validate_layers). When
/// @p primary_sharing is PrimarySocketSharing::exclusive, no two roles may share a primary socket
/// node; pose layers and contact chains always need unique ownership. Each support contact chain
/// must end at its declared socket's node, overlap no other contact chain and move no role's
/// primary socket. Throws `std::out_of_range` for an unknown chain or socket.
void validate_attachment_ownership(const MotionRuntime &runtime, const AttachmentLibrary &library,
                                   const AttachmentSet &attachments, const AttachmentSockets &sockets,
                                   PrimarySocketSharing primary_sharing = PrimarySocketSharing::exclusive);
/// Checks that @p attachments can perform @p action: the handling profiles of the attached items
/// must meet the action's required roles (see ActionRuntime::validate_roles), and every required
/// prop track of the action must be a track of the visual attached for its role.
///
/// The handling profile that performs the action is the caller's choice, which
/// ActionRuntime::sample checks. Throws `std::out_of_range` for an unknown action or item, and
/// `std::invalid_argument` for an unmet role or a missing required track.
void validate_attachment_action(const ActionRuntime &runtime, const AttachmentLibrary &library,
                                const AttachmentSet &attachments, std::string_view action);
} // namespace anima
