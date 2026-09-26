#pragma once
#include <anima/assets/action_runtime.hpp>
#include <anima/assets/motion_runtime.hpp>
#include <anima/scene.hpp>
#include <map>
#include <mutex>
#include <set>

/// @file
/// Attachments: props held at body sockets, with their catalogs, shared loading, placement,
/// support contacts and ownership checks.
///
/// Part of the `anima::assets` target. Animation ownership (handling), item identity and geometry
/// (visuals) are independent records. Frames are column-major matrices; distances are in meters.
/// Documents are UTF-8 JSON of at most 4 MiB and 64 nesting levels; duplicate and unknown fields
/// are rejected. Invalid documents and arguments throw `std::invalid_argument` unless stated.
/// Model load failures and node or clip names that match nothing or several things throw
/// `std::runtime_error`. JSON syntax errors and values of the wrong JSON type throw the private
/// JSON parser's exceptions, which derive only from `std::exception`. Item eligibility and
/// gameplay rules belong to the caller.

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
    /// Authoring rationale; not interpreted.
    std::string reason;
    /// Character model-space pole for the chain, supplied by the handling profile.
    anima::Vec3 pole{};
    /// Base clips during which the contact is active.
    std::set<std::string, std::less<>> clips;
    /// Actions during which the contact is active.
    std::set<std::string, std::less<>> actions;
};
/// A handling profile: how a held item is carried and supported.
struct AttachmentHandling {
    /// Carry layer that replaces AttachmentHandling::carry for one base clip.
    struct CarryOverride {
        /// Handling layer to use.
        std::string layer;
        /// Authoring rationale; not interpreted.
        std::string reason;
    };
    /// Unique, nonempty id.
    std::string id;
    /// Primary body socket; the empty handling has none, and items need one.
    std::string socket;
    /// Default carry handling layer, or empty for none.
    std::string carry;
    /// Carry override per base clip name.
    std::map<std::string, CarryOverride, std::less<>> carry_overrides;
    /// Support contacts with distinct chains; 1 to 4 when the catalog declares any.
    std::vector<AttachmentContact> support_contacts;
    /// Carry layer for base clip @p motion: its override, else #carry.
    std::string_view layer(std::string_view motion) const {
        const auto found = carry_overrides.find(motion);
        return found == carry_overrides.end() ? std::string_view(carry) : std::string_view(found->second.layer);
    }
};
/// A prop model with its grip and markers.
struct AttachmentVisual {
    std::string id;
    /// Relative `.glb` path without `..`, resolved against AttachmentCatalog::directory.
    std::filesystem::path model;
    /// Rigid, right-handed frame in prop model space that the primary socket holds.
    anima::Mat4 primary_grip = anima::identity();
    /// Named rigid, right-handed frames in prop model space, or relative to their node when listed
    /// in #marker_nodes. The name `primary` is reserved.
    std::map<std::string, anima::Mat4, std::less<>> markers;
    /// Optional prop node whose animation moves the grip; see animated_attachment_binding.
    std::string primary_node;
    /// Marker name to the prop node it follows.
    std::map<std::string, std::string, std::less<>> marker_nodes;
    /// Semantic track name to prop clip name.
    std::map<std::string, std::string, std::less<>> animation_tracks;
};
/// A catalog item: a visual carried with a handling profile.
struct AttachmentDefinition {
    std::string id;
    /// AttachmentVisual id.
    std::string visual;
    /// AttachmentHandling id; the profile must have a primary socket.
    std::string handling;
    /// Review grouping; never selects gameplay behavior.
    std::string category;
    /// Action named by the item's optional `action_override`; stored, not interpreted.
    std::string action;
    /// Authoring rationale of the `action_override`.
    std::string action_reason;
};
/// A decoded catalog; see decode_attachment_catalog.
struct AttachmentCatalog {
    /// Directory that AttachmentVisual::model paths resolve against.
    std::filesystem::path directory;
    /// Handling profile used with no attachments; it has no socket.
    std::string empty_handling;
    /// Review columns: an empty entry for empty hands, then the default items in catalog order.
    std::vector<std::string> defaults{std::string{}};
    /// Handling profiles by id.
    std::map<std::string, AttachmentHandling, std::less<>> motions;
    /// Visuals by id.
    std::map<std::string, AttachmentVisual, std::less<>> visuals;
    /// Items by id.
    std::map<std::string, AttachmentDefinition, std::less<>> items;
};
/// A body socket: a frame relative to a model node.
struct AttachmentSocket {
    /// Body model node.
    std::size_t node{};
    /// Frame relative to #node.
    anima::Mat4 local = anima::identity();
};

/// Decodes a catalog document (`schema_version` 2, `units` `"meters"`) whose models resolve
/// against @p directory.
///
/// The document has `empty_handling`, `defaults`, and nonempty `handling`, `visuals` and `items`
/// arrays. A handling entry has `id`, `socket`, `carry` and optional `carry_overrides` (base clip
/// to `layer` and `reason`) and `support_contacts`: 1 to 4 entries of `chain`, `socket`, `marker`,
/// `pole`, `clips`, `reason` and optional `actions`, each active for at least one clip or action.
/// A visual has `id`, `model`, `primary_grip`, `markers` and optional `primary_node`,
/// `marker_nodes` and `animation_tracks`. An item has `id`, `category`, `visual`, `handling` and
/// optional `action_override` (`action` and `reason`); its visual must have every marker that its
/// handling's contacts use. `defaults` maps review groups to unique item ids. Frames are 16
/// column-major numbers.
AttachmentCatalog decode_attachment_catalog(std::string_view document, const std::filesystem::path &directory);
/// Decodes the body sockets of @p body, the model of @p manifest.
///
/// The document has `skeleton` and `bind_signature` equal to the manifest's; `rest_joints`,
/// mapping node names to their expected model-space rest matrices, each within `1e-5` of
/// @p body's; and `sockets`, mapping nonempty names to `node`, one of the rest joints, and
/// `local`, an affine frame relative to it. Frames are 16 column-major numbers.
std::map<std::string, AttachmentSocket, std::less<>>
decode_attachment_sockets(std::string_view document, const Manifest &manifest, const Asset &body);
/// A loaded prop model.
struct AttachmentAsset {
    /// Imported model.
    std::shared_ptr<const Asset> source;
    /// Mesh compiled from #source.
    std::shared_ptr<const Mesh> render;
};
/// Catalog lookups and shared prop loading.
///
/// load() and resident_assets() lock an internal mutex, so they may run on several threads.
class AttachmentLibrary {
  public:
    /// The catalog as given; the library does not validate it again.
    const AttachmentCatalog catalog;
    explicit AttachmentLibrary(AttachmentCatalog definition);
    /// Item @p id. Throws for an unknown id.
    const AttachmentDefinition &item(std::string_view id) const;
    /// Visual @p id. Throws for an unknown id.
    const AttachmentVisual &visual(std::string_view id) const;
    /// Handling profile of item @p item_id, or the empty handling for an empty id. Throws for
    /// unknown ids.
    const AttachmentHandling &motion(std::string_view item_id) const;
    /// Loads the model of visual @p visual_id; loads of the same file share its Asset and Mesh
    /// while they are alive.
    ///
    /// Throws for an unknown visual or an animated model without
    /// AttachmentVisual::animation_tracks, and `std::runtime_error` when the model fails to load or
    /// lacks a node or clip that the visual names.
    std::shared_ptr<const AttachmentAsset> load(std::string_view visual_id) const;
    /// Meshes of loaded models that are still alive.
    std::vector<std::shared_ptr<const Mesh>> resident_assets() const;

  private:
    static void validate(const Asset &source, const AttachmentVisual &visual);
    struct CachedModel {
        std::weak_ptr<const Asset> source;
        std::weak_ptr<const Mesh> render;
    };
    mutable std::mutex mutex_;
    mutable std::map<std::filesystem::path, CachedModel> models_;
};
/// A socket combined with a visual's grip: where the prop's model origin goes, relative to the
/// body node.
using AttachmentBinding = AttachmentSocket;
/// Binding that puts @p visual's primary grip on @p socket. Throws anima::MathError when the grip
/// cannot be inverted.
AttachmentBinding bind_attachment(const AttachmentSocket &socket, const AttachmentVisual &visual);
/// Prop model-to-world matrix, `actor * pose.world[binding.node] * binding.local`; with the
/// default @p actor it is in the body's model space. Throws `std::out_of_range` for a node outside
/// @p pose.
Mat4 attachment_placement(const Pose &pose, const AttachmentBinding &binding, const Mat4 &actor = identity());
/// Prop pose for semantic @p track at normalized @p progress in [0, 1]: its clip sampled at
/// `progress * duration`. An empty track, or a track the visual lacks when @p required is false,
/// gives the rest pose. Throws for invalid progress or a missing required track.
Pose sample_attachment_pose(const AttachmentAsset &asset, const AttachmentVisual &visual, std::string_view track = {},
                            double progress = 0, bool required = true);
/// @p binding adjusted so that the grip as moved by AttachmentVisual::primary_node in @p pose,
/// rather than the static grip, meets the socket. Returns @p binding unchanged when the visual has
/// no primary node.
AttachmentBinding animated_attachment_binding(const AttachmentBinding &binding, const AttachmentVisual &visual,
                                              const Asset &asset, const Pose &pose);
/// Marker @p name in prop model space. A marker listed in AttachmentVisual::marker_nodes follows
/// its node in @p pose, so it needs @p asset and @p pose. Throws for an unknown marker or a
/// missing pose.
Mat4 attachment_marker(const AttachmentVisual &visual, const Asset *asset, const Pose *pose, std::string_view name);
/// One attached item in a scene.
struct AttachmentInstance {
    /// Scene instance, once added.
    std::optional<Scene::Id> instance;
    /// Item category.
    std::string category;
    /// Item id; empty when nothing is attached.
    std::string item_id;
    std::shared_ptr<const AttachmentAsset> asset;
    /// Binding from the item's handling socket and visual.
    AttachmentBinding binding;
    /// Replaces the attached item with item @p id, or removes it for an empty id.
    ///
    /// The new item is added to @p scene as a root instance with an identity transform; place it
    /// with attachment_placement. Returns false and changes nothing when @p id is already
    /// attached. Throws for an unknown item or socket and as AttachmentLibrary::load does; the old
    /// item stays attached when loading fails.
    bool equip(Scene &scene, const AttachmentLibrary &library,
               const std::map<std::string, AttachmentSocket, std::less<>> &sockets, std::string_view id);
};
/// Items attached for named roles.
struct AttachmentSet {
    /// Instances by role.
    std::map<std::string, AttachmentInstance, std::less<>> roles;
    /// Whether the set holds exactly @p desired, a map from role to item id.
    bool matches(const std::map<std::string, std::string, std::less<>> &desired) const;
    /// Loads the items of @p desired, a map from role to item id, and binds them to @p sockets,
    /// without touching any scene. Throws for more than 8 roles, an empty role or item id, or an
    /// unknown item or socket, and as AttachmentLibrary::load does.
    static AttachmentSet prepare(const AttachmentLibrary &library,
                                 const std::map<std::string, AttachmentSocket, std::less<>> &sockets,
                                 const std::map<std::string, std::string, std::less<>> &desired);
    /// Adds every prepared item to @p scene as a root instance with an identity transform; place
    /// them with attachment_placement. On failure the instances already added are removed. Throws
    /// unless every role is prepared and not yet added.
    void add(Scene &scene);
    /// Adds every prepared item as a child of @p owner, starting at its socket in the owner's
    /// current pose and with the owner's visibility. The children inherit the owner's transform and
    /// lifetime but do not follow later poses. Throws as add(Scene &) does, and when @p owner has
    /// no mesh.
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
/// Solves the support contacts of @p handling that are active for base clip @p clip or action
/// @p action, in order, starting from @p source.
///
/// Each contact moves its chain so that its body socket frame meets its prop marker, with the prop
/// placed through @p primary in @p source. Weights come from @p weights by chain name (default 1,
/// each in [0, 1]); a zero weight skips the contact. Animated markers need @p prop_asset and
/// @p prop_pose. With no active contact, returns @p source unchanged. Throws for an invalid weight
/// or unknown socket, and as MotionRuntime::evaluate and attachment_marker do.
MotionEvaluation apply_attachment_contacts(const MotionRuntime &runtime, const Pose &source, std::string_view clip,
                                           const AttachmentHandling &handling, const AttachmentVisual &visual,
                                           const AttachmentBinding &primary,
                                           const std::map<std::string, AttachmentSocket, std::less<>> &sockets,
                                           const Asset *prop_asset = nullptr, const Pose *prop_pose = nullptr,
                                           std::string_view action = {},
                                           const std::map<std::string, float, std::less<>> *weights = nullptr);
/// Checks that the items of @p attachments can be carried together; call it before adding a
/// prepared set to a scene.
///
/// For every base clip, the carry layers of the roles must not overlap (see
/// MotionRuntime::validate_carries). With @p exclusive_primary, no two roles may share a primary
/// socket node; pose layers and contact chains always need unique ownership. Each support contact
/// chain must end at its declared socket's node, overlap no other contact chain and move no
/// role's primary socket. Throws `std::out_of_range` for an unknown chain.
void validate_attachment_ownership(const MotionRuntime &runtime, const AttachmentLibrary &library,
                                   const AttachmentSet &attachments,
                                   const std::map<std::string, AttachmentSocket, std::less<>> &sockets,
                                   bool exclusive_primary = true);
/// Checks that @p attachments can perform @p action and returns the handling profile to use, as
/// ActionRuntime::loadout_handling chooses it with the catalog's empty handling. Every required
/// prop track of the action must be a track of the visual attached for its role.
std::string validate_attachment_action(const ActionRuntime &runtime, const AttachmentLibrary &library,
                                       const AttachmentSet &attachments, std::string_view action);
} // namespace anima
