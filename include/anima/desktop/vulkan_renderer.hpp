#pragma once
/// @file
/// Desktop rendering ownership, configuration and diagnostics.
#include <anima/desktop/renderer_failure.hpp>

#include <anima/environment.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

struct SDL_Window;

namespace anima {

class Scene;
class Mesh;
class MeshPreparation;
class UiContext;
namespace detail {
struct UiFrame;
}

/// Initial renderer configuration; no application effect or simulation is owned.
struct RendererOptions {
    bool validation = false;
    std::filesystem::path capture;
    /// Validation harness: throw after a named initialization stage.
    RendererFailureStage fail_after = RendererFailureStage::none;
    bool disable_present_fences = false;
    std::vector<std::shared_ptr<const Scene>> scenes;
    /// A missing/empty scene normally clears the background. The viewer opts into
    /// its diagnostic triangle explicitly; this flag never overrides nonempty geometry.
    bool diagnostic_triangle = false;
    /// Opt-in diagnostic timings. No query pool or CPU timing calls when disabled.
    bool profile = false;
    /// Uses the current view and conservative animated
    /// bounds; explicit scene/primitive visibility remains a separate control.
    bool frustum_culling = true;
};

/// Failure-injection settings used by independent renderer qualification.
struct SceneReplacementOptions {
    /// Validation harness only: vertex, index, texture, texture-upload, descriptors,
    /// palette, ready, upload-timeout or device-lost. None disables failure injection.
    RendererFailureStage fail_after = RendererFailureStage::none;
};

/// Failure-injection settings used by resource lifetime qualification.
struct ResourcePreparationOptions {
    /// Validation harness only: vertex, index, texture, texture-upload,
    /// descriptors, ready, upload-timeout or device-lost.
    RendererFailureStage fail_after = RendererFailureStage::none;
};

/// A fatal Vulkan failure after which drawing and replacement are illegal.
/// Shutdown must still run; a hung driver during retirement requires a watchdog.
class RendererFatalError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
/// Resource preparation failed before acquiring/submitting the next frame.
/// The renderer can be used again after the consumer repairs or clears its scene.
class SceneResourceError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/// Cumulative rendering and validation counters, including shutdown diagnostics.
struct RenderStats {
    std::uint64_t presented_frames{};
    std::uint32_t swapchain_generations{};
    std::uint32_t validation_warnings{};
    std::uint32_t validation_errors{};
    bool captured{};
    std::uint32_t capture_count{};
    std::uint32_t scene_generations{}; // Includes initial scene/empty state.
};

/// Opt-in elapsed CPU/GPU timing scopes, in milliseconds.
/// CPU/GPU intervals can overlap; summing them does not yield frame latency.
struct FrameProfile {
    double fence_wait_ms{}, upload_ms{}, acquire_ms{}, record_submit_ms{}, present_ms{};
    std::uint64_t uploaded_bytes{};
    /// Previous submitted frame, read only after its existing graphics fence.
    /// Covers command-buffer GPU execution, excluding presentation. False means
    /// unsupported/unavailable, not zero cost. CPU and GPU times may overlap.
    bool gpu_available{};
    double gpu_ms{};
    /// Same submitted frame: shadow, opaque scene/sky,
    /// display conversion/UI, and capture/transitions. Available under gpu_available.
    double gpu_shadow_ms{}, gpu_scene_ms{}, gpu_resolve_ms{}, gpu_transfer_ms{};
};

/// Selected resource residency and draw counters, not total device/process memory.
struct ResourceStats {
    /// Live material sampling policies shared across images and scenes.
    std::uint64_t resident_material_samplers{};
    std::uint64_t mesh_uploads{}, geometry_uploaded_bytes{};
    std::uint64_t resident_geometry_bytes{}, resident_texture_bytes{}, pose_buffer_bytes{};
    std::uint64_t pose_uploaded_bytes{}, draw_calls{}, instances{}, cached_assets{};
    /// Current prepared resource frame: candidates exclude explicit hidden/empty
    /// draws. Culled instances have no remaining main-view draws; instances counts
    /// main-view submissions. Pose bytes may also include offscreen shadow casters.
    /// Culling retains cached geometry and never alters the source scene.
    std::uint64_t candidate_instances{}, culled_instances{}, candidate_draws{}, culled_draws{}, submitted_indices{};
    std::uint64_t shadow_draw_calls{}, shadow_submitted_indices{}, shadow_bytes{};
    std::uint64_t world_target_bytes{}; ///< Scene-colour and depth allocations; excludes swapchain and shadows.
};

/// Owns the Vulkan device resources for one borrowed SDL window.
///
/// Use on the application's SDL video/render thread, with no concurrent calls or
/// mutation of the selected Scene during drawing/preparation. The window must
/// outlive the renderer. Settings and scene changes occur between draws.
///
/// The current pipeline provides built-in mesh shading, shadows, background and
/// display conversion. It does not expose custom shader programs or render passes.
/// Vulkan types stay private. Scene/environment methods require the optional
/// asset library; core-only applications need neither this class nor graphics.
class VulkanRenderer {
  public:
    /// Initialize rendering for a live SDL window created with SDL_WINDOW_VULKAN.
    /// Completed initialization stages are retired if construction throws.
    VulkanRenderer(SDL_Window *window, RendererOptions options);
    /// Retire owned resources; the caller remains responsible for the window.
    ~VulkanRenderer();
    VulkanRenderer(const VulkanRenderer &) = delete;
    VulkanRenderer &operator=(const VulkanRenderer &) = delete;
    /// Reconcile drawable/swapchain size on the next draw, without immediate work.
    void request_resize() noexcept;
    /// Request a PPM capture on a subsequent successful draw.
    /// @throws std::invalid_argument If the output path is empty.
    void request_capture(std::filesystem::path path);
    /// With asset rendering: a finite, invertible Vulkan view-projection. Derives the perspective eye or
    /// orthographic view direction for material highlights. Invalid input keeps
    /// the previous view intact.
    void set_view(const std::array<float, 16> &view_projection);
    /// Between draws; disabling supplies an unculled reference for diagnostics.
    void set_frustum_culling(bool enabled);
    /// Between draws; invalid input preserves the previous environment.
    void set_environment(const Environment &environment);
    /// Select scenes in submission order, or clear with an empty list. Null and
    /// duplicate scenes reject. Meshes are shared across the selection and cached
    /// while externally owned. Called between draws; preparation failure preserves
    /// the previous selection. Retained unloaded SceneSet views remain empty.
    void set_scenes(std::vector<std::shared_ptr<const Scene>> scenes, SceneReplacementOptions options = {});
    /// Synchronous preparation between draws, independent of scene selection.
    /// Repeated assets reuse the cache. Successful uploads remain cached while
    /// assets have external owners, including when a later upload fails. The
    /// selected scene/view/poses remain unchanged on success or recoverable failure.
    void prepare_meshes(std::span<const std::shared_ptr<const Mesh>> assets, ResourcePreparationOptions options = {});
    /// Uses CPU mip chains prepared off-thread. This call still owns synchronous
    /// GPU allocation/upload on the render thread; it does not retain the chains.
    void prepare_mesh(const MeshPreparation &preparation, ResourcePreparationOptions options = {});
    /// Read selected live resources and the most recently prepared frame counters.
    [[nodiscard]] ResourceStats resource_stats() const noexcept;
    /// Prepare, submit and present one frame; does not advance simulation.
    /// @return False if the drawable cannot currently be presented, for example
    /// while minimized; the caller continues its event loop and may retry.
    /// @throws SceneResourceError For recoverable incremental preparation failure.
    /// @throws RendererFatalError For a fatal Vulkan failure requiring shutdown.
    [[nodiscard]] bool draw();
    /// Read timing scopes. Check FrameProfile::gpu_available before using GPU data.
    [[nodiscard]] FrameProfile frame_profile() const noexcept;
    /// Retire GPU resources and return counters including cleanup validation.
    /// Repeated shutdown is permitted. Subsequent drawing/replacement is illegal.
    [[nodiscard]] RenderStats shutdown();

  private:
    friend class UiContext;
    [[nodiscard]] bool draw_ui(const detail::UiFrame &frame);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace anima
