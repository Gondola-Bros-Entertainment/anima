#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

/// @file
/// Checked ownership of RmlUi documents, element handles and scoped event subscriptions.
///
/// Part of the optional `anima::ui_documents` target (`ANIMA_BUILD_UI_DOCUMENTS=ON`, also built by
/// `ANIMA_BUILD_UI=ON`), which needs no SDL, Vulkan or asset library. The target links RmlUi
/// privately, and this header includes no RmlUi header. RmlUi is built with its FreeType font
/// engine and without its SVG and Lottie plugins. Use a host and everything it owns from one
/// thread.
///
/// Handles are checked. Once a document, element or event handle expires, its members other
/// than valid() and UiDocument::close throw `std::out_of_range`; other failures are stated per
/// member. The `native()` escape hatches return borrowed RmlUi objects: do not keep those
/// references across DOM mutation, document close or host shutdown.

namespace Rml {
class Context;
class Element;
class ElementDocument;
} // namespace Rml
namespace anima {
class ComponentCodecs;
class UiElement;
class UiDocument;
class UiDocuments;
class UiSubscription;
class UiEvent;
namespace detail {
struct UiDocumentState;
struct UiElementState;
struct UiDocumentsState;
struct UiListener;
struct UiEventState;
} // namespace detail
/// Event callback, run synchronously inside RmlUi's event dispatch.
///
/// An exception it throws does not cross RmlUi. The host keeps the first one until
/// UiDocuments::check_events rethrows it and drops later ones, and an interruptible event stops
/// propagating at once.
using UiCallback = std::function<void(const UiEvent &)>;
/// Checked, non-owning handle to one element of a UiDocument.
///
/// It is valid while the element is in its document's tree and the document is open. Removing or
/// replacing the element, or closing its document (including by host shutdown), invalidates it.
/// A new element with the same ID does not revive the handle, but reattaching the same element to
/// that document does.
class UiElement {
  public:
    UiElement() = default;
    /// Whether the element is in its open document's tree.
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::string id() const;
    [[nodiscard]] std::string tag() const;
    /// Replaces the element's children with one text node holding @p text, which is never
    /// parsed as RML. Returns false, changing nothing, when the only child is already such a
    /// text node. Throws `std::runtime_error` when RmlUi cannot create the text node.
    bool set_text(std::string_view text);
    /// Replaces the element's children with the elements parsed from the RML @p rml.
    void set_markup(std::string_view rml);
    /// Sets the inline RCSS property @p name to @p value. Throws `std::invalid_argument` when
    /// RmlUi rejects the declaration.
    void set_property(std::string_view name, std::string_view value);
    /// Adds the class @p name when @p enabled, otherwise removes it.
    void set_class(std::string_view name, bool enabled);
    void set_attribute(std::string_view name, std::string_view value);
    /// Attribute @p name, or @p fallback when the element does not have it.
    [[nodiscard]] std::string attribute(std::string_view name, std::string_view fallback = {}) const;
    /// Sets the value of a form control (`input`, `select` or `textarea`). Throws
    /// `std::invalid_argument` for other elements.
    void set_value(std::string_view value);
    /// Value of a form control; throws as set_value() does.
    [[nodiscard]] std::string value() const;
    /// Calls @p callback for each @p type event, such as `click`, that reaches this element,
    /// until the returned subscription disconnects. With @p capture, the listener runs in the
    /// capture phase instead of the bubble phase; either way it runs when this element is the
    /// target. Throws `std::invalid_argument` for an empty @p type or @p callback.
    [[nodiscard]] UiSubscription on(std::string type, UiCallback callback, bool capture = false) const;
    /// The borrowed RmlUi element, for features without an Anima wrapper.
    [[nodiscard]] Rml::Element &native() const;

  private:
    friend class UiDocument;
    friend class UiEvent;
    explicit UiElement(std::shared_ptr<detail::UiElementState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::UiElementState> state_;
};
/// Checked view of the event being dispatched to a UiCallback.
///
/// The event and its copies expire when the callback returns.
class UiEvent {
  public:
    /// Whether the callback that received the event is still running.
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::string type() const;
    /// The element the event was dispatched to, possibly a descendant of the subscribed
    /// element. Throws `std::out_of_range` once its document has closed.
    [[nodiscard]] UiElement target() const;
    /// Event parameter @p name as an `int`, or @p fallback when the event has no such parameter.
    [[nodiscard]] int integer(std::string_view name, int fallback = 0) const;
    /// Event parameter @p name as a string, or @p fallback when the event has no such parameter.
    [[nodiscard]] std::string string(std::string_view name, std::string_view fallback = {}) const;
    /// Stops the event after the listeners of the current element, skipping later elements and
    /// RmlUi's default actions. RmlUi ignores it for events that are not interruptible, such as
    /// `show`, `hide`, `focus`, `blur` and `change`.
    void stop_propagation() const;

  private:
    friend struct detail::UiListener;
    explicit UiEvent(std::shared_ptr<detail::UiEventState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::UiEventState> state_;
};
/// Move-only token that keeps one UiElement::on listener attached.
///
/// Destroying the token, or assigning another to it, disconnects the listener, as do removing the
/// element and closing its document. A callback may disconnect its own subscription, replace its
/// element or close its document: the listener and callback stay alive until that invocation
/// returns.
class UiSubscription {
  public:
    UiSubscription() = default;
    ~UiSubscription();
    UiSubscription(UiSubscription &&) noexcept;
    UiSubscription &operator=(UiSubscription &&) noexcept;
    UiSubscription(const UiSubscription &) = delete;
    UiSubscription &operator=(const UiSubscription &) = delete;
    /// Detaches the listener. Idempotent.
    void disconnect();
    /// Whether the listener is attached to a live element of an open document.
    [[nodiscard]] bool connected() const noexcept;

  private:
    friend class UiElement;
    explicit UiSubscription(std::shared_ptr<detail::UiListener> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::UiListener> state_;
};
/// Move-only owner of one loaded RmlUi document; destroying or reassigning the owner closes it.
///
/// Element handles and subscriptions never extend the document's lifetime. The document stays
/// valid until close(), destruction, host shutdown or a close through native().
class UiDocument {
  public:
    UiDocument() = default;
    ~UiDocument();
    UiDocument(UiDocument &&) noexcept;
    UiDocument &operator=(UiDocument &&) noexcept;
    UiDocument(const UiDocument &) = delete;
    UiDocument &operator=(const UiDocument &) = delete;
    /// Whether this owns an open document whose host is running.
    [[nodiscard]] bool valid() const noexcept;
    /// Handle to the document element itself.
    [[nodiscard]] UiElement root() const;
    /// Handle to the first element with ID @p id in breadth-first order. Throws
    /// `std::out_of_range` when there is none.
    [[nodiscard]] UiElement element(std::string_view id) const;
    /// Like element(), but returns an empty handle when no element has ID @p id.
    [[nodiscard]] UiElement find(std::string_view id) const;
    /// Makes the document visible without modal state and focuses its first tab element with an
    /// `autofocus` attribute, or else the document. Its `show` callbacks run before this returns.
    void show();
    /// Hides the document and gives focus back to the previously focused document. Its `hide`
    /// callbacks run before this returns.
    void hide();
    /// Whether the document is shown and not `display: none`.
    [[nodiscard]] bool visible() const;
    /// Closes the document now: its element handles and subscriptions become invalid before
    /// RmlUi releases its storage, and those subscriptions do not receive its `unload` event.
    /// Idempotent, and safe inside the document's own callbacks.
    void close();
    /// The borrowed RmlUi document; see UiElement::native.
    [[nodiscard]] Rml::ElementDocument &native() const;

  private:
    friend class UiDocuments;
    explicit UiDocument(std::shared_ptr<detail::UiDocumentState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::UiDocumentState> state_;
};
/// Host that loads and owns documents in one borrowed, initialized RmlUi context.
///
/// Shut the host down, or destroy it, before destroying that context or shutting down RmlUi;
/// UiContext does so for its own host. Documents, element handles and subscriptions may outlive
/// the host, whose shutdown invalidates them. The host adds no rendering, input or context
/// updates: an application hosting documents without UiContext drives the native context itself
/// and then calls check_events().
class UiDocuments {
  public:
    /// Borrows @p context until shutdown().
    explicit UiDocuments(Rml::Context &context);
    /// Performs shutdown().
    ///
    /// Destroying the host inside one of its event callbacks, where shutdown() throws, writes a
    /// diagnostic to `stderr` and terminates the program instead: the event dispatch in progress
    /// resumes when the callback returns. Destroy it after the callback returns. A callback of
    /// another host may destroy it.
    ~UiDocuments();
    UiDocuments(const UiDocuments &) = delete;
    UiDocuments &operator=(const UiDocuments &) = delete;
    /// Loads the RML document at @p path, hidden, through RmlUi's file interface. Throws
    /// `std::runtime_error` when RmlUi returns no document and `std::out_of_range` after
    /// shutdown().
    [[nodiscard]] UiDocument load(const std::filesystem::path &path);
    /// Loads a hidden document from the RML text @p rml. @p source becomes its source URL, which
    /// resolves relative resource paths and names the document in RmlUi log messages. Throws as
    /// load() does.
    [[nodiscard]] UiDocument from_memory(std::string_view rml, std::string source = {});
    /// Rethrows, and clears, the first exception a UiCallback of this host has thrown since the
    /// last check. UiContext::process_event and UiContext::update call it for their host.
    void check_events();
    /// Closes every document of this host, invalidating their handles and subscriptions, and
    /// discards an unreported callback exception; later loads throw `std::out_of_range`.
    /// Idempotent. Throws `std::logic_error` inside one of this host's event callbacks, changing
    /// nothing.
    void shutdown();

  private:
    friend void add_ui_component_codec(ComponentCodecs &, UiDocuments &,
                                       std::function<std::filesystem::path(std::string_view)>);
    UiDocument own(Rml::ElementDocument *document);
    std::shared_ptr<detail::UiDocumentsState> state_;
    std::shared_ptr<int> lifetime_ = std::make_shared<int>(0);
};
} // namespace anima
