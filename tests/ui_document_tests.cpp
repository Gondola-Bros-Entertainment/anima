#include <anima/ui/document.hpp>
#ifdef TEST_UI_SCENE
#include "component_payloads.hpp"
#include <anima/prefab.hpp>
#include <anima/scene_set.hpp>
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
#ifdef TEST_UI_SCENE
template <class F> bool driver_rejected(F f) noexcept {
    try {
        f();
    } catch (const std::logic_error &) {
        return true;
    } catch (...) {
    }
    return false;
}
struct PanelDriverChecks {
    bool rejected = true;
    unsigned calls{};
};
struct PanelDriverProbe {
    Scene &scene;
    SceneSet &scenes;
    PanelDriverChecks &checks;
    PanelDriverProbe(Scene &owner, SceneSet &selection, PanelDriverChecks &results)
        : scene(owner), scenes(selection), checks(results) {
        attempt();
    }
    void attempt() noexcept {
        ++checks.calls;
        checks.rejected &= driver_rejected([&] { sync_ui_panels(scene); });
        checks.rejected &= driver_rejected([&] { sync_ui_panels(scenes); });
    }
    void on_enable() noexcept { attempt(); }
    void on_disable() noexcept { attempt(); }
    void on_update(double) { attempt(); }
    void on_fixed_update(double) { attempt(); }
    void on_late_update(double) { attempt(); }
};
void panel_selection(UiDocuments &host, const std::filesystem::path &file) {
    SceneSet scenes;
    auto first = scenes.create("first"), second = scenes.create("second");
    auto object = first->create(), parent = second->create(), child = second->create();
    child.set_parent(parent);
    auto panel = object.add_component<UiPanel>(host, "controls", file);
    auto other = child.add_component<UiPanel>(host, "controls", file);
    panel.set_enabled(false);
    parent.set_active(false);
    sync_ui_panels(scenes);
    check(!panel->document().visible() && other.enabled() && !other->document().visible(),
          "Scene set did not reconcile UI enablement and inherited activation");
    panel.set_enabled(true);
    parent.set_active(true);
    other->set_visible(false);
    sync_ui_panels(scenes);
    check(panel->document().visible() && !other->document().visible(),
          "Scene set did not preserve authored panel visibility");
    other->set_visible(true);
    sync_ui_panels(scenes);
    check(other->document().visible(), "Scene set did not restore panel visibility");

    panel->set_visible(false);
    other->document().close();
    rejects([&] { sync_ui_panels(scenes); });
    check(panel->document().visible(), "Expired later document partially published earlier visibility");
    // A rejected snapshot must release both the set and each selected scene.
    scenes.update(0);
    first->fixed_update(0);
    auto recovered = scenes.create("recovered");
    scenes.unload(recovered);
    child.remove_component<UiPanel>();
    sync_ui_panels(scenes);
    check(!panel->document().visible(), "Panel driver remained locked after rejected snapshot");
    scenes.unload(second);
    sync_ui_panels(scenes);
    scenes.clear();
    sync_ui_panels(scenes);
}
void panel_reentry(UiDocuments &host, const std::filesystem::path &file) {
    PanelDriverChecks checks;
    SceneSet scenes;
    auto first = scenes.create("first"), second = scenes.create("second");
    auto object = first->create();
    auto panel = object.add_component<UiPanel>(host, "controls", file, false);
    bool complete_set = true;
    unsigned events = 0;
    const auto attempt = [&](const UiEvent &) {
        ++events;
        check(driver_rejected([&] { sync_ui_panels(first.get()); }), "UI event reentered single-scene driver");
        check(driver_rejected([&] { sync_ui_panels(scenes); }), "UI event reentered scene-set driver");
        check(driver_rejected([&] { first->update(0); }), "UI event entered scene update");
        check(driver_rejected([&] { scenes.fixed_update(0); }), "UI event entered scene-set scheduling");
        check(driver_rejected([&] { (void)scenes.create("nested"); }), "UI event changed set membership");
        if (complete_set)
            check(driver_rejected([&] { second->update(0); }), "UI event entered another selected scene");
    };
    auto show = panel->document().root().on("show", attempt);
    auto hide = panel->document().root().on("hide", attempt);
    panel->set_visible(true);
    sync_ui_panels(scenes);
    host.check_events();
    check(events == 1 && panel->document().visible(), "Panel show event did not run");
    complete_set = false;
    panel->set_visible(false);
    sync_ui_panels(first.get());
    host.check_events();
    check(events == 2 && !panel->document().visible(), "Panel hide event did not run");
    scenes.update(0);
    first->fixed_update(0);
    (void)scenes.create("after-events");

    object.add_component<PanelDriverProbe>(first.get(), scenes, checks);
    scenes.update(0);
    scenes.fixed_update(0);
    check(checks.calls == 5 && checks.rejected, "Panel driver entered component construction or scheduling");
    // Retire the probe while its result storage and borrowed scene still exist.
    object.remove_component<PanelDriverProbe>();
    check(checks.calls == 6 && checks.rejected, "Panel driver entered component retirement");
}
void panel_event_mutations(UiDocuments &host, const std::filesystem::path &file) {
    for (const bool showing : {true, false}) {
        SceneSet scenes;
        auto scene = scenes.create("self-removal");
        auto object = scene->create();
        auto panel = object.add_component<UiPanel>(host, "controls", file, !showing);
        auto root = panel->document().root();
        unsigned events = 0;
        auto removal = root.on(showing ? "show" : "hide", [&](const UiEvent &) {
            ++events;
            object.remove_component<UiPanel>();
        });
        panel->set_visible(showing);
        sync_ui_panels(scenes);
        host.check_events();
        check(events == 1 && !panel && !root.valid() && !removal.connected(),
              "Panel self-removal during visibility event retained resources");
        sync_ui_panels(scenes);
    }
    {
        SceneSet scenes;
        auto scene = scenes.create("subtree-removal");
        auto object = scene->create(), child = scene->create();
        child.set_parent(object);
        auto panel = object.add_component<UiPanel>(host, "controls", file, false);
        auto nested = child.add_component<UiPanel>(host, "controls", file, false);
        auto root = panel->document().root(), child_root = nested->document().root();
        unsigned events = 0, child_events = 0;
        auto destruction = root.on("show", [&](const UiEvent &) {
            ++events;
            object.destroy();
        });
        auto skipped = child_root.on("show", [&](const UiEvent &) { ++child_events; });
        panel->set_visible(true);
        nested->set_visible(true);
        sync_ui_panels(scenes);
        host.check_events();
        check(events == 1 && child_events == 0 && scene->size() == 0 && !object.valid() && !child.valid() && !panel &&
                  !nested && !root.valid() && !child_root.valid() && !destruction.connected() && !skipped.connected(),
              "Object destruction during visibility dispatch retained a subtree or visited retired panels");
        sync_ui_panels(scenes);
    }
    {
        SceneSet scenes;
        auto first = scenes.create("first"), second = scenes.create("second");
        auto object = first->create(), later = second->create();
        auto panel = object.add_component<UiPanel>(host, "controls", file, false);
        auto other = later.add_component<UiPanel>(host, "controls", file, false);
        auto removal = panel->document().root().on("show", [&](const UiEvent &) { later.remove_component<UiPanel>(); });
        panel->set_visible(true);
        other->set_visible(true);
        sync_ui_panels(scenes);
        host.check_events();
        check(panel->document().visible() && !other, "Visibility event did not safely remove a later panel");
    }
    {
        SceneSet scenes;
        auto first = scenes.create("first"), second = scenes.create("second");
        auto panel = first->create().add_component<UiPanel>(host, "controls", file, false);
        auto later = second->create();
        auto old = later.add_component<UiPanel>(host, "controls", file, false);
        auto old_root = old->document().root();
        ComponentRef<UiPanel> replacement;
        auto replace = panel->document().root().on("show", [&](const UiEvent &) {
            later.remove_component<UiPanel>();
            replacement = later.add_component<UiPanel>(host, "controls", file, false);
            replacement->set_visible(true);
        });
        panel->set_visible(true);
        old->set_visible(true);
        sync_ui_panels(scenes);
        host.check_events();
        check(!old && !old_root.valid() && replacement && !replacement->document().visible(),
              "Visibility snapshot retargeted a removed attachment to its replacement");
        sync_ui_panels(scenes);
        host.check_events();
        check(replacement->document().visible(), "Replacement panel did not participate in the next snapshot");
    }
    {
        SceneSet scenes;
        auto first = scenes.create("first"), second = scenes.create("second");
        auto panel = first->create().add_component<UiPanel>(host, "controls", file);
        auto later = second->create();
        auto other = later.add_component<UiPanel>(host, "controls", file);
        auto closure = panel->document().root().on("hide", [&](const UiEvent &) { other->document().close(); });
        panel->set_visible(false);
        other->set_visible(false);
        sync_ui_panels(scenes);
        host.check_events();
        check(!panel->document().visible() && other.valid() && !other->document().valid(),
              "Visibility event did not safely close a later document");
        later.remove_component<UiPanel>();
        sync_ui_panels(scenes);
    }
    {
        SceneSet scenes;
        auto first = scenes.create("first"), second = scenes.create("second");
        auto panel = first->create().add_component<UiPanel>(host, "controls", file, false);
        ComponentRef<UiPanel> added;
        auto addition = panel->document().root().on("show", [&](const UiEvent &) {
            added = second->create().add_component<UiPanel>(host, "controls", file, false);
            added->set_visible(true);
        });
        panel->set_visible(true);
        sync_ui_panels(scenes);
        host.check_events();
        check(added && !added->document().visible(), "New panel participated in an existing visibility snapshot");
        sync_ui_panels(scenes);
        host.check_events();
        check(added->document().visible(), "New panel did not participate in the next visibility snapshot");
    }
}
#endif
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
        panel_selection(host, file);
        panel_reentry(host, file);
        panel_event_mutations(host, file);
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
