#include <RmlUi/Core.h>
#include <anima/ui/document.hpp>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <string_view>

// Each scenario destroys a UiDocuments host from inside an event callback. Destroying the host whose
// callback is running must terminate during that destruction, deliberately and with no exception in
// flight; returning, terminating elsewhere or terminating because an exception escaped the destructor
// fails the scenario. The other_host scenario destroys a different, idle host from a callback, which
// must not terminate. Every scenario ends its process, so CTest runs each one separately instead of a
// doctest suite.
namespace {
using namespace anima;

class Renderer final : public Rml::RenderInterface {
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override {
        return 1;
    }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i &size, const Rml::String &) override {
        size = {1, 1};
        return 1;
    }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return 1; }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}
};

constexpr auto markup = "<rml><head></head><body><div id='target'></div></body></rml>";
constexpr int context_width = 640;
constexpr int context_height = 480;

std::unique_ptr<UiDocuments> owned_host;
std::unique_ptr<UiDocuments> idle_host;
bool destroying = false;
bool expect_termination = true;

template <class Owner> void destroy(Owner &owner) {
    destroying = true;
    owner.reset();
    destroying = false;
}

// Loads a document into owned_host and clicks its target, whose callback destroys @p victim.
void click_destroying(Rml::Context &context, std::unique_ptr<UiDocuments> &victim) {
    owned_host = std::make_unique<UiDocuments>(context);
    auto document = owned_host->from_memory(markup);
    auto target = document.element("target");
    auto subscription = target.on("click", [&victim](const UiEvent &) { destroy(victim); });
    target.native().DispatchEvent("click", {});
    owned_host->check_events();
}

const std::map<std::string_view, void (*)(Rml::Context &)> scenarios{
    {"own_callback", [](Rml::Context &context) { click_destroying(context, owned_host); }},
    {"other_host",
     [](Rml::Context &context) {
         expect_termination = false;
         idle_host = std::make_unique<UiDocuments>(context);
         click_destroying(context, idle_host);
     }},
};

[[noreturn]] void on_terminate() noexcept {
    // std::terminate entered because of a throw leaves that exception current; a deliberate call does not.
    const auto escaped = std::current_exception();
    const bool passed = destroying && expect_termination && !escaped;
    if (passed)
        std::fputs("PASS terminated during destruction\n", stderr);
    else if (!escaped)
        std::fputs("FAIL unexpected termination\n", stderr);
    else {
        try {
            std::rethrow_exception(escaped);
        } catch (const std::exception &error) {
            std::fprintf(stderr, "FAIL terminated because an exception escaped: %s\n", error.what());
        } catch (...) {
            std::fputs("FAIL terminated because a non-standard exception escaped\n", stderr);
        }
    }
    // _Exit skips the destructors and RmlUi shutdown that the abandoned host would trip.
    std::_Exit(passed ? EXIT_SUCCESS : EXIT_FAILURE);
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 2 || !scenarios.contains(argv[1])) {
        std::fputs("usage: anima_ui_destruction_tests <scenario>\n", stderr);
        return EXIT_FAILURE;
    }
    std::set_terminate(on_terminate);
    Renderer renderer;
    Rml::SetRenderInterface(&renderer);
    if (!Rml::Initialise()) {
        std::fputs("FAIL RmlUi initialization\n", stderr);
        return EXIT_FAILURE;
    }
    auto *context = Rml::CreateContext("destruction-test", {context_width, context_height});
    if (!context) {
        std::fputs("FAIL RmlUi context creation\n", stderr);
        return EXIT_FAILURE;
    }
    scenarios.at(argv[1])(*context);
    if (expect_termination) {
        std::fputs("FAIL destruction returned instead of terminating\n", stderr);
        return EXIT_FAILURE;
    }
    owned_host.reset();
    Rml::Shutdown();
    Rml::SetRenderInterface(nullptr);
    std::puts("PASS destroying an idle host from a callback");
}
