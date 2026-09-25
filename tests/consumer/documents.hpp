#pragma once
#include <RmlUi/Core.h>
#include <anima/ui/document.hpp>
#ifdef CONSUMER_UI_SCENE
#include <anima/scene_set.hpp>
#include <anima/ui/scene.hpp>
#include <chrono>
#include <fstream>
#endif
#include <stdexcept>
namespace documents_consumer {
class Renderer final : public Rml::RenderInterface {
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override {
        return 1;
    }
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override {}
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override {}
    Rml::TextureHandle LoadTexture(Rml::Vector2i &, const Rml::String &) override { return 0; }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override { return 1; }
    void ReleaseTexture(Rml::TextureHandle) override {}
    void EnableScissorRegion(bool) override {}
    void SetScissorRegion(Rml::Rectanglei) override {}
};
#ifdef CONSUMER_UI_SCENE
inline void scene_panels(anima::UiDocuments &documents) {
    struct Workspace {
        std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("anima-ui-scene-consumer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Workspace() {
            if (!std::filesystem::create_directory(path))
                throw std::runtime_error("Cannot create independent UI fixture directory");
        }
        ~Workspace() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } workspace;
    const auto file = workspace.path / "panel.rml";
    {
        std::ofstream output(file, std::ios::binary);
        output << "<rml><head><style>body { width: 40px; height: 40px; }</style></head>"
                  "<body><div id='marker'></div></body></rml>";
        output.close();
        if (!output)
            throw std::runtime_error("Cannot write independent UI fixture");
    }
    anima::SceneSet scenes;
    auto persistent = scenes.create("persistent"), level = scenes.create("level");
    auto hud = persistent->create(), parent = level->create(), object = level->create();
    object.set_parent(parent);
    auto first = hud.add_component<anima::UiPanel>(documents, "panel", file, false);
    auto second = object.add_component<anima::UiPanel>(documents, "panel", file, false);
    auto retained = second->document().element("marker");
    first->set_visible(true);
    second->set_visible(true);
    anima::sync_ui_panels(scenes);
    if (!first->document().visible() || !second->document().visible())
        throw std::runtime_error("Independent UI scene set failed to publish visibility");
    parent.set_active(false);
    anima::sync_ui_panels(scenes);
    if (!first->document().visible() || second->document().visible() || !second.enabled())
        throw std::runtime_error("Independent UI inherited activation leaked across scenes");
    parent.set_active(true);
    second.set_enabled(false);
    first->set_visible(false);
    anima::sync_ui_panels(scenes);
    if (first->document().visible() || second->document().visible())
        throw std::runtime_error("Independent UI ignored authored visibility or component enablement");
    first->set_visible(true);
    second.set_enabled(true);
    anima::sync_ui_panels(scenes);
    if (!first->document().visible() || !second->document().visible())
        throw std::runtime_error("Independent UI scene set failed to reactivate panels");
    scenes.unload(level);
    anima::sync_ui_panels(scenes);
    if (second || retained.valid() || !first->document().visible())
        throw std::runtime_error("Independent UI scene unload retained a document or affected another scene");
    auto survivor = first->document().root();
    scenes.clear();
    anima::sync_ui_panels(scenes);
    documents.check_events();
    if (first || survivor.valid())
        throw std::runtime_error("Independent UI scene clear retained live handles");
}
#endif
inline void run() {
    Renderer renderer;
    Rml::SetRenderInterface(&renderer);
    if (!Rml::Initialise())
        throw std::runtime_error("UI initialization failed");
    struct Quit {
        ~Quit() {
            Rml::Shutdown();
            Rml::SetRenderInterface(nullptr);
        }
    } quit;
    auto *context = Rml::CreateContext("external-documents", {100, 100});
    if (!context)
        throw std::runtime_error("UI context failed");
    anima::UiDocuments documents(*context);
    auto panel = documents.from_memory("<rml><head></head><body><button id='ok'>OK</button></body></rml>");
    auto button = panel.element("ok");
    int clicks = 0;
    auto listener = button.on("click", [&](const anima::UiEvent &event) {
        if (event.target().id() == "ok")
            ++clicks;
    });
    button.native().DispatchEvent("click", {});
    documents.check_events();
    panel.close();
    if (clicks != 1 || button.valid() || listener.connected())
        throw std::runtime_error("External document lifecycle failed");
#ifdef CONSUMER_UI_SCENE
    scene_panels(documents);
#endif
}
} // namespace documents_consumer
