#pragma once

#include <RmlUi/Core.h>
#include <SDL3/SDL_events.h>
#include <anima/desktop/vulkan_renderer.hpp>
#include <anima/ui/document.hpp>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace anima {
struct UiInputResult {
    bool consumed{};   // Suppress this event's gameplay action when true.
    bool pointer{};    // UI currently owns pointer interaction.
    bool keyboard{};   // A document element currently has keyboard focus.
    bool text{};       // SDL text input is active for this UI.
    bool focus_lost{}; // Clear held gameplay actions, even if consumed is false.
};
struct UiStats {
    std::uint64_t updates{}, rendered_frames{};
    std::uint32_t log_warnings{}, log_errors{};
};
class UiUnsupportedFeature : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

// Optional anima::ui target, ANIMA_BUILD_UI=ON. Borrows renderer and SDL window;
// destroy/shutdown UI before renderer, then window, then SDL. Single-threaded.
// Owns RmlUi's process globals: one live UiContext;
// do not independently initialise/shut down RmlUi or replace its interfaces.
class UiContext {
  public:
    UiContext(SDL_Window *window, VulkanRenderer &renderer, std::string name = "anima");
    ~UiContext();
    UiContext(const UiContext &) = delete;
    UiContext &operator=(const UiContext &) = delete;

    // Native interoperability. The borrowed context reference expires on shutdown;
    // open_document and its checked handles provide document ownership.
    [[nodiscard]] Rml::Context &context();
    [[nodiscard]] UiDocuments &documents();
    [[nodiscard]] UiDocument open_document(const std::filesystem::path &path);
    void load_font(const std::filesystem::path &path, bool fallback = false);

    // Feed SDL events before gameplay dispatch. Window events are also forwarded
    // to renderer resize handling. UTF-8 text, modifiers, focus and DPI are handled.
    [[nodiscard]] UiInputResult process_event(const SDL_Event &event);
    [[nodiscard]] UiInputResult input_state() const;
    // Refreshes drawable/DPI metrics and updates RmlUi layout/time. RML sizes in dp
    // follow display scaling; px values are physical framebuffer pixels.
    void update();
    // Records UI, then renders ONE combined scene+UI frame in the existing pass.
    // Call instead of renderer.draw(), after update(). False if not drawable.
    // Unsupported advanced render features fail explicitly before GPU acquisition.
    [[nodiscard]] bool render();
    [[nodiscard]] UiStats stats() const;
    void shutdown(); // Idempotent. All borrowed RmlUi handles become invalid.

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace anima
