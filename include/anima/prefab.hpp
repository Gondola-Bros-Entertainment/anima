#pragma once
#include <anima/scene.hpp>
#include <functional>

namespace anima {
// Stable application-owned keys, never serialized pointers or implicit paths.
using MeshName = std::function<std::string(const std::shared_ptr<const Mesh> &)>;
using MeshResolver = std::function<std::shared_ptr<const Mesh>(std::string_view)>;

class Prefab {
  public:
    struct Node {
        std::string name;
        std::optional<std::size_t> parent; // Earlier node; exactly one root at index 0.
        Mat4 local = identity();
        std::shared_ptr<const Mesh> mesh;
        std::optional<Pose> pose;
        bool visible = true;
        std::vector<Vec3> material_factors;  // Empty means mesh defaults.
        std::vector<bool> primitive_visible; // Empty means all visible.
        std::vector<ComponentData> components;
        bool active = true; // Authored object activation, independent of its parent.
        ObjectKey key;      // Authored identity within the document. Zero auto-assigns at construction.
    };
    // Retains a copy of destination-bound codecs for future instantiation.
    // Selecting another Scene does not rebind physics worlds, mixers or UI hosts.
    explicit Prefab(std::vector<Node> nodes, ComponentCodecs codecs = {});
    static Prefab capture(GameObject root, const ComponentCodecs &codecs = {});
    [[nodiscard]] std::span<const Node> nodes() const { return nodes_; }
    // Placement is multiplied by the authored root-local matrix.
    [[nodiscard]] GameObject instantiate(Scene &scene, const Mat4 &placement = identity()) const;
    [[nodiscard]] GameObject instantiate(GameObject parent, const Mat4 &placement = identity()) const;
    // Use these destination bindings for this instance without changing the
    // retained codecs. The registry is borrowed only for this call.
    // All component types validate before any objects are made;
    // decoder failure removes every staged object and its owned resources.
    [[nodiscard]] GameObject instantiate(Scene &scene, const Mat4 &placement, const ComponentCodecs &codecs) const;
    [[nodiscard]] GameObject instantiate(GameObject parent, const Mat4 &placement, const ComponentCodecs &codecs) const;
    [[nodiscard]] std::string serialize(const MeshName &name) const;
    static Prefab deserialize(std::string_view document, const MeshResolver &resolve, ComponentCodecs codecs = {});

  private:
    std::vector<Node> nodes_;
    ComponentCodecs codecs_;
    GameObject create(Scene &scene, const GameObject *parent, const Mat4 &placement,
                      const ComponentCodecs &codecs) const;
};

// A versioned forest of objects. Includes transforms, current poses, renderer
// overrides and explicitly registered component state. Excludes GPU residency.
[[nodiscard]] std::string serialize_scene(Scene &scene, const MeshName &name, const ComponentCodecs &codecs = {});
[[nodiscard]] std::shared_ptr<Scene> load_scene(std::string_view document, const MeshResolver &resolve,
                                                const ComponentCodecs &codecs = {});
} // namespace anima
