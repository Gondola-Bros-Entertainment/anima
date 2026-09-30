#include <anima/ui/document.hpp>
#ifdef TEST_UI_SCENE
#include "component_payloads.hpp"
#include <anima/prefab.hpp>
#include <anima/scene_set.hpp>
#include <anima/ui/scene.hpp>
#endif
#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace anima;
namespace {
// The controls document that CTest passes, beside the LatoLatin font it uses.
std::filesystem::path controls;

constexpr auto expired_element = "Expired UI element";
constexpr auto expired_event = "Expired UI event";
constexpr auto host_shut_down = "UI document host is shut down";

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
constexpr auto select_markup = "<rml><head><style>body { font-family: LatoLatin; }</style></head><body><select "
                               "id='size'/><input id='input' type='text'/></body></rml>";

// The options of @p dropdown as "value=label" entries, requiring each label to be the option's one text node.
std::vector<std::string> option_entries(Rml::ElementFormControlSelect &dropdown) {
    std::vector<std::string> entries;
    for (int index = 0; index < dropdown.GetNumOptions(); ++index) {
        auto &option = *dropdown.GetOption(index);
        REQUIRE(option.GetNumChildren() == 1);
        const auto *text = dynamic_cast<Rml::ElementText *>(option.GetChild(0));
        REQUIRE(text != nullptr);
        entries.push_back(option.GetAttribute<Rml::String>("value", {}) + "=" + text->GetText());
    }
    return entries;
}
// The text that @p dropdown shows for its selection, which RmlUi copies from the selected option's RML.
std::string shown_value(Rml::Element &dropdown) {
    for (int index = 0; index < dropdown.GetNumChildren(true); ++index)
        if (auto &child = *dropdown.GetChild(index); child.GetTagName() == "selectvalue") {
            REQUIRE(child.GetNumChildren() == 1);
            const auto *text = dynamic_cast<Rml::ElementText *>(child.GetChild(0));
            REQUIRE(text != nullptr);
            return text->GetText();
        }
    FAIL("The select has no selectvalue element");
    return {};
}

// Initializes RmlUi with a stub renderer and the test font for one test case, and owns its context. Declare it
// before any UiDocuments so hosts are destroyed first.
class Session {
  public:
    Session() {
        Rml::SetRenderInterface(&renderer_);
        REQUIRE(Rml::Initialise());
        REQUIRE(Rml::LoadFontFace((controls.parent_path() / "LatoLatin-Regular.ttf").string()));
        context_ = Rml::CreateContext(context_name, {640, 480});
        REQUIRE(context_ != nullptr);
    }
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    ~Session() {
        if (context_)
            Rml::RemoveContext(context_name);
        Rml::Shutdown();
        Rml::SetRenderInterface(nullptr);
    }
    Rml::Context &context() { return *context_; }

  private:
    static constexpr auto context_name = "documents-test";
    Renderer renderer_;
    Rml::Context *context_{};
};

#ifdef TEST_UI_SCENE
constexpr auto busy_scene = "Scene drivers require an idle live scene";
constexpr auto nested_updates = "Component updates cannot be nested";
constexpr auto member_callbacks = "A member scene is running callbacks";
constexpr auto set_updating = "Scene set is updating";
constexpr auto set_driven = "Scene set is held by a scene driver";

// Whether @p f throws std::logic_error with @p message; for noexcept component hooks, where an assertion
// cannot report directly.
template <class F> bool driver_rejected(F f, std::string_view message) noexcept {
    try {
        f();
    } catch (const std::logic_error &error) {
        return error.what() == message;
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
        attempt(member_callbacks);
    }
    // @p set_error is what the scene set reports: that it is updating while its update runs this hook, else that
    // a member scene is running callbacks.
    void attempt(std::string_view set_error) noexcept {
        ++checks.calls;
        checks.rejected &= driver_rejected([&] { sync_ui_panels(scene); }, busy_scene);
        checks.rejected &= driver_rejected([&] { sync_ui_panels(scenes); }, set_error);
    }
    void on_enable() noexcept { attempt(set_updating); }
    void on_disable() noexcept { attempt(member_callbacks); }
    void on_update(double) { attempt(set_updating); }
    void on_fixed_update(double) { attempt(set_updating); }
    void on_late_update(double) { attempt(set_updating); }
};
struct Payload {
    std::string state, error;
};
// invalid_component_payloads(valid, "visible") with their errors, in its order: an empty object, which lacks the
// codec's first field, then "visible" renamed, an unknown field, "visible" duplicated, "visible" duplicated
// through an escape and nesting past the limit.
std::vector<Payload> invalid_payloads(const std::string &valid) {
    const auto states = invalid_component_payloads(valid, "visible");
    const std::string errors[]{"Missing JSON field: asset",      "Missing JSON field: visible",
                               "Unknown JSON field: unexpected", "Duplicate JSON document field",
                               "Duplicate JSON document field",  "JSON document exceeds nesting limit"};
    REQUIRE(states.size() == std::size(errors));
    std::vector<Payload> result;
    for (std::size_t i = 0; i < states.size(); ++i)
        result.push_back({states[i], errors[i]});
    return result;
}
#endif
} // namespace

TEST_CASE("Documents find elements and set literal text and form values") {
    Session session;
    UiDocuments host(session.context());
    auto doc = host.from_memory(markup);
    auto button = doc.element("action");
    CHECK_THROWS_WITH_AS(doc.element("absent"), "Missing UI element: absent", std::out_of_range);
    CHECK_FALSE(doc.find("absent").valid());
    CHECK(button.set_text("<literal & text>"));
    CHECK_FALSE(button.set_text("<literal & text>"));
    // The text is one text node, not parsed markup.
    CHECK(button.native().GetNumChildren() == 1);
    CHECK(dynamic_cast<Rml::ElementText *>(button.native().GetChild(0)) != nullptr);
    doc.element("input").set_value("hello");
    CHECK(doc.element("input").value() == "hello");
    CHECK_THROWS_WITH_AS(button.set_value("bad"), "UI element is not a form control", std::invalid_argument);
}

TEST_CASE("Select options replace the list, keep a surviving value and dispatch one change event") {
    Session session;
    UiDocuments host(session.context());
    auto doc = host.from_memory(select_markup);
    auto size = doc.element("size");
    auto &control = dynamic_cast<Rml::ElementFormControlSelect &>(size.native());
    // Markup children that RmlUi has not yet moved into its option list, one of them marked selected, and a value
    // that no current option has. Setting the attribute dispatches `change` before the listener exists.
    size.set_markup("<option value='m'>Old medium</option><option value='l' selected>Old large</option>");
    size.set_attribute("value", "m");
    // Each event's value, and the option count its callback saw.
    std::vector<std::string> changes;
    std::vector<int> counts;
    auto listener = size.on("change", [&](const UiEvent &event) {
        changes.push_back(event.string("value"));
        counts.push_back(control.GetNumOptions());
        CHECK(size.value() == changes.back());
    });

    const std::vector<UiOption> sizes{{"s", "Small"}, {"m", "<b>Medium</b> & \"more\""}, {"l", "Large"}};
    CHECK_THROWS_WITH_AS(doc.element("input").set_options(sizes), "UI element is not a select", std::invalid_argument);
    CHECK_THROWS_WITH_AS(doc.root().set_options(sizes), "UI element is not a select", std::invalid_argument);
    size.set_options(sizes);
    CHECK(option_entries(control) == std::vector<std::string>{"s=Small", "m=<b>Medium</b> & \"more\"", "l=Large"});
    CHECK(size.value() == "m");
    CHECK(control.GetSelection() == 1);
    CHECK(changes == std::vector<std::string>{"m"});
    CHECK(counts == std::vector<int>{3});
    // RmlUi's update agrees with the selection, dispatching nothing, and shows the label literally.
    session.context().Update();
    CHECK(changes.size() == 1);
    CHECK(control.GetSelection() == 1);
    CHECK(shown_value(size.native()) == "<b>Medium</b> & \"more\"");

    // Without the selected value, the first option is selected.
    size.set_options(std::vector<UiOption>{{"x", "Extra"}, {"y", ""}});
    CHECK(option_entries(control) == std::vector<std::string>{"x=Extra", "y="});
    CHECK(size.value() == "x");
    CHECK(control.GetSelection() == 0);
    CHECK(changes == std::vector<std::string>{"m", "x"});
    CHECK(counts == std::vector<int>{3, 2});

    // A duplicate value changes nothing.
    CHECK_THROWS_WITH_AS(size.set_options(std::vector<UiOption>{{"a", "A"}, {"b", "B"}, {"a", "C"}}),
                         "Duplicate UI option value: a", std::invalid_argument);
    CHECK(option_entries(control) == std::vector<std::string>{"x=Extra", "y="});
    CHECK(size.value() == "x");
    CHECK(changes.size() == 2);

    // An unchanged value still dispatches `change`, and no options leave an empty value.
    size.set_options(std::vector<UiOption>{{"y", "Why"}, {"x", "Ex"}});
    CHECK(control.GetSelection() == 1);
    size.set_options({});
    CHECK(control.GetNumOptions() == 0);
    CHECK(control.GetSelection() == -1);
    CHECK(size.value().empty());
    CHECK(changes == std::vector<std::string>{"m", "x", "x", ""});
    CHECK(counts == std::vector<int>{3, 2, 2, 0});
    host.check_events();

    doc.close();
    CHECK_THROWS_WITH_AS(size.set_options(sizes), expired_element, std::out_of_range);
}

TEST_CASE("Event subscriptions expire, disconnect and report callback failures") {
    Session session;
    UiDocuments host(session.context());
    auto doc = host.from_memory(markup);
    auto button = doc.element("action");
    int clicks = 0;
    std::optional<UiEvent> retained;
    auto events = button.on("click", [&](const UiEvent &event) {
        ++clicks;
        retained = event;
        CHECK(event.target().id() == "action");
    });
    button.native().DispatchEvent("click", {});
    host.check_events();
    CHECK(clicks == 1);
    REQUIRE(retained);
    CHECK_FALSE(retained->valid());
    CHECK_THROWS_WITH_AS(retained->type(), expired_event, std::out_of_range);
    events.disconnect();
    button.native().DispatchEvent("click", {});
    CHECK(clicks == 1);

    UiSubscription self;
    self = button.on("click", [&](const UiEvent &) {
        ++clicks;
        self.disconnect();
    });
    button.native().DispatchEvent("click", {});
    button.native().DispatchEvent("click", {});
    CHECK(clicks == 2);
    CHECK_FALSE(self.connected());

    // check_events rethrows a callback's failure once.
    auto failure = button.on("click", [](const UiEvent &) { throw std::runtime_error("event failure"); });
    button.native().DispatchEvent("click", {});
    CHECK_THROWS_WITH_AS(host.check_events(), "event failure", std::runtime_error);
    CHECK_NOTHROW(host.check_events());
    failure.disconnect();

    int stateful_count = 0;
    auto stateful =
        button.on("click", [count = 0, &stateful_count](const UiEvent &) mutable { stateful_count = ++count; });
    button.native().DispatchEvent("click", {});
    button.native().DispatchEvent("click", {});
    CHECK(stateful_count == 2);
    stateful.disconnect();

    auto shutdown = button.on("click", [&](const UiEvent &) { host.shutdown(); });
    button.native().DispatchEvent("click", {});
    CHECK_THROWS_WITH_AS(host.check_events(), "Cannot shut down UI host during an event callback", std::logic_error);
    CHECK(doc.valid());
    shutdown.disconnect();
}

TEST_CASE("Replacing markup, closing and moving documents invalidate their handles") {
    Session session;
    UiDocuments host(session.context());
    auto doc = host.from_memory(markup);
    auto replaced = doc.element("action");
    auto replacement_listener = replaced.on("click", [](const UiEvent &) {});
    doc.element("container").set_markup("<button id='action'>Replacement</button>");
    CHECK_FALSE(replaced.valid());
    CHECK_FALSE(replacement_listener.connected());
    CHECK_THROWS_WITH_AS(replaced.id(), expired_element, std::out_of_range);

    auto closing = doc.root().on("click", [&](const UiEvent &) { doc.close(); });
    doc.element("action").native().DispatchEvent("click", {});
    host.check_events();
    CHECK_FALSE(doc.valid());
    CHECK_FALSE(closing.connected());
    session.context().Update();

    auto moved = host.from_memory(markup);
    auto node = moved.element("action");
    auto destination = std::move(moved);
    CHECK_FALSE(moved.valid());
    CHECK(destination.valid());
    CHECK(node.valid());
    destination.close();
    CHECK_FALSE(node.valid());
}

#ifdef TEST_UI_SCENE
TEST_CASE("Panels restore through prefabs, follow enablement and activation, and roll back invalid payloads") {
    Session session;
    UiDocuments host(session.context());
    Scene scene;
    ComponentCodecs codecs;
    add_ui_component_codec(codecs, host, [&](std::string_view key) {
        CHECK(key == "controls");
        return controls;
    });
    auto root = scene.create();
    root.add_component<UiPanel>(host, "controls", controls, true);
    auto source = Prefab::capture(root, codecs);
    auto copy = Prefab::deserialize(source.serialize({}), {}, codecs).instantiate(scene);
    {
        // A lone continuation byte is never valid UTF-8, so the codec cannot encode this key.
        auto unencodable = scene.create();
        unencodable.add_component<UiPanel>(host, "\xff", controls, true);
        CHECK_THROWS_WITH_AS(Prefab::capture(unencodable, codecs),
                             "[json.exception.type_error.316] invalid UTF-8 byte at index 0: 0xFF",
                             std::invalid_argument);
        unencodable.destroy();
    }
    CHECK(&root.get_component<UiPanel>()->document().native() != &copy.get_component<UiPanel>()->document().native());

    auto panel = copy.get_component<UiPanel>();
    auto element = panel->document().element("action");
    panel.set_enabled(false);
    sync_ui_panels(scene);
    CHECK_FALSE(panel->document().visible());
    panel.set_enabled(true);
    sync_ui_panels(scene);
    CHECK(panel->document().visible());
    auto parent = scene.create();
    copy.set_parent(parent);
    parent.set_active(false);
    sync_ui_panels(scene);
    CHECK(panel.enabled());
    CHECK_FALSE(panel->document().visible());
    parent.set_active(true);
    sync_ui_panels(scene);
    CHECK(panel->document().visible());
    copy.destroy();
    parent.destroy();
    CHECK_FALSE(element.valid());

    auto nodes = std::vector<Prefab::Node>(source.nodes().begin(), source.nodes().end());
    nodes.push_back(nodes[0]);
    nodes.back().key = {};
    nodes[1].parent = 0;
    const auto count = session.context().GetNumDocuments();
    for (const auto &payload : invalid_payloads(nodes[0].components[0].state)) {
        CAPTURE(payload.state);
        nodes[1].components[0].state = payload.state;
        CHECK_THROWS_WITH_AS(Prefab(nodes, codecs).instantiate(scene), payload.error.c_str(), std::invalid_argument);
        CHECK(scene.size() == 1);
        CHECK(session.context().GetNumDocuments() == count);
    }
}

TEST_CASE("Scene set panel selection reconciles enablement, activation and authored visibility") {
    Session session;
    UiDocuments host(session.context());
    SceneSet scenes;
    auto first = scenes.create("first"), second = scenes.create("second");
    auto object = first->create(), parent = second->create(), child = second->create();
    child.set_parent(parent);
    auto panel = object.add_component<UiPanel>(host, "controls", controls);
    auto other = child.add_component<UiPanel>(host, "controls", controls);
    panel.set_enabled(false);
    parent.set_active(false);
    sync_ui_panels(scenes);
    CHECK_FALSE(panel->document().visible());
    CHECK(other.enabled());
    CHECK_FALSE(other->document().visible());
    panel.set_enabled(true);
    parent.set_active(true);
    other->set_visible(false);
    sync_ui_panels(scenes);
    CHECK(panel->document().visible());
    CHECK_FALSE(other->document().visible());
    other->set_visible(true);
    sync_ui_panels(scenes);
    CHECK(other->document().visible());

    // A later expired document rejects the whole snapshot before earlier panels change.
    panel->set_visible(false);
    other->document().close();
    CHECK_THROWS_WITH_AS(sync_ui_panels(scenes), "UI panel document expired", std::out_of_range);
    CHECK(panel->document().visible());
    // A rejected snapshot must release both the set and each selected scene.
    scenes.update(0);
    first->fixed_update(0);
    auto recovered = scenes.create("recovered");
    scenes.unload(recovered);
    child.remove_component<UiPanel>();
    sync_ui_panels(scenes);
    CHECK_FALSE(panel->document().visible());
    scenes.unload(second);
    sync_ui_panels(scenes);
    scenes.clear();
    sync_ui_panels(scenes);
}

TEST_CASE("Panel drivers reject reentry from UI events and component callbacks") {
    Session session;
    UiDocuments host(session.context());
    PanelDriverChecks checks;
    SceneSet scenes;
    auto first = scenes.create("first"), second = scenes.create("second");
    auto object = first->create();
    auto panel = object.add_component<UiPanel>(host, "controls", controls, false);
    // Whether the set's driver dispatches the event. The set then reports that a scene driver holds it; while one
    // scene's driver dispatches it, the set reports that a member scene is running callbacks instead.
    bool complete_set = true;
    unsigned events = 0;
    const auto attempt = [&](const UiEvent &) {
        ++events;
        CHECK_THROWS_WITH_AS(sync_ui_panels(first.get()), busy_scene, std::logic_error);
        const auto set_error = complete_set ? set_driven : member_callbacks;
        CHECK_THROWS_WITH_AS(sync_ui_panels(scenes), set_error, std::logic_error);
        CHECK_THROWS_WITH_AS(first->update(0), nested_updates, std::logic_error);
        CHECK_THROWS_WITH_AS(scenes.fixed_update(0), set_error, std::logic_error);
        CHECK_THROWS_WITH_AS(scenes.create("nested"), set_error, std::logic_error);
        if (complete_set)
            CHECK_THROWS_WITH_AS(second->update(0), nested_updates, std::logic_error);
    };
    auto show = panel->document().root().on("show", attempt);
    auto hide = panel->document().root().on("hide", attempt);
    panel->set_visible(true);
    sync_ui_panels(scenes);
    host.check_events();
    CHECK(events == 1);
    CHECK(panel->document().visible());
    complete_set = false;
    panel->set_visible(false);
    sync_ui_panels(first.get());
    host.check_events();
    CHECK(events == 2);
    CHECK_FALSE(panel->document().visible());
    scenes.update(0);
    first->fixed_update(0);
    (void)scenes.create("after-events");

    object.add_component<PanelDriverProbe>(first.get(), scenes, checks);
    scenes.update(0);
    scenes.fixed_update(0);
    CHECK(checks.calls == 5);
    CHECK(checks.rejected);
    // Retire the probe while its result storage and borrowed scene still exist.
    object.remove_component<PanelDriverProbe>();
    CHECK(checks.calls == 6);
    CHECK(checks.rejected);
}

TEST_CASE("Visibility events remove, replace, close and add panels safely") {
    Session session;
    UiDocuments host(session.context());
    SUBCASE("A panel removes itself during its show or hide event") {
        for (const bool showing : {true, false}) {
            CAPTURE(showing);
            SceneSet scenes;
            auto scene = scenes.create("self-removal");
            auto object = scene->create();
            auto panel = object.add_component<UiPanel>(host, "controls", controls, !showing);
            auto root = panel->document().root();
            unsigned events = 0;
            auto removal = root.on(showing ? "show" : "hide", [&](const UiEvent &) {
                ++events;
                object.remove_component<UiPanel>();
            });
            panel->set_visible(showing);
            sync_ui_panels(scenes);
            host.check_events();
            CHECK(events == 1);
            CHECK_FALSE(panel);
            CHECK_FALSE(root.valid());
            CHECK_FALSE(removal.connected());
            sync_ui_panels(scenes);
        }
    }
    SUBCASE("Destroying an object during dispatch retires its subtree before its panels are visited") {
        SceneSet scenes;
        auto scene = scenes.create("subtree-removal");
        auto object = scene->create(), child = scene->create();
        child.set_parent(object);
        auto panel = object.add_component<UiPanel>(host, "controls", controls, false);
        auto nested = child.add_component<UiPanel>(host, "controls", controls, false);
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
        CHECK(events == 1);
        CHECK(child_events == 0);
        CHECK(scene->size() == 0);
        CHECK_FALSE(object.valid());
        CHECK_FALSE(child.valid());
        CHECK_FALSE(panel);
        CHECK_FALSE(nested);
        CHECK_FALSE(root.valid());
        CHECK_FALSE(child_root.valid());
        CHECK_FALSE(destruction.connected());
        CHECK_FALSE(skipped.connected());
        sync_ui_panels(scenes);
    }
    SUBCASE("A visibility event removes a later panel") {
        SceneSet scenes;
        auto first = scenes.create("first"), second = scenes.create("second");
        auto object = first->create(), later = second->create();
        auto panel = object.add_component<UiPanel>(host, "controls", controls, false);
        auto other = later.add_component<UiPanel>(host, "controls", controls, false);
        auto removal = panel->document().root().on("show", [&](const UiEvent &) { later.remove_component<UiPanel>(); });
        panel->set_visible(true);
        other->set_visible(true);
        sync_ui_panels(scenes);
        host.check_events();
        CHECK(panel->document().visible());
        CHECK_FALSE(other);
    }
    SUBCASE("A snapshot does not retarget a removed panel to its replacement") {
        SceneSet scenes;
        auto first = scenes.create("first"), second = scenes.create("second");
        auto panel = first->create().add_component<UiPanel>(host, "controls", controls, false);
        auto later = second->create();
        auto old = later.add_component<UiPanel>(host, "controls", controls, false);
        auto old_root = old->document().root();
        ComponentRef<UiPanel> replacement;
        auto replace = panel->document().root().on("show", [&](const UiEvent &) {
            later.remove_component<UiPanel>();
            replacement = later.add_component<UiPanel>(host, "controls", controls, false);
            replacement->set_visible(true);
        });
        panel->set_visible(true);
        old->set_visible(true);
        sync_ui_panels(scenes);
        host.check_events();
        CHECK_FALSE(old);
        CHECK_FALSE(old_root.valid());
        REQUIRE(replacement);
        CHECK_FALSE(replacement->document().visible());
        sync_ui_panels(scenes);
        host.check_events();
        CHECK(replacement->document().visible());
    }
    SUBCASE("A visibility event closes a later document") {
        SceneSet scenes;
        auto first = scenes.create("first"), second = scenes.create("second");
        auto panel = first->create().add_component<UiPanel>(host, "controls", controls);
        auto later = second->create();
        auto other = later.add_component<UiPanel>(host, "controls", controls);
        auto closure = panel->document().root().on("hide", [&](const UiEvent &) { other->document().close(); });
        panel->set_visible(false);
        other->set_visible(false);
        sync_ui_panels(scenes);
        host.check_events();
        CHECK_FALSE(panel->document().visible());
        CHECK(other.valid());
        CHECK_FALSE(other->document().valid());
        later.remove_component<UiPanel>();
        sync_ui_panels(scenes);
    }
    SUBCASE("A panel added during a snapshot joins the next one") {
        SceneSet scenes;
        auto first = scenes.create("first"), second = scenes.create("second");
        auto panel = first->create().add_component<UiPanel>(host, "controls", controls, false);
        ComponentRef<UiPanel> added;
        auto addition = panel->document().root().on("show", [&](const UiEvent &) {
            added = second->create().add_component<UiPanel>(host, "controls", controls, false);
            added->set_visible(true);
        });
        panel->set_visible(true);
        sync_ui_panels(scenes);
        host.check_events();
        REQUIRE(added);
        CHECK_FALSE(added->document().visible());
        sync_ui_panels(scenes);
        host.check_events();
        CHECK(added->document().visible());
    }
}

TEST_CASE("Restoring an invalid UI asset key fails with UiPanel's error before the resolver runs") {
    Session session;
    UiDocuments host(session.context());
    constexpr std::size_t maximum_asset_key_bytes = 4096; // UiPanel's documented key limit.
    std::vector<std::string> resolved;
    ComponentCodecs codecs;
    add_ui_component_codec(codecs, host, [&](std::string_view key) {
        resolved.emplace_back(key);
        return controls;
    });
    Scene authoring;
    auto source = authoring.create();
    source.add_component<UiPanel>(host, "controls", controls, false);
    const auto captured = Prefab::capture(source, codecs);
    auto nodes = std::vector<Prefab::Node>(captured.nodes().begin(), captured.nodes().end());
    Scene scene;
    for (const auto &key : {std::string{}, std::string(maximum_asset_key_bytes + 1, 'k')}) {
        CAPTURE(key.size());
        nodes[0].components[0].state = "{\"asset\":\"" + key + "\",\"visible\":false}";
        const Prefab prefab(nodes, codecs);
        CHECK_THROWS_WITH_AS(prefab.instantiate(scene), "Invalid UI asset key", std::invalid_argument);
        CHECK(resolved.empty());
        CHECK(scene.size() == 0);
    }
}
#endif

TEST_CASE("Handles stay safe after host shutdown and across RmlUi reinitialization") {
    UiElement stale;
    UiDocument survivor;
    UiSubscription surviving_subscription;
    {
        Session session;
        UiDocuments host(session.context());
        survivor = host.from_memory(markup);
        stale = survivor.element("action");
        surviving_subscription = stale.on("click", [](const UiEvent &) {});
        host.shutdown();
        host.shutdown();
        CHECK_FALSE(survivor.valid());
        CHECK_FALSE(stale.valid());
        CHECK_FALSE(surviving_subscription.connected());
        CHECK_THROWS_WITH_AS(host.from_memory(markup), host_shut_down, std::out_of_range);
    }
    // The handles outlive RmlUi's globals, so every backend observer must already have been released.
    Session reinitialized;
    survivor.close();
    surviving_subscription.disconnect();
    CHECK_THROWS_WITH_AS(stale.tag(), expired_element, std::out_of_range);
}

int main(int argc, char **argv) {
    doctest::Context context(argc, argv);
    // ui_documents names the controls document: the first argument that is not a doctest option.
    for (int i = 1; i < argc; ++i)
        if (argv[i][0] != '-') {
            controls = argv[i];
            break;
        }
    if (controls.empty()) {
        std::cerr << "Expected the controls document path\n";
        return 1;
    }
    return context.run();
}
