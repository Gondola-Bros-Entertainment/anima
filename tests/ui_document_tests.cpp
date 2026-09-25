#include <anima/ui/document.hpp>
#ifdef TEST_UI_SCENE
#include "component_payloads.hpp"
#include <anima/prefab.hpp>
#include <anima/ui/scene.hpp>
#endif
#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementText.h>
#include <iostream>
#include <optional>
using namespace anima;
namespace {
void check(bool v, const char *m) {
    if (!v)
        throw std::runtime_error(m);
}
template <class F> void rejects(F f) {
    bool rejected = false;
    try {
        f();
    } catch (const std::exception &) {
        rejected = true;
    }
    check(rejected, "Expected UI rejection");
}
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
constexpr auto markup = "<rml><head><style>body { font-family: LatoLatin; }</style></head><body><div "
                        "id='container'><button id='action'>Test</button></div><input "
                        "id='input' type='text'/></body></rml>";
void run(const std::filesystem::path &file) {
    Renderer renderer;
    Rml::SetRenderInterface(&renderer);
    check(Rml::Initialise(), "Rml initialization failed");
    struct Quit {
        ~Quit() {
            Rml::Shutdown();
            Rml::SetRenderInterface(nullptr);
        }
    } quit;
    check(Rml::LoadFontFace((file.parent_path() / "LatoLatin-Regular.ttf").string()), "Font load failed");
    auto *context = Rml::CreateContext("documents-test", {640, 480});
    check(context, "Context missing");
    UiElement stale;
    UiDocument survivor;
    UiSubscription surviving_subscription;
    {
        UiDocuments host(*context);
        auto doc = host.from_memory(markup);
        auto button = doc.element("action");
        rejects([&] { (void)doc.element("absent"); });
        check(!doc.find("absent").valid(), "Missing optional element not empty");
        check(button.set_text("<literal & text>") && !button.set_text("<literal & text>"),
              "Literal text update failed");
        check(button.native().GetNumChildren() == 1 && dynamic_cast<Rml::ElementText *>(button.native().GetChild(0)),
              "Text was parsed as markup");
        doc.element("input").set_value("hello");
        check(doc.element("input").value() == "hello", "Form value failed");
        rejects([&] { button.set_value("bad"); });
        int clicks = 0;
        std::optional<UiEvent> retained;
        auto events = button.on("click", [&](const UiEvent &event) {
            ++clicks;
            retained = event;
            check(event.target().id() == "action", "Event target failed");
        });
        button.native().DispatchEvent("click", {});
        host.check_events();
        check(clicks == 1 && retained && !retained->valid(), "Event did not expire after callback");
        rejects([&] { (void)retained->type(); });
        events.disconnect();
        button.native().DispatchEvent("click", {});
        check(clicks == 1, "Disconnected event fired");
        UiSubscription self;
        self = button.on("click", [&](const UiEvent &) {
            ++clicks;
            self.disconnect();
        });
        button.native().DispatchEvent("click", {});
        button.native().DispatchEvent("click", {});
        check(clicks == 2 && !self.connected(), "Self-disconnect failed");
        auto failure = button.on("click", [](const UiEvent &) { throw std::runtime_error("event failure"); });
        button.native().DispatchEvent("click", {});
        rejects([&] { host.check_events(); });
        host.check_events();
        failure.disconnect();
        int stateful_count = 0;
        auto stateful =
            button.on("click", [count = 0, &stateful_count](const UiEvent &) mutable { stateful_count = ++count; });
        button.native().DispatchEvent("click", {});
        button.native().DispatchEvent("click", {});
        if (stateful_count != 2)
            throw std::runtime_error("Mutable callback count: " + std::to_string(stateful_count));
        stateful.disconnect();
        auto shutdown = button.on("click", [&](const UiEvent &) { host.shutdown(); });
        button.native().DispatchEvent("click", {});
        rejects([&] { host.check_events(); });
        check(doc.valid(), "Rejected callback shutdown changed host state");
        shutdown.disconnect();
        auto replaced = doc.element("action");
        auto replacement_listener = replaced.on("click", [](const UiEvent &) {});
        doc.element("container").set_markup("<button id='action'>Replacement</button>");
        check(!replaced.valid() && !replacement_listener.connected(), "Replaced node identity survived");
        rejects([&] { (void)replaced.id(); });
        auto closing = doc.root().on("click", [&](const UiEvent &) { doc.close(); });
        doc.element("action").native().DispatchEvent("click", {});
        host.check_events();
        check(!doc.valid() && !closing.connected(), "Close during dispatch failed");
        context->Update();
        auto moved = host.from_memory(markup);
        auto node = moved.element("action");
        auto destination = std::move(moved);
        check(!moved.valid() && destination.valid() && node.valid(), "Move lost document ownership");
        destination.close();
        check(!node.valid(), "Close did not invalidate elements immediately");
#ifdef TEST_UI_SCENE
        {
            Scene scene;
            ComponentCodecs codecs;
            add_ui_component_codec(codecs, host, [&](std::string_view key) {
                check(key == "controls", "Wrong asset key");
                return file;
            });
            auto root = scene.create();
            root.add_component<UiPanel>(host, "controls", file, true);
            auto source = Prefab::capture(root, codecs);
            auto copy = Prefab::deserialize(source.serialize({}), {}, codecs).instantiate(scene);
            check(&root.get_component<UiPanel>()->document().native() !=
                      &copy.get_component<UiPanel>()->document().native(),
                  "Prefab UI documents shared ownership");
            auto panel = copy.get_component<UiPanel>();
            auto element = panel->document().element("action");
            panel.set_enabled(false);
            sync_ui_panels(scene);
            check(!panel->document().visible(), "Disabled panel remained visible");
            panel.set_enabled(true);
            sync_ui_panels(scene);
            check(panel->document().visible(), "Panel reenable failed");
            auto parent = scene.create();
            copy.set_parent(parent);
            parent.set_active(false);
            sync_ui_panels(scene);
            check(panel.enabled() && !panel->document().visible(), "Inactive hierarchy retained UI panel");
            parent.set_active(true);
            sync_ui_panels(scene);
            check(panel->document().visible(), "Reactivated hierarchy did not restore UI panel");
            copy.destroy();
            parent.destroy();
            check(!element.valid(), "Component destruction left live UI handles");
            auto nodes = std::vector<Prefab::Node>(source.nodes().begin(), source.nodes().end());
            nodes.push_back(nodes[0]);
            nodes.back().key = {};
            nodes[1].parent = 0;
            const auto count = context->GetNumDocuments();
            for (const auto &payload : invalid_component_payloads(nodes[0].components[0].state, "visible")) {
                nodes[1].components[0].state = payload;
                rejects([&] { (void)Prefab(nodes, codecs).instantiate(scene); });
                check(scene.size() == 1 && context->GetNumDocuments() == count, "UI prefab rollback leaked document");
            }
        }
#endif
        survivor = host.from_memory(markup);
        stale = survivor.element("action");
        surviving_subscription = stale.on("click", [](const UiEvent &) {});
        host.shutdown();
        host.shutdown();
        check(!survivor.valid() && !stale.valid() && !surviving_subscription.connected(),
              "Host shutdown left live handles");
        rejects([&] { (void)host.from_memory(markup); });
    }
    Rml::RemoveContext("documents-test");
    // Handles intentionally outlive RmlUi globals. All backend observers must have been released.
    Rml::Shutdown();
    Rml::SetRenderInterface(&renderer);
    check(Rml::Initialise(), "Rml reinitialization failed");
    survivor.close();
    surviving_subscription.disconnect();
    rejects([&] { (void)stale.tag(); });
}
} // namespace
int main(int argc, char **argv) {
    try {
        check(argc == 2, "Expected document path");
        run(argv[1]);
        std::cout << "PASS checked UI documents, scoped events, shutdown and prefab lifetimes\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
