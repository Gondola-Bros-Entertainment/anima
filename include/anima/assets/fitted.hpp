#pragma once
#include <anima/assets/preview.hpp>
#include <anima/scene.hpp>
#include <map>
#include <set>
namespace anima {
// Fitted meshes share a declared body bind and never own animation.
struct FittedAsset {
    std::string slot;
    std::shared_ptr<const Asset> source;
    std::shared_ptr<const Mesh> render;
    std::vector<std::pair<std::size_t, std::size_t>> joints;
    FittedAsset(std::string_view slot, const Asset &body, std::shared_ptr<const Asset> fitted);
    Pose pose(const Pose &character) const;
};
struct FittedDefinition {
    std::string id, slot;
    std::filesystem::path model;
};
class FittedLibrary {
  public:
    FittedLibrary() = default;
    FittedLibrary(std::shared_ptr<const Asset> body, const Manifest &manifest, std::string_view profile,
                  std::string_view document);
    bool empty() const { return definitions_.empty(); }
    std::size_t size() const { return definitions_.size(); }
    bool contains(std::string_view id) const { return definitions_.contains(id); }
    bool known(std::string_view id) const { return known_ids_.contains(id); }
    void clear() {
        definitions_.clear();
        known_ids_.clear();
    }
    const auto &definitions() const { return definitions_; }
    const FittedDefinition &definition(std::string_view id) const;
    std::shared_ptr<const FittedAsset> load(std::string_view id) const;
    std::vector<std::shared_ptr<const Mesh>> resident_assets() const;

  private:
    friend class FittedSet;
    struct State;
    std::shared_ptr<State> state_;
    std::map<std::string, FittedDefinition, std::less<>> definitions_;
    std::set<std::string, std::less<>> known_ids_;
};
// Owns the fitted objects for one body. The scene and body must outlive mutation,
// but destruction is safe after either expires. No game slot/eligibility policy.
class FittedSet {
  public:
    struct Instance {
        std::string item;
        std::shared_ptr<const FittedAsset> asset;
        GameObject object;
    };
    FittedSet(GameObject owner, FittedLibrary library);
    ~FittedSet();
    FittedSet(const FittedSet &) = delete;
    FittedSet &operator=(const FittedSet &) = delete;
    // Preserves retained objects; failure leaves accepted membership unchanged.
    // New objects inherit the body's current pose, placement and visibility.
    void replace(const std::set<std::string, std::less<>> &items);
    // Call after publishing the final body pose/transform/visibility. Hidden sets
    // defer pose work until visible again. This does not drive body animation.
    void sync();
    void on_late_update(double) { sync(); }
    const std::vector<Instance> &instances() const { return instances_; }

  private:
    Scene &scene() const;
    GameObject owner_;
    std::shared_ptr<const Mesh> mesh_;
    FittedLibrary library_;
    std::vector<Instance> instances_;
};
} // namespace anima
