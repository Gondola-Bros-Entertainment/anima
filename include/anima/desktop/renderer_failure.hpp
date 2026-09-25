#pragma once
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

namespace anima {
// Explicit opt-in fault injection for renderer lifecycle verification.
// Wire/CLI spellings are converted at the caller boundary.
enum class RendererFailureStage {
    none,
    instance,
    surface,
    device,
    resources,
    swapchain,
    vertex,
    index,
    texture,
    texture_upload,
    descriptors,
    palette,
    ready,
    upload_timeout,
    device_lost
};
inline constexpr auto renderer_failure_names = std::to_array<std::string_view>(
    {"", "instance", "surface", "device", "resources", "swapchain", "vertex", "index", "texture", "texture-upload",
     "descriptors", "palette", "ready", "upload-timeout", "device-lost"});
constexpr std::string_view renderer_failure_name(RendererFailureStage stage) {
    return renderer_failure_names.at(static_cast<std::size_t>(stage));
}
inline RendererFailureStage parse_renderer_failure_stage(std::string_view name) {
    for (std::size_t i = 0; i < renderer_failure_names.size(); ++i)
        if (renderer_failure_names[i] == name)
            return static_cast<RendererFailureStage>(i);
    throw std::invalid_argument("Unknown renderer failure stage: " + std::string(name));
}
class InjectedRendererFailure : public std::runtime_error {
  public:
    InjectedRendererFailure(RendererFailureStage stage, bool initial)
        : std::runtime_error(std::string(initial ? "Injected initialization failure after "
                                                 : "Injected scene replacement failure after ") +
                             std::string(renderer_failure_name(stage))),
          stage_(stage), initial_(initial) {}
    RendererFailureStage stage() const noexcept { return stage_; }
    bool initialization() const noexcept { return initial_; }

  private:
    RendererFailureStage stage_;
    bool initial_;
};
} // namespace anima
