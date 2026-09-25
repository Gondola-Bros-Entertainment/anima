#pragma once
#include <anima/prefab.hpp>

namespace anima {
class SceneSet;
enum class CameraProjection { perspective, orthographic };

struct CameraSettings {
    CameraProjection projection = CameraProjection::perspective;
    float vertical_fov_degrees = 45;
    float orthographic_height = 10;
    float near_plane = .1F, far_plane = 1000;
};

/// Scene camera looking along local -Z, with local +Y up. Its world transform
/// must have orthogonal, nonzero, right-handed axes; positive scale is ignored.
class Camera {
  public:
    explicit Camera(CameraSettings settings = {});
    [[nodiscard]] const CameraSettings &settings() const { return settings_; }
    /// Validate all settings before publication, including the unused lens field.
    void configure(CameraSettings settings);

  private:
    CameraSettings settings_;
};

/// Authored view selection. Multiple cameras are allowed; exactly one active
/// CameraView is required by view_matrix. A null link is persistable but cannot
/// produce a view. Selection addresses an object's current Camera attachment.
struct CameraView {
    CameraView() = default;
    GameObject camera;
};

/// Resolve one active view and its active camera within the supplied selection.
/// Idle scenes only, after updates and before drawing. Does not advance phases,
/// mutate a renderer or follow SceneSet::active. Missing/ambiguous/stale/foreign
/// selections and invalid poses/aspects reject without a fallback camera.
/// Returns a column-major Vulkan projection (Y down, depth 0..1).
[[nodiscard]] Mat4 view_matrix(Scene &scene, float aspect);
[[nodiscard]] Mat4 view_matrix(SceneSet &scenes, float aspect);

/// Explicit strict v1 lens and object-link codecs inside scene/prefab v3.
/// View links remap through ObjectReferences; cross-document links reject.
void add_camera_component_codecs(ComponentCodecs &codecs);
} // namespace anima
