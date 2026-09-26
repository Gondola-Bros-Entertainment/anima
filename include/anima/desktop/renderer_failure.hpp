#pragma once
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

/// @file
/// Opt-in failure injection for anima::VulkanRenderer lifecycle tests. Part of the `anima::desktop` target.
///
/// Production callers leave every `fail_after` option at anima::RendererFailureStage::none. Command-line or
/// wire spellings are converted with anima::parse_renderer_failure_stage at the caller's boundary.

namespace anima {
/// Point at which the renderer throws an injected failure.
///
/// The stages from `vertex` to `descriptors`, `upload_timeout` and `device_lost` fire only when a mesh is
/// uploaded, not when it is already cached, and `palette` only when the pose buffer grows. Unless a stage
/// says otherwise, it throws InjectedRendererFailure without making the renderer fatal.
enum class RendererFailureStage {
    none,           ///< No injection.
    instance,       ///< Construction, after creating the Vulkan instance.
    surface,        ///< Construction, after creating the window surface.
    device,         ///< Construction, after creating the logical device.
    resources,      ///< Construction, after frame resources and the initial selection.
    swapchain,      ///< Swapchain creation in draw(), after the first image's framebuffer.
    vertex,         ///< Preparation, after uploading a new mesh's vertex buffer.
    index,          ///< Preparation, after uploading a new mesh's index buffer.
    texture,        ///< Preparation, after allocating a new mesh's first texture image.
    texture_upload, ///< Preparation, after submitting a new mesh's first texture upload.
    descriptors,    ///< Preparation, after writing a new mesh's material descriptors.
    palette,        ///< VulkanRenderer::set_scenes, after allocating a larger pose buffer.
    ready,          ///< Preparation, after every upload and before a new selection is published.
    upload_timeout, ///< Preparation: a simulated texture upload timeout, which throws RendererFatalError.
    device_lost     ///< Preparation: simulated device loss at texture upload retirement; throws RendererFatalError.
};
/// Spellings of the RendererFailureStage values, indexed by enumerator: `none` is empty and underscores
/// become hyphens.
inline constexpr auto renderer_failure_names = std::to_array<std::string_view>(
    {"", "instance", "surface", "device", "resources", "swapchain", "vertex", "index", "texture", "texture-upload",
     "descriptors", "palette", "ready", "upload-timeout", "device-lost"});
/// Spelling of @p stage. Throws `std::out_of_range` for a value outside the enumeration.
constexpr std::string_view renderer_failure_name(RendererFailureStage stage) {
    return renderer_failure_names.at(static_cast<std::size_t>(stage));
}
/// Stage spelled @p name in renderer_failure_names; the empty string is `none`. Throws `std::invalid_argument`
/// for any other name.
inline RendererFailureStage parse_renderer_failure_stage(std::string_view name) {
    for (std::size_t i = 0; i < renderer_failure_names.size(); ++i)
        if (renderer_failure_names[i] == name)
            return static_cast<RendererFailureStage>(i);
    throw std::invalid_argument("Unknown renderer failure stage: " + std::string(name));
}
/// A failure injected at a RendererFailureStage. Classify it by stage(), not by its message.
class InjectedRendererFailure : public std::runtime_error {
  public:
    /// @p initial selects the initialization() result and the message wording.
    InjectedRendererFailure(RendererFailureStage stage, bool initial)
        : std::runtime_error(std::string(initial ? "Injected initialization failure after "
                                                 : "Injected scene replacement failure after ") +
                             std::string(renderer_failure_name(stage))),
          stage_(stage), initial_(initial) {}
    /// The stage that fired.
    RendererFailureStage stage() const noexcept { return stage_; }
    /// True when the failure was requested through RendererOptions::fail_after, false when through
    /// SceneReplacementOptions or ResourcePreparationOptions.
    bool initialization() const noexcept { return initial_; }

  private:
    RendererFailureStage stage_;
    bool initial_;
};
} // namespace anima
