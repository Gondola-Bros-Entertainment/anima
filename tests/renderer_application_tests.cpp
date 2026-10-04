// RendererOptions::application_name and RendererOptions::application_version without a window or device: construction
// checks both before it needs a window, so an accepted value reaches the window check and a rejected one does not.
#include <anima/desktop/vulkan_renderer.hpp>
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

using namespace anima;
namespace {
constexpr auto no_window = "Renderer requires an SDL window";
constexpr auto invalid_name = "Application name must be UTF-8 without a null character";
constexpr auto invalid_version =
    "Application version must have a major of at most 127, a minor of at most 1023 and a patch of at most 4095";

RendererOptions named(std::string name) {
    RendererOptions options;
    options.application_name = std::move(name);
    return options;
}

RendererOptions versioned(std::array<std::uint32_t, 3> version) {
    RendererOptions options;
    options.application_version = version;
    return options;
}
} // namespace

TEST_CASE("The application is reported as Anima 0.0.0 unless the options name it") {
    const RendererOptions options;
    CHECK(options.application_name == "Anima");
    CHECK(options.application_version == std::array<std::uint32_t, 3>{0, 0, 0});
}

TEST_CASE("A UTF-8 application name and a version within Vulkan's fields are accepted") {
    // Two, three and four byte sequences at the edges of their ranges.
    CHECK_THROWS_WITH_AS(
        VulkanRenderer(nullptr, named("Caf\xc3\xa9 \xe3\x82\xb2\xe3\x83\xbc\xe3\x83\xa0 \xf0\x9f\x8e\xae")), no_window,
        std::invalid_argument);
    CHECK_THROWS_WITH_AS(VulkanRenderer(nullptr, named("\xc2\x80\xe0\xa0\x80\xed\x9f\xbf\xee\x80\x80\xf0\x90\x80\x80"
                                                       "\xf4\x8f\xbf\xbf")),
                         no_window, std::invalid_argument);
    CHECK_THROWS_WITH_AS(VulkanRenderer(nullptr, named("")), no_window, std::invalid_argument);
    CHECK_THROWS_WITH_AS(VulkanRenderer(nullptr, versioned({127, 1023, 4095})), no_window, std::invalid_argument);
}

TEST_CASE("An application name with a null character or malformed UTF-8 is rejected") {
    using namespace std::string_literals;
    const std::string names[] = {
        "Game\0Name"s,       // a null character, which would end the name that Vulkan reads
        "\x80"s,             // a continuation byte without a lead
        "\xc0\xaf"s,         // an overlong two byte form
        "\xe0\x80\xaf"s,     // an overlong three byte form
        "\xf0\x80\x80\xaf"s, // an overlong four byte form
        "\xed\xa0\x80"s,     // a surrogate
        "\xf4\x90\x80\x80"s, // above U+10FFFF
        "\xf5\x80\x80\x80"s, // a lead byte that no sequence uses
        "\xe3\x81"s,         // a truncated sequence
        "\xe3\x41\x81"s,     // a sequence interrupted by ASCII
    };
    for (const auto &name : names) {
        CAPTURE(name);
        CHECK_THROWS_WITH_AS(VulkanRenderer(nullptr, named(name)), invalid_name, std::invalid_argument);
    }
}

TEST_CASE("An application version beyond Vulkan's packed fields is rejected") {
    CHECK_THROWS_WITH_AS(VulkanRenderer(nullptr, versioned({128, 0, 0})), invalid_version, std::invalid_argument);
    CHECK_THROWS_WITH_AS(VulkanRenderer(nullptr, versioned({0, 1024, 0})), invalid_version, std::invalid_argument);
    CHECK_THROWS_WITH_AS(VulkanRenderer(nullptr, versioned({0, 0, 4096})), invalid_version, std::invalid_argument);
}
