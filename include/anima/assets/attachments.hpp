#pragma once
#include <anima/assets/action_runtime.hpp>
#include <anima/assets/motion_runtime.hpp>
#include <anima/scene.hpp>
#include <map>
#include <mutex>
#include <set>
namespace anima {
// Animation ownership, item identity and geometry are independent records.
struct AttachmentContact {
    std::string chain, socket, marker, reason;
    anima::Vec3 pole{}; // Character model space, supplied by the handling profile.
    std::set<std::string, std::less<>> clips;
    std::set<std::string, std::less<>> actions;
};
struct AttachmentHandling {
    struct CarryOverride {
        std::string layer, reason;
    };
    std::string id, socket, carry;
    std::map<std::string, CarryOverride, std::less<>> carry_overrides;
    std::vector<AttachmentContact> support_contacts;
    std::string_view layer(std::string_view motion) const {
        const auto found = carry_overrides.find(motion);
        return found == carry_overrides.end() ? std::string_view(carry) : std::string_view(found->second.layer);
    }
};
struct AttachmentVisual {
    std::string id;
    std::filesystem::path model;
    anima::Mat4 primary_grip = anima::identity();
    std::map<std::string, anima::Mat4, std::less<>> markers;
    std::string primary_node;
    std::map<std::string, std::string, std::less<>> marker_nodes, animation_tracks;
};
struct AttachmentDefinition {
    std::string id, visual, handling;
    std::string category; // Optional review grouping; never selects gameplay behavior.
    std::string action, action_reason;
};
struct AttachmentCatalog {
    std::filesystem::path directory;
    std::string empty_handling;
    std::vector<std::string> defaults{std::string{}}; // Review columns, including empty hands.
    std::map<std::string, AttachmentHandling, std::less<>> motions;
    std::map<std::string, AttachmentVisual, std::less<>> visuals;
    std::map<std::string, AttachmentDefinition, std::less<>> items;
};
struct AttachmentSocket {
    std::size_t node{};
    anima::Mat4 local = anima::identity();
};

AttachmentCatalog decode_attachment_catalog(std::string_view document, const std::filesystem::path &directory);
std::map<std::string, AttachmentSocket, std::less<>>
decode_attachment_sockets(std::string_view document, const Manifest &manifest, const Asset &body);
struct AttachmentAsset {
    std::shared_ptr<const Asset> source;
    std::shared_ptr<const Mesh> render;
};
class AttachmentLibrary {
  public:
    const AttachmentCatalog catalog;
    explicit AttachmentLibrary(AttachmentCatalog definition);
    const AttachmentDefinition &item(std::string_view id) const;
    const AttachmentVisual &visual(std::string_view id) const;
    const AttachmentHandling &motion(std::string_view item_id) const;
    std::shared_ptr<const AttachmentAsset> load(std::string_view visual_id) const;
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
using AttachmentBinding = AttachmentSocket;
AttachmentBinding bind_attachment(const AttachmentSocket &socket, const AttachmentVisual &visual);
Mat4 attachment_placement(const Pose &pose, const AttachmentBinding &binding, const Mat4 &actor = identity());
Pose sample_attachment_pose(const AttachmentAsset &asset, const AttachmentVisual &visual, std::string_view track = {},
                            double progress = 0, bool required = true);
AttachmentBinding animated_attachment_binding(const AttachmentBinding &binding, const AttachmentVisual &visual,
                                              const Asset &asset, const Pose &pose);
Mat4 attachment_marker(const AttachmentVisual &visual, const Asset *asset, const Pose *pose, std::string_view name);
struct AttachmentInstance {
    std::optional<Scene::Id> instance;
    std::string category, item_id;
    std::shared_ptr<const AttachmentAsset> asset;
    AttachmentBinding binding;
    bool equip(Scene &scene, const AttachmentLibrary &library,
               const std::map<std::string, AttachmentSocket, std::less<>> &sockets, std::string_view id);
};
struct AttachmentSet {
    std::map<std::string, AttachmentInstance, std::less<>> roles;
    bool matches(const std::map<std::string, std::string, std::less<>> &desired) const;
    static AttachmentSet prepare(const AttachmentLibrary &library,
                                 const std::map<std::string, AttachmentSocket, std::less<>> &sockets,
                                 const std::map<std::string, std::string, std::less<>> &desired);
    void add(Scene &scene);
    // Body-owned children inherit transform/lifetime and start at their socket.
    void add(GameObject owner);
    void remove(Scene &scene);
    void set_visible(Scene &scene, bool visible) const;

  private:
    void add_to(Scene &scene, const GameObject *owner);
};
MotionEvaluation apply_attachment_contacts(const MotionRuntime &runtime, const Pose &source, std::string_view clip,
                                           const AttachmentHandling &handling, const AttachmentVisual &visual,
                                           const AttachmentBinding &primary,
                                           const std::map<std::string, AttachmentSocket, std::less<>> &sockets,
                                           const Asset *prop_asset = nullptr, const Pose *prop_pose = nullptr,
                                           std::string_view action = {},
                                           const std::map<std::string, float, std::less<>> *weights = nullptr);
// Call before committing a prepared set. Callers decide whether primary sockets
// are exclusive; pose layers and contact chains always need unique ownership.
void validate_attachment_ownership(const MotionRuntime &runtime, const AttachmentLibrary &library,
                                   const AttachmentSet &attachments,
                                   const std::map<std::string, AttachmentSocket, std::less<>> &sockets,
                                   bool exclusive_primary = true);
std::string validate_attachment_action(const ActionRuntime &runtime, const AttachmentLibrary &library,
                                       const AttachmentSet &attachments, std::string_view action);
} // namespace anima
