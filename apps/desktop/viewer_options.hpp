#pragma once
#include <anima/desktop/vulkan_renderer.hpp>
#include <string_view>
namespace anima::viewer {
struct ViewerOptions {
    anima::RendererOptions renderer;
    std::uint64_t frames{};
    std::uint64_t timeout_seconds = 20;
    std::filesystem::path asset, manifest;
    std::string clip;
    double pose_time = 0;
    bool paused{};
    bool help{}, empty{};
};
bool parse_viewer_argument(ViewerOptions &, int &, int, char **);
ViewerOptions parse_viewer_options(int, char **);
void validate_viewer_options(ViewerOptions &);
std::string_view viewer_usage();
} // namespace anima::viewer
