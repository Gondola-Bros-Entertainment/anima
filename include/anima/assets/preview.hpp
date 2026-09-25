#pragma once
#include <anima/animation.hpp>

namespace anima {
struct EquipmentMetadata {
    std::string id, slot, model, mesh, skeleton, socket;
    bool included{};
};
struct Manifest {
    std::filesystem::path directory;
    std::string asset_id, model, skeleton_id, bind_signature;
    std::size_t joint_count{};
    std::vector<ClipMetadata> clips;
    std::vector<EquipmentMetadata> equipment;
    // Optional checked sibling file consumed by the application's motion adapter.
    std::string motion_contract;
};
[[nodiscard]] Manifest read_manifest(const std::filesystem::path &path);
void validate_manifest(const Manifest &manifest, const Asset &asset);
// Returns equipment joint-node -> character joint-node pairs, checked by name, hierarchy and bind transforms.
[[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>> compatible_skin(const Asset &character,
                                                                               const Asset &equipment);
// A manifest-backed model viewer. Gameplay equipment assembly belongs to consumers.
class AssetPreview {
  public:
    explicit AssetPreview(const std::filesystem::path &manifest);
    AssetPreview(const AssetPreview &) = delete;
    AssetPreview &operator=(const AssetPreview &) = delete;
    AssetPreview(AssetPreview &&) noexcept = default;
    AssetPreview &operator=(AssetPreview &&) noexcept = default;
    void select(const std::string &clip, bool play = true);
    void bind_pose();
    void toggle_play();
    void restart();
    void seek(double time);
    [[nodiscard]] std::vector<ClipEvent> advance(double elapsed);
    [[nodiscard]] std::shared_ptr<const Scene> render_scene() const noexcept { return scene_; }
    [[nodiscard]] const Pose &pose() const noexcept { return animator_->pose(); }
    [[nodiscard]] const Playback &playback() const noexcept { return animator_->playback(); }
    [[nodiscard]] bool is_bind() const noexcept { return bind_; }
    [[nodiscard]] std::string status() const;
    [[nodiscard]] const Manifest &manifest() const noexcept { return manifest_; }

  private:
    Manifest manifest_;
    std::shared_ptr<const Asset> character_;
    std::shared_ptr<Scene> scene_ = std::make_shared<Scene>();
    ComponentRef<Animator> animator_;
    bool bind_{};
};
} // namespace anima
