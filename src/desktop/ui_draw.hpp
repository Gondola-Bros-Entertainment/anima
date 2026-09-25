#pragma once
// Private renderer bridge; no Vulkan or RmlUi types cross this boundary.
#include <array>
#include <cstdint>
#include <memory>
#include <vector>
namespace anima::detail {
struct UiVertex {
    float x{}, y{};
    std::array<std::uint8_t, 4> color{};
    float u{}, v{};
};
struct UiTexture {
    std::uint32_t width{}, height{};
    std::vector<std::uint8_t> rgba; // Premultiplied, sRGB-encoded RGB; linear alpha.
};
struct UiBatch {
    std::uint32_t first{}, count{};
    std::shared_ptr<const UiTexture> texture;
    std::array<float, 16> transform{};
    std::array<float, 2> translation{};
    bool clipped{};
    std::array<int, 4> clip{}; // left, top, right, bottom in framebuffer coordinates.
};
struct UiFrame {
    std::vector<UiVertex> vertices; // Expanded triangles simplify safe streaming.
    std::vector<UiBatch> batches;
    int width{}, height{};
};
} // namespace anima::detail
