// RendererOptions::log without a window or device: a construction that the renderer rejects before it needs one still
// shuts down, and reports its final counters as it does.
#include <anima/desktop/vulkan_renderer.hpp>
#include <doctest/doctest.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace anima;
namespace {
constexpr auto no_window = "Renderer requires an SDL window";
}

TEST_CASE("A sink receives the final counters of a rejected construction as information, without a line break") {
    std::vector<std::pair<RendererLogLevel, std::string>> messages;
    RendererOptions options;
    options.log = [&messages](RendererLogLevel level, std::string_view text) { messages.emplace_back(level, text); };
    CHECK_THROWS_WITH_AS(VulkanRenderer(nullptr, options), no_window, std::invalid_argument);
    REQUIRE(messages.size() == 1);
    CHECK(messages.front().first == RendererLogLevel::info);
    CHECK(messages.front().second == "Renderer cleanup: validation_warnings=0 validation_errors=0");
}

TEST_CASE("Without a sink the renderer drops its information") {
    // An empty sink is never called, which would throw std::bad_function_call and terminate.
    CHECK_THROWS_WITH_AS(VulkanRenderer(nullptr, {}), no_window, std::invalid_argument);
}
