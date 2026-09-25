#pragma once
#include "viewer_options.hpp"
#include <chrono>
namespace anima {
class AssetPreview;
struct OrbitCamera;
} // namespace anima
namespace anima::viewer {
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
