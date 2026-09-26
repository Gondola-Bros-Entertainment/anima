#pragma once

#include <RmlUi/Core.h>
#include <SDL3/SDL_events.h>
#include <anima/desktop/vulkan_renderer.hpp>
#include <anima/ui/document.hpp>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

/// @file
/// Desktop UI: RmlUi documents that take SDL3 input and draw through the VulkanRenderer.
///
/// Part of the optional `anima::ui` target (`ANIMA_BUILD_UI=ON`, which requires
/// `ANIMA_BUILD_DESKTOP=ON`). RmlUi's public headers and core library propagate to consumers for
/// native interoperability. Use a UiContext and everything it hosts from one thread.
///
/// RCSS `px` lengths are framebuffer pixels, and `dp` lengths scale with the SDL display scale.
/// UI colors and images are treated as sRGB and blended in linear light over the display-converted
/// scene, so scene exposure and tone mapping do not affect them.

namespace anima {
/// Outcome of UiContext::process_event for one SDL event, with the capture state after it.
struct UiInputResult {
    /// The UI used this event, so gameplay should not also apply it. A press keeps its owner
    /// until release: pointer events are consumed while a press that began on a document is held
    /// and never while one that reached gameplay is held. The release of a key whose press
    /// reached gameplay is never consumed.
    bool consumed{};
    /// The window has input focus and the UI owns the pointer: a press that began on a document
    /// is held, or no gameplay press is held and the pointer is over or pressing a document
    /// element. Hit testing includes each visible document's own box and ignores transparency;
    /// RCSS `pointer-events: none` excludes an element.
    bool pointer{};
    /// The window has input focus and a visible `textarea`, `select` or `input` has focus, so key
    /// events are consumed. Inputs of type `button`, `submit`, `checkbox` and `radio`, like other
    /// focusable elements, take focus without capturing the keyboard.
    bool keyboard{};
    /// The keyboard is captured and SDL text input is active for the window. Text and IME
    /// composition events reach the UI, and are consumed, only while this is set.
    bool text{};
    /// The event was this window losing input focus. Clear held gameplay actions even though
    /// #consumed is false.
    bool focus_lost{};
};
/// Counters of a UiContext since construction.
struct UiStats {
    /// Completed UiContext::update calls.
    std::uint64_t updates{};
    /// UiContext::render calls that presented a frame.
    std::uint64_t rendered_frames{};
    /// Warnings RmlUi logged.
    std::uint32_t log_warnings{};
    /// Errors and failed assertions RmlUi logged.
    std::uint32_t log_errors{};
};
/// Thrown by UiContext::render when a document needs a render feature or resource that the UI
/// backend does not provide.
///
/// Unsupported features: render layers, filters and backdrop effects, mask images, custom and
/// gradient shaders, and clip masks, which RmlUi uses to clip to rounded borders or through
/// transforms. Rejected resources: non-finite or malformed geometry, non-finite transforms, more
/// than 2,000,000 triangle vertices in one frame, textures over 8,192 pixels on either axis, and
/// image files that are missing, empty, over 64 MiB or not PNG or JPEG. The failure is sticky:
/// every later render() throws it again, so shut the context down and create a new one after
/// fixing the document.
class UiUnsupportedFeature : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/// Desktop UI runtime that owns RmlUi's process-wide state and one RmlUi context.
///
/// Only one UiContext may be live at a time; another may be created after shutdown(). While one
/// is live, do not initialize or shut down RmlUi or replace its interfaces. The context borrows
/// its window and renderer: shut it down before shutting down the renderer or destroying the
/// window, and destroy it before `SDL_Quit`. Destroying a live context inside one of its event
/// callbacks terminates the program.
///
/// After shutdown(), every member except stats() and shutdown() throws `std::logic_error`. The
/// context sets the SDL cursor from the RCSS `cursor` property, uses the SDL clipboard and writes
/// RmlUi log messages to `std::cerr`.
class UiContext {
  public:
    /// Initializes RmlUi and creates the RmlUi context @p name for @p window, which @p renderer
    /// must present to. Blending in linear light needs an sRGB presentation format, so the
    /// surface of @p renderer must offer one; this is checked before RmlUi is initialized.
    ///
    /// Throws `std::invalid_argument` for a null window or empty name, `std::logic_error` while
    /// another UiContext is live or after @p renderer shuts down, RendererFatalError when
    /// @p renderer has already failed or cannot query its surface, and `std::runtime_error` when
    /// the surface offers no sRGB format or when RmlUi or SDL fails; a failed construction releases
    /// what it initialized.
    UiContext(SDL_Window *window, VulkanRenderer &renderer, std::string name = "anima");
    ~UiContext();
    UiContext(const UiContext &) = delete;
    UiContext &operator=(const UiContext &) = delete;

    /// The native RmlUi context, for features without an Anima wrapper such as data models.
    [[nodiscard]] Rml::Context &context();
    /// The host that owns documents opened through this context; pass it to UiPanel and
    /// add_ui_component_codec. It lives until this context is destroyed.
    [[nodiscard]] UiDocuments &documents();
    /// Loads a hidden document from @p path through documents(); see UiDocuments::load.
    [[nodiscard]] UiDocument open_document(const std::filesystem::path &path);
    /// Loads a font face for documents; RmlUi reads its family, style and weight from the file.
    /// A @p fallback face also supplies glyphs that other faces lack. Text renders only in loaded
    /// faces. Throws `std::runtime_error` when RmlUi cannot load the face.
    void load_font(const std::filesystem::path &path, bool fallback = false);

    /// Routes one SDL event to the UI. Call it for every event before gameplay sees it.
    ///
    /// Events for other windows and unhandled types, such as touch and gamepad events, are
    /// ignored. Window pixel-size, display-scale, display and restore events refresh the UI
    /// metrics and call VulkanRenderer::request_resize. Pass pointer coordinates as SDL reports
    /// them; the context scales them by the window's pixel density.
    ///
    /// A press on a document captures the SDL mouse until its buttons are released; hiding or
    /// closing that document cancels the press, ends the capture and consumes the release. A
    /// press outside every document blurs the focused control. Losing window focus releases every
    /// press and key in RmlUi, blurs the focused control and stops text input; pointer and key
    /// events are then ignored until focus returns.
    ///
    /// Throws `std::invalid_argument` for non-finite or out-of-range pointer coordinates and
    /// `std::runtime_error` when SDL reports invalid display metrics. After applying the event, it
    /// rethrows a captured callback exception; see UiDocuments::check_events.
    [[nodiscard]] UiInputResult process_event(const SDL_Event &event);
    /// Current capture state without an event; UiInputResult::consumed and
    /// UiInputResult::focus_lost are false.
    [[nodiscard]] UiInputResult input_state() const;
    /// Advances the UI by one frame: refreshes display metrics and focus state, then updates
    /// RmlUi layout, animations and data models. Call it once per frame after processing events
    /// and before render().
    ///
    /// It cancels presses on hidden or closed documents, blurs a focused element that became
    /// hidden and stops SDL text input when nothing captures the keyboard. Throws
    /// `std::runtime_error` when SDL reports invalid display metrics. After the RmlUi update, it
    /// rethrows a captured callback exception; see UiDocuments::check_events.
    void update();
    /// Draws the renderer's selected scenes with the UI composited over them and presents one
    /// frame. Call it instead of VulkanRenderer::draw, after update().
    ///
    /// Returns false without presenting when the renderer cannot present, as VulkanRenderer::draw
    /// does, or when the UI layout does not yet match the swapchain size; the next update() and
    /// render() retry. Before acquiring a swapchain image, it throws UiUnsupportedFeature when a
    /// document needs an unsupported render feature. Throws `std::runtime_error` if the surface
    /// stops offering an sRGB format after construction, `std::invalid_argument` when a UI
    /// texture exceeds the device's image size limit, and otherwise fails as VulkanRenderer::draw
    /// does.
    [[nodiscard]] bool render();
    [[nodiscard]] UiStats stats() const;
    /// Shuts down the document host, then RmlUi: every document, checked handle and borrowed
    /// RmlUi reference becomes invalid, and SDL text input and mouse capture stop. Idempotent;
    /// the destructor calls it. Throws `std::logic_error` inside an event callback, changing
    /// nothing.
    void shutdown();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace anima
