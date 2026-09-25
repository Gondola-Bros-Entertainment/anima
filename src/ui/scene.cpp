#include "../detail/json.hpp"
#include "../detail/scene_driver.hpp"
#include <anima/ui/scene.hpp>
namespace anima {
UiPanel::UiPanel(UiDocuments &host, std::string key, const std::filesystem::path &path, bool visible)
    : asset_key_(std::move(key)), visible_(visible) {
    if (asset_key_.empty() || asset_key_.size() > 4096)
        throw std::invalid_argument("Invalid UI asset key");
    document_ = host.load(path);
    if (visible_)
        document_.show();
}
void UiPanel::set_visible(bool visible) { visible_ = visible; }
namespace {
template <class Scenes> void synchronize_panels(Scenes &scenes) {
    const detail::SceneDriver::Scope scope(scenes);
    const auto panels = scenes.template components<UiPanel>();
    for (auto component : panels)
        if (!component->document().valid())
            throw std::out_of_range("UI panel document expired");
    for (auto component : panels) {
        // A preceding visibility event may remove another panel or close its
        // document. Pin the current component through events that remove itself.
        if (!component)
            continue;
        auto panel = component.operator->();
        auto &document = panel->document();
        if (!document.valid())
            continue;
        const bool visible = component.active() && panel->visible();
        if (visible != document.visible()) {
            if (visible)
                document.show();
            else
                document.hide();
        }
    }
}
} // namespace
void sync_ui_panels(Scene &scene) { synchronize_panels(scene); }
void sync_ui_panels(SceneSet &scenes) { synchronize_panels(scenes); }
void add_ui_component_codec(ComponentCodecs &codecs, UiDocuments &host, UiDocumentResolver resolve) {
    if (!resolve)
        throw std::invalid_argument("UI codec requires an asset resolver");
    const std::weak_ptr<int> lifetime = host.lifetime_;
    codecs.add<UiPanel>(
        "anima.ui-panel.v1",
        [](const UiPanel &panel, const ObjectReferences &) {
            return nlohmann::json{{"asset", panel.asset_key()}, {"visible", panel.visible()}}.dump();
        },
        [&host, lifetime, resolve = std::move(resolve)](GameObject object, std::string_view data,
                                                        const ObjectReferences &) {
            if (lifetime.expired())
                throw std::out_of_range("UI codec host expired");
            const auto json = detail::parse_json(data, 8192);
            detail::json_fields(json, {"asset", "visible"});
            if (!json.at("asset").is_string() || !json.at("visible").is_boolean())
                throw std::invalid_argument("Invalid UI panel fields");
            const auto key = json.at("asset").get<std::string>();
            object.add_component<UiPanel>(host, key, resolve(key), json.at("visible").get<bool>());
        });
}
} // namespace anima
