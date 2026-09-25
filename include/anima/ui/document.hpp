#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
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
using UiCallback = std::function<void(const UiEvent &)>;
// Checked node identity, invalidated by removal/replacement, document close or host shutdown.
class UiElement {
  public:
    UiElement() = default;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::string id() const;
    [[nodiscard]] std::string tag() const;
    bool set_text(std::string_view text); // Literal text, never parsed as markup; reports change.
    void set_markup(std::string_view rml);
    void set_property(std::string_view name, std::string_view value);
    void set_class(std::string_view name, bool enabled);
    void set_attribute(std::string_view name, std::string_view value);
    [[nodiscard]] std::string attribute(std::string_view name, std::string_view fallback = {}) const;
    void set_value(std::string_view value); // Checked form control access.
    [[nodiscard]] std::string value() const;
    [[nodiscard]] UiSubscription on(std::string type, UiCallback callback, bool capture = false) const;
    [[nodiscard]] Rml::Element &native() const; // Borrowed escape hatch; never retain across mutation.
  private:
    friend class UiDocument;
    friend class UiEvent;
    explicit UiElement(std::shared_ptr<detail::UiElementState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::UiElementState> state_;
};
// Copies expire immediately after the callback returns; later access throws.
class UiEvent {
  public:
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::string type() const;
    [[nodiscard]] UiElement target() const;
    [[nodiscard]] int integer(std::string_view name, int fallback = 0) const;
    [[nodiscard]] std::string string(std::string_view name, std::string_view fallback = {}) const;
    void stop_propagation() const;

  private:
    friend struct detail::UiListener;
    explicit UiEvent(std::shared_ptr<detail::UiEventState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::UiEventState> state_;
};
// Move-only scoped subscription. Self-disconnect and closing a document inside a callback are safe.
class UiSubscription {
  public:
    UiSubscription() = default;
    ~UiSubscription();
    UiSubscription(UiSubscription &&) noexcept;
    UiSubscription &operator=(UiSubscription &&) noexcept;
    UiSubscription(const UiSubscription &) = delete;
    UiSubscription &operator=(const UiSubscription &) = delete;
    void disconnect();
    [[nodiscard]] bool connected() const noexcept;

  private:
    friend class UiElement;
    explicit UiSubscription(std::shared_ptr<detail::UiListener> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::UiListener> state_;
};
// Move-only document owner; destruction closes it. Elements never extend its lifetime.
class UiDocument {
  public:
    UiDocument() = default;
    ~UiDocument();
    UiDocument(UiDocument &&) noexcept;
    UiDocument &operator=(UiDocument &&) noexcept;
    UiDocument(const UiDocument &) = delete;
    UiDocument &operator=(const UiDocument &) = delete;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] UiElement root() const;
    [[nodiscard]] UiElement element(std::string_view id) const; // Throws for a missing ID.
    [[nodiscard]] UiElement find(std::string_view id) const;    // Empty for a missing ID.
    void show();
    void hide();
    [[nodiscard]] bool visible() const;
    void close(); // Idempotent; invalidates checked handles immediately.
    [[nodiscard]] Rml::ElementDocument &native() const;

  private:
    friend class UiDocuments;
    explicit UiDocument(std::shared_ptr<detail::UiDocumentState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::UiDocumentState> state_;
};
// Borrows an initialized native context. Shutdown this host before RmlUi context/global
// destruction. UiContext manages that ordering automatically. No SDL/GPU dependency.
class UiDocuments {
  public:
    explicit UiDocuments(Rml::Context &context);
    ~UiDocuments();
    UiDocuments(const UiDocuments &) = delete;
    UiDocuments &operator=(const UiDocuments &) = delete;
    [[nodiscard]] UiDocument load(const std::filesystem::path &path);
    [[nodiscard]] UiDocument from_memory(std::string_view rml, std::string source = {});
    void check_events(); // Rethrow first captured callback exception outside native dispatch.
    void shutdown();

  private:
    friend void add_ui_component_codec(ComponentCodecs &, UiDocuments &,
                                       std::function<std::filesystem::path(std::string_view)>);
    UiDocument own(Rml::ElementDocument *document);
    std::shared_ptr<detail::UiDocumentsState> state_;
    std::shared_ptr<int> lifetime_ = std::make_shared<int>(0);
};
} // namespace anima
