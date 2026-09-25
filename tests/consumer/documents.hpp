#pragma once
#include <RmlUi/Core.h>
#include <anima/ui/document.hpp>
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
}
} // namespace documents_consumer
