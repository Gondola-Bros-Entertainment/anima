#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <algorithm>
#include <anima/ui/document.hpp>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <utility>
#include <vector>

namespace anima::detail {
struct UiDocumentsState {
    Rml::Context *context{};
    std::vector<std::weak_ptr<UiDocumentState>> documents;
    std::exception_ptr error;
    unsigned dispatch_depth{};
};
struct UiElementState {
    std::weak_ptr<UiDocumentState> document;
    Rml::ObserverPtr<Rml::Element> element;
};
struct UiDocumentState : std::enable_shared_from_this<UiDocumentState> {
    std::weak_ptr<UiDocumentsState> host;
    Rml::ObserverPtr<Rml::Element> document;
    std::vector<std::weak_ptr<UiElementState>> elements;
    std::vector<std::weak_ptr<UiListener>> listeners;
    bool closed{};
    bool valid() const {
        auto owner = host.lock();
        return !closed && owner && owner->context && document && document->GetParentNode();
    }
    std::shared_ptr<UiElementState> element(Rml::Element *node) {
        auto result = std::make_shared<UiElementState>();
        result->document = weak_from_this();
        result->element = node->GetObserverPtr();
        std::erase_if(elements, [](const auto &entry) { return entry.expired(); });
        elements.push_back(result);
        return result;
    }
    void close();
};
struct UiEventState {
    Rml::Event *event{};
    std::weak_ptr<UiDocumentState> document;
    Rml::Event &get() const {
        if (!event)
            throw std::out_of_range("Expired UI event");
        return *event;
    }
};
struct UiListener final : Rml::EventListener, std::enable_shared_from_this<UiListener> {
    std::weak_ptr<UiDocumentState> document;
    Rml::ObserverPtr<Rml::Element> element;
    std::string type;
    std::shared_ptr<UiCallback> callback;
    bool capture{}, attached{};
    void disconnect() {
        if (attached && element) {
            attached = false;
            element->RemoveEventListener(type, this, capture);
        }
        element.reset();
        callback = {};
    }
    void OnDetach(Rml::Element *) override {
        attached = false;
        element.reset();
    }
    void ProcessEvent(Rml::Event &event) override {
        const auto self = shared_from_this(); // Pins listener during self-disconnect/document close.
        const auto doc = document.lock();
        const auto host = doc ? doc->host.lock() : nullptr;
        if (!attached || !doc || !doc->valid() || !host || !callback)
            return;
        auto invocation = callback;
        auto state = std::make_shared<UiEventState>();
        state->event = &event;
        state->document = doc;
        ++host->dispatch_depth;
        try {
            (*invocation)(UiEvent(state));
        } catch (...) {
            if (!host->error)
                host->error = std::current_exception();
            event.StopImmediatePropagation();
        }
        state->event = nullptr;
        --host->dispatch_depth;
    }
};
void UiDocumentState::close() {
    if (closed)
        return;
    closed = true;
    for (const auto &entry : listeners)
        if (auto listener = entry.lock())
            listener->disconnect();
    listeners.clear();
    for (const auto &entry : elements)
        if (auto element = entry.lock())
            element->element.reset();
    elements.clear();
    // Clear observers before native Close dispatches any user/native unload listeners.
    auto *node = static_cast<Rml::ElementDocument *>(document.get());
    document.reset();
    if (node)
        node->Close();
}
} // namespace anima::detail
namespace anima {
bool UiElement::valid() const noexcept {
    if (!state_ || !state_->element)
        return false;
    const auto doc = state_->document.lock();
    if (!doc || !doc->valid())
        return false;
    for (auto *node = state_->element.get(); node; node = node->GetParentNode())
        if (node == doc->document.get())
            return true;
    return false;
}
Rml::Element &UiElement::native() const {
    if (!valid())
        throw std::out_of_range("Expired UI element");
    return *state_->element.get();
}
std::string UiElement::id() const { return native().GetId(); }
std::string UiElement::tag() const { return native().GetTagName(); }
bool UiElement::set_text(std::string_view text) {
    auto &node = native();
    if (node.GetNumChildren() == 1)
        if (auto *child = dynamic_cast<Rml::ElementText *>(node.GetChild(0))) {
            if (child->GetText() == text)
                return false;
            child->SetText(std::string(text));
            return true;
        }
    auto child = node.GetOwnerDocument()->CreateTextNode(std::string(text));
    if (!child)
        throw std::runtime_error("Unable to create UI text node");
    node.SetInnerRML("");
    node.AppendChild(std::move(child));
    return true;
}
void UiElement::set_markup(std::string_view rml) { native().SetInnerRML(std::string(rml)); }
void UiElement::set_property(std::string_view name, std::string_view value) {
    if (!native().SetProperty(std::string(name), std::string(value)))
        throw std::invalid_argument("Invalid UI property");
}
void UiElement::set_class(std::string_view name, bool enabled) { native().SetClass(std::string(name), enabled); }
void UiElement::set_attribute(std::string_view name, std::string_view value) {
    native().SetAttribute(std::string(name), std::string(value));
}
std::string UiElement::attribute(std::string_view name, std::string_view fallback) const {
    return native().GetAttribute<std::string>(std::string(name), std::string(fallback));
}
namespace {
detail::UiEventState &checked_event(const std::shared_ptr<detail::UiEventState> &state) {
    if (!state)
        throw std::out_of_range("Expired UI event");
    return *state;
}
Rml::ElementFormControl &control(Rml::Element &node) {
    auto *form = dynamic_cast<Rml::ElementFormControl *>(&node);
    if (!form)
        throw std::invalid_argument("UI element is not a form control");
    return *form;
}
} // namespace
void UiElement::set_value(std::string_view value) { control(native()).SetValue(std::string(value)); }
std::string UiElement::value() const { return control(native()).GetValue(); }
UiSubscription UiElement::on(std::string type, UiCallback callback, bool capture) const {
    if (type.empty() || !callback)
        throw std::invalid_argument("UI subscription needs event type and callback");
    auto &node = native();
    auto doc = state_->document.lock();
    auto listener = std::make_shared<detail::UiListener>();
    listener->document = doc;
    listener->element = node.GetObserverPtr();
    listener->type = std::move(type);
    listener->callback = std::make_shared<UiCallback>(std::move(callback));
    listener->capture = capture;
    std::erase_if(doc->listeners, [](const auto &entry) { return entry.expired(); });
    doc->listeners.push_back(listener);
    node.AddEventListener(listener->type, listener.get(), capture);
    listener->attached = true;
    return UiSubscription(std::move(listener));
}
bool UiEvent::valid() const noexcept { return state_ && state_->event; }
std::string UiEvent::type() const { return checked_event(state_).get().GetType(); }
UiElement UiEvent::target() const {
    auto *target = checked_event(state_).get().GetTargetElement();
    auto doc = state_->document.lock();
    if (!doc || !doc->valid() || !target)
        throw std::out_of_range("Expired UI event target");
    return UiElement(doc->element(target));
}
int UiEvent::integer(std::string_view name, int fallback) const {
    return checked_event(state_).get().GetParameter<int>(std::string(name), fallback);
}
std::string UiEvent::string(std::string_view name, std::string_view fallback) const {
    return checked_event(state_).get().GetParameter<std::string>(std::string(name), std::string(fallback));
}
void UiEvent::stop_propagation() const { checked_event(state_).get().StopPropagation(); }
UiSubscription::~UiSubscription() { disconnect(); }
UiSubscription::UiSubscription(UiSubscription &&other) noexcept : state_(std::move(other.state_)) {}
UiSubscription &UiSubscription::operator=(UiSubscription &&other) noexcept {
    if (this != &other) {
        disconnect();
        state_ = std::move(other.state_);
    }
    return *this;
}
void UiSubscription::disconnect() {
    if (state_)
        state_->disconnect();
    state_.reset();
}
bool UiSubscription::connected() const noexcept {
    const auto doc = state_ ? state_->document.lock() : nullptr;
    return state_ && state_->attached && state_->element && doc && doc->valid();
}
UiDocument::~UiDocument() { close(); }
UiDocument::UiDocument(UiDocument &&other) noexcept : state_(std::move(other.state_)) {}
UiDocument &UiDocument::operator=(UiDocument &&other) noexcept {
    if (this != &other) {
        close();
        state_ = std::move(other.state_);
    }
    return *this;
}
bool UiDocument::valid() const noexcept { return state_ && state_->valid(); }
Rml::ElementDocument &UiDocument::native() const {
    if (!valid())
        throw std::out_of_range("Expired UI document");
    return *static_cast<Rml::ElementDocument *>(state_->document.get());
}
UiElement UiDocument::root() const {
    auto &node = native();
    return UiElement(state_->element(&node));
}
UiElement UiDocument::find(std::string_view id) const {
    auto *node = native().GetElementById(std::string(id));
    return node ? UiElement(state_->element(node)) : UiElement{};
}
UiElement UiDocument::element(std::string_view id) const {
    auto result = find(id);
    if (!result.valid())
        throw std::out_of_range("Missing UI element: " + std::string(id));
    return result;
}
void UiDocument::show() { native().Show(); }
void UiDocument::hide() { native().Hide(); }
bool UiDocument::visible() const { return native().IsVisible(); }
void UiDocument::close() {
    auto state = std::move(state_);
    if (state)
        state->close();
}
UiDocuments::UiDocuments(Rml::Context &context) : state_(std::make_shared<detail::UiDocumentsState>()) {
    state_->context = &context;
}
UiDocuments::~UiDocuments() {
    // shutdown() refuses to run inside this host's callbacks because the dispatch beneath them resumes when
    // they return; for a UiContext's host, on a native context that the UiContext would already have shut
    // down. A destructor cannot refuse, so it terminates, as ~Scene does.
    if (state_->dispatch_depth) {
        std::fputs("UI document host destroyed inside one of its event callbacks, directly or through its "
                   "UiContext; destroy it after the callback returns\n",
                   stderr);
        std::terminate();
    }
    shutdown();
}
UiDocument UiDocuments::own(Rml::ElementDocument *document) {
    if (!document)
        throw std::runtime_error("Unable to load UI document");
    try {
        auto state = std::make_shared<detail::UiDocumentState>();
        state->host = state_;
        state->document = document->GetObserverPtr();
        std::erase_if(state_->documents, [](const auto &entry) { return entry.expired(); });
        state_->documents.push_back(state);
        return UiDocument(std::move(state));
    } catch (...) {
        document->Close();
        throw;
    }
}
UiDocument UiDocuments::load(const std::filesystem::path &path) {
    if (!state_->context)
        throw std::out_of_range("UI document host is shut down");
    const auto text = path.u8string();
    return own(state_->context->LoadDocument(std::string(text.begin(), text.end())));
}
UiDocument UiDocuments::from_memory(std::string_view rml, std::string source) {
    if (!state_->context)
        throw std::out_of_range("UI document host is shut down");
    return own(state_->context->LoadDocumentFromMemory(std::string(rml), std::move(source)));
}
void UiDocuments::check_events() {
    if (auto error = std::exchange(state_->error, {}))
        std::rethrow_exception(error);
}
void UiDocuments::shutdown() {
    if (!state_->context)
        return;
    if (state_->dispatch_depth)
        throw std::logic_error("Cannot shut down UI host during an event callback");
    // Mark closed before native unload callbacks can try to load more documents.
    state_->context = nullptr;
    auto documents = std::move(state_->documents);
    for (const auto &entry : documents)
        if (auto doc = entry.lock())
            doc->close();
    state_->error = {};
}
} // namespace anima
