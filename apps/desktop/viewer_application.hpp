#pragma once
#include "viewer_options.hpp"
#include <chrono>
#include <stdexcept>
namespace anima {
class AssetPreview;
struct OrbitCamera;
} // namespace anima
namespace anima::viewer {
// SDL has no video device, as without a display, or cannot load a Vulkan driver for the viewer's window.
// run_viewer() throws it and RendererUnavailableError instead of reporting them and returning 1, so a caller can
// tell a system without a display or Vulkan device from a failure.
class DisplayUnavailable : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
struct ViewerResult {
    std::uint64_t frames{}, ticks{}, suspended_iterations{};
    unsigned pixel_events{}, minimize_events{}, restore_events{}, camera_updates{}, preview_events{};
    bool timed_out{};
    RenderStats renderer;
};
struct ViewerFrame {
    SDL_Window *window;
    VulkanRenderer &renderer;
    std::uint64_t frames;
    std::chrono::steady_clock::time_point now;
    double seconds;
    AssetPreview *preview;
    OrbitCamera *camera;
    unsigned &camera_updates;
};
class ViewerDriver {
  public:
    virtual ~ViewerDriver() = default;
    virtual void before_draw(const ViewerFrame &) {}
    virtual double playback_seconds(double elapsed) const { return elapsed; }
    virtual bool passed(const ViewerResult &) const { return true; }
};
int run_viewer(ViewerOptions, ViewerDriver * = nullptr);
} // namespace anima::viewer
